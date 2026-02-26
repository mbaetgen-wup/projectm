/**
 * projectM -- Milkdrop-esque visualisation SDK
 * Copyright (C)2003-2004 projectM Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * See 'LICENSE.txt' included within this release
 *
 */

#include "MilkdropPreset.hpp"

#include "Factory.hpp"
#include "MilkdropPresetExceptions.hpp"
#include "PresetFileParser.hpp"

#include <Logging.hpp>
#include <Renderer/TextureManager.hpp>

#include <chrono>
#include <thread>

namespace libprojectM {
namespace MilkdropPreset {

MilkdropPreset::MilkdropPreset(const std::string& absoluteFilePath)
    : m_absoluteFilePath(absoluteFilePath)
    , m_perFrameContext(m_state.globalMemory, &m_state.globalRegisters)
    , m_perPixelContext(m_state.globalMemory, &m_state.globalRegisters)
    , m_motionVectors(m_state)
    , m_waveform(m_state)
    , m_darkenCenter(m_state)
    , m_border(m_state)
{
    Load(absoluteFilePath);
}

MilkdropPreset::MilkdropPreset(std::istream& presetData)
    : m_perFrameContext(m_state.globalMemory, &m_state.globalRegisters)
    , m_perPixelContext(m_state.globalMemory, &m_state.globalRegisters)
    , m_motionVectors(m_state)
    , m_waveform(m_state)
    , m_darkenCenter(m_state)
    , m_border(m_state)
{
    Load(presetData);
}

void MilkdropPreset::CompileExpressions()
{
    // Called by the CPU worker thread.  Always compile — even if
    // m_expressionsCompiled was set to true by the render thread
    // (which only uses that flag to skip Phase 0's inline path).

    // Transpile shader code (HLSL→GLSL) first.
    // This is CPU-only string transformation work.  The shader objects
    // were already created by the constructor on the GL thread;
    // we just do the expensive transpile step here.
    if (!m_shadersTranspiled)
    {
        TranspileShaders();
        m_shadersTranspiled = true;
    }

    CompileCodeAndRunInitExpressions();
    m_expressionsCompiled = true;
}

void MilkdropPreset::SetExpressionsCompiled(bool compiled)
{
    m_expressionsCompiled = compiled;
}

void MilkdropPreset::Initialize(const Renderer::RenderContext& renderContext)
{
    // Monolithic path: run all initialization synchronously.
    auto t0 = std::chrono::steady_clock::now();

    // Phase 0: setup.
    assert(renderContext.textureManager);
    m_state.renderContext = renderContext;
    m_state.blurTexture.Initialize(renderContext);
    m_state.LoadShaders();

    auto t1 = std::chrono::steady_clock::now();

    // Skip if already done on CPU worker thread.
    if (!m_expressionsCompiled)
    {
        // Transpile + expressions together (synchronous path).
        TranspileShaders();
        m_shadersTranspiled = true;
        CompileCodeAndRunInitExpressions();
        m_expressionsCompiled = true;
    }
    else if (!m_shadersTranspiled)
    {
        TranspileShaders();
        m_shadersTranspiled = true;
    }

    auto t2 = std::chrono::steady_clock::now();

    m_framebuffer.SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY);
    m_motionVectorUVMap->SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY);
    if (m_state.mainTexture.expired())
    {
        m_state.mainTexture = m_framebuffer.GetColorAttachmentTexture(1, 0);
    }

    auto t3 = std::chrono::steady_clock::now();

    // Use synchronous compilation — no async submit/poll/finalize dance.
    m_perPixelMesh.CompileWarpShader(m_state);

    auto t4 = std::chrono::steady_clock::now();

    m_finalComposite.CompileCompositeShader(m_state);

    auto t5 = std::chrono::steady_clock::now();

    SetInitialized();

    auto us = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    LOG_INFO("[MilkdropPreset::Initialize] setup=" + std::to_string(us(t0, t1) / 1000.0f) +
             "ms expr=" + std::to_string(us(t1, t2) / 1000.0f) +
             "ms fbo=" + std::to_string(us(t2, t3) / 1000.0f) +
             "ms warp=" + std::to_string(us(t3, t4) / 1000.0f) +
             "ms comp=" + std::to_string(us(t4, t5) / 1000.0f) +
             "ms total=" + std::to_string(us(t0, t5) / 1000.0f) + "ms");
}

void MilkdropPreset::InitializePhase(const Renderer::RenderContext& renderContext, int phase)
{
    switch (phase)
    {
        case 0:
        {
            // Setup: render context, blur textures, static shaders,
            // expression compilation, framebuffer allocation.
            assert(renderContext.textureManager);
            m_state.renderContext = renderContext;
            m_state.blurTexture.Initialize(renderContext);
            m_state.LoadShaders();

            // Skip if already done on CPU worker thread.
            if (!m_expressionsCompiled)
            {
                CompileCodeAndRunInitExpressions();
                m_expressionsCompiled = true;
            }

            m_framebuffer.SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY);
            m_motionVectorUVMap->SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY);
            if (m_state.mainTexture.expired())
            {
                m_state.mainTexture = m_framebuffer.GetColorAttachmentTexture(1, 0);
            }
            break;
        }
        case 1:
            // Submit BOTH shaders for async compilation in the same frame.
            // With GL_KHR_parallel_shader_compile the driver compiles them
            // in parallel on background threads.  Without the extension,
            // SubmitCompileAsync falls back to synchronous compilation.
            m_perPixelMesh.CompileWarpShaderAsync(m_state);
            m_finalComposite.CompileCompositeShaderAsync(m_state);
            break;

        case 2:
            // Finalize compilation — check results and clean up.
            // StageGlWork has already polled IsPhaseComplete(1) until both
            // shaders reported GL_COMPLETION_STATUS_KHR == GL_TRUE.
            m_perPixelMesh.FinalizeWarpShaderCompile();
            m_finalComposite.FinalizeCompositeShaderCompile();
            SetInitialized();
            break;

        default:
            break;
    }
}

bool MilkdropPreset::IsPhaseComplete(int phase) const
{
    switch (phase)
    {
        case 1:
            // Phase 1 submitted async shader compiles.
            // Poll completion status without blocking.
            return m_perPixelMesh.IsWarpShaderCompileComplete()
                && m_finalComposite.IsCompositeShaderCompileComplete();

        default:
            return true;
    }
}

void MilkdropPreset::RenderFrame(const libprojectM::Audio::FrameAudioData& audioData, const Renderer::RenderContext& renderContext)
{
    m_state.audioData = audioData;
    m_state.renderContext = renderContext;

    // Update framebuffer and u/v texture size if needed
    if (m_framebuffer.SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY))
    {
        m_motionVectorUVMap->SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY);
        m_isFirstFrame = true;
    }

    m_state.mainTexture = m_framebuffer.GetColorAttachmentTexture(m_previousFrameBuffer, 0);

    // First evaluate per-frame code
    PerFrameUpdate();

    glViewport(0, 0, renderContext.viewportSizeX, renderContext.viewportSizeY);

    m_framebuffer.Bind(m_previousFrameBuffer);
    // Motion vector field. Drawn to the previous frame texture before warping it.
    // Only do it after drawing one frame after init or resize.
    if (!m_isFirstFrame)
    {
        m_motionVectors.Draw(m_perFrameContext, m_motionVectorUVMap->Texture());
    }

    // y-flip the previous frame and assign the flipped texture as "main"
    m_flipTexture.Draw(*renderContext.shaderCache, m_framebuffer.GetColorAttachmentTexture(m_previousFrameBuffer, 0), nullptr, true, false);
    m_state.mainTexture = m_flipTexture.Texture();

    // We now draw to the current framebuffer.
    m_framebuffer.Bind(m_currentFrameBuffer);

    // Add motion vector u/v texture for the warp mesh draw and clean both buffers.
    m_framebuffer.SetAttachment(m_currentFrameBuffer, 1, m_motionVectorUVMap);

    // Draw previous frame image warped via per-pixel mesh and warp shader
    m_perPixelMesh.Draw(m_state, m_perFrameContext, m_perPixelContext);

    // Remove the u/v texture from the framebuffer.
    m_framebuffer.RemoveColorAttachment(m_currentFrameBuffer, 1);

    // Update blur textures
    {
        const auto warpedImage = m_framebuffer.GetColorAttachmentTexture(m_previousFrameBuffer, 0);
        assert(warpedImage.get());
        m_state.blurTexture.Update(*warpedImage, m_perFrameContext);
    }

    // Draw audio-data-related stuff
    for (auto& shape : m_customShapes)
    {
        shape->Draw();
    }
    for (auto& wave : m_customWaveforms)
    {
        wave->Draw(m_perFrameContext);
    }
    m_waveform.Draw(m_perFrameContext);

    // Done in DrawSprites() in Milkdrop
    if (*m_perFrameContext.darken_center > 0)
    {
        m_darkenCenter.Draw();
    }
    m_border.Draw(m_perFrameContext);

    // y-flip the image for final compositing again
    m_flipTexture.Draw(*renderContext.shaderCache, m_framebuffer.GetColorAttachmentTexture(m_currentFrameBuffer, 0), nullptr, true, false);
    m_state.mainTexture = m_flipTexture.Texture();

    // We no longer need the previous frame image, use it to render the final composite.
    m_framebuffer.BindRead(m_currentFrameBuffer);
    m_framebuffer.BindDraw(m_previousFrameBuffer);

    m_finalComposite.Draw(m_state, m_perFrameContext);

    if (!m_finalComposite.HasCompositeShader())
    {
        // Flip texture again in "previous" framebuffer as old-school effects are still upside down.
        m_flipTexture.Draw(*renderContext.shaderCache, m_framebuffer.GetColorAttachmentTexture(m_previousFrameBuffer, 0), m_framebuffer, m_previousFrameBuffer, true, false);
    }

    // Swap framebuffer IDs for the next frame.
    std::swap(m_currentFrameBuffer, m_previousFrameBuffer);

    m_isFirstFrame = false;
}

auto MilkdropPreset::OutputTexture() const -> std::shared_ptr<Renderer::Texture>
{
    // the composited image is always stored in the "current" framebuffer after a frame is rendered.
    return m_framebuffer.GetColorAttachmentTexture(m_currentFrameBuffer, 0);
}

void MilkdropPreset::DrawInitialImage(const std::shared_ptr<Renderer::Texture>& image, const Renderer::RenderContext& renderContext)
{
    m_framebuffer.SetSize(renderContext.viewportSizeX, renderContext.viewportSizeY);

    // Render to previous framebuffer, as this is the image used to draw the next frame on.
    m_flipTexture.Draw(*renderContext.shaderCache, image, m_framebuffer, m_previousFrameBuffer);
}

void MilkdropPreset::BindFramebuffer()
{
    if (m_framebuffer.Width() > 0 && m_framebuffer.Height() > 0)
    {
        m_framebuffer.BindDraw(m_previousFrameBuffer);
    }
}

void MilkdropPreset::PerFrameUpdate()
{
    m_perFrameContext.LoadStateVariables(m_state);
    m_perPixelContext.LoadStateReadOnlyVariables(m_state, m_perFrameContext);

    m_perFrameContext.ExecutePerFrameCode();

    m_perPixelContext.LoadPerFrameQVariables(m_state, m_perFrameContext);

    // Clamp gamma and echo zoom values
    *m_perFrameContext.gamma = std::max(0.0, std::min(8.0, *m_perFrameContext.gamma));
    *m_perFrameContext.echo_zoom = std::max(0.001, std::min(1000.0, *m_perFrameContext.echo_zoom));
}

void MilkdropPreset::Load(const std::string& pathname)
{
    LOG_DEBUG("[MilkdropPreset] Loading preset from file \"" + pathname + "\".")

    SetFilename(ParseFilename(pathname));

    PresetFileParser parser;

    if (!parser.Read(pathname))
    {
        const std::string error = "[MilkdropPreset] Could not parse preset file \"" + pathname + "\".";
        LOG_ERROR(error)
        throw MilkdropPresetLoadException(error);
    }

    InitializePreset(parser);
}

void MilkdropPreset::Load(std::istream& stream)
{
    LOG_DEBUG("[MilkdropPreset] Loading preset from stream.");

    PresetFileParser parser;

    if (!parser.Read(stream))
    {
        const std::string error =  "[MilkdropPreset] Could not parse preset data.";
        LOG_ERROR(error)
        throw MilkdropPresetLoadException(error);
    }

    InitializePreset(parser);
}

void MilkdropPreset::InitializePreset(PresetFileParser& parsedFile)
{
    // Create the offscreen rendering surfaces.
    m_motionVectorUVMap = std::make_shared<Renderer::TextureAttachment>(GL_RG16F, GL_RG, GL_FLOAT, 0, 0);
    m_framebuffer.CreateColorAttachment(0, 0); // Main image 1
    m_framebuffer.CreateColorAttachment(1, 0); // Main image 2

    Renderer::Framebuffer::Unbind();

    // Load global init variables into the state
    m_state.Initialize(parsedFile);

    // Register code context variables
    m_perFrameContext.RegisterBuiltinVariables();
    m_perPixelContext.RegisterBuiltinVariables();

    // Custom waveforms:
    for (int i = 0; i < CustomWaveformCount; i++)
    {
        auto wave = std::make_unique<CustomWaveform>(m_state);
        wave->Initialize(parsedFile, i);
        m_customWaveforms[i] = std::move(wave);
    }

    // Custom shapes:
    for (int i = 0; i < CustomShapeCount; i++)
    {
        auto shape = std::make_unique<CustomShape>(m_state);
        shape->Initialize(parsedFile, i);
        m_customShapes[i] = std::move(shape);
    }

    // Create shader objects and load HLSL code from preset state.
    // This must run on the GL thread because LoadCompositeShader may
    // create VideoEcho/Filters objects that allocate GL vertex buffers.
    // The HLSL→GLSL transpile step is deferred to TranspileShaders()
    // which runs on the CPU worker thread.
    m_perPixelMesh.LoadWarpShader(m_state);
    m_finalComposite.LoadCompositeShader(m_state);
}

void MilkdropPreset::CompileCodeAndRunInitExpressions()
{
    // Per-frame init and code
    m_perFrameContext.LoadStateVariables(m_state);
    m_perFrameContext.EvaluateInitCode(m_state);
    m_perFrameContext.CompilePerFrameCode(m_state.perFrameCode);

    // Per-vertex code
    m_perPixelContext.CompilePerPixelCode(m_state.perPixelCode);

    for (int i = 0; i < CustomWaveformCount; i++)
    {
        auto& wave = m_customWaveforms[i];
        wave->CompileCodeAndRunInitExpressions(m_perFrameContext);
    }

    for (int i = 0; i < CustomShapeCount; i++)
    {
        auto& shape = m_customShapes[i];
        shape->CompileCodeAndRunInitExpressions();
    }
}

void MilkdropPreset::LoadShaderCode()
{
    m_perPixelMesh.LoadWarpShader(m_state);
    m_finalComposite.LoadCompositeShader(m_state);

    // Pre-transpile shaders from HLSL to GLSL now (CPU-only work).
    // This avoids a stall on the render thread later during Initialize()
    // when CompileWarpShader/CompileCompositeShader are called.
    m_perPixelMesh.TranspileWarpShader();
    m_finalComposite.TranspileCompositeShader();
}

void MilkdropPreset::TranspileShaders()
{
    // Pure CPU string transformation — safe to call from any thread.
    // Must be called after LoadWarpShader/LoadCompositeShader have
    // created the MilkdropShader objects (which happens in the
    // constructor on the GL thread).
    m_perPixelMesh.TranspileWarpShader();
    m_finalComposite.TranspileCompositeShader();
}

void MilkdropPreset::PreloadTextures(Renderer::TextureManager* textureManager)
{
    if (!textureManager)
    {
        return;
    }

    // Collect sampler names from both shaders.
    std::set<std::string> allSamplers;

    auto warpSamplers = m_perPixelMesh.GetWarpSamplerNames();
    allSamplers.insert(warpSamplers.begin(), warpSamplers.end());

    auto compositeSamplers = m_finalComposite.GetCompositeSamplerNames();
    allSamplers.insert(compositeSamplers.begin(), compositeSamplers.end());

    if (!allSamplers.empty())
    {
        textureManager->PreloadTexturesForSamplers(allSamplers);
    }
}

auto MilkdropPreset::ParseFilename(const std::string& filename) -> std::string
{
    const std::size_t start = filename.find_last_of('/');

    if (start == std::string::npos || start >= (filename.length() - 1))
    {
        return "";
    }

    return filename.substr(start + 1, filename.length());
}


} // namespace MilkdropPreset
} // namespace libprojectM
