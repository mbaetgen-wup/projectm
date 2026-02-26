#pragma once

#include <Audio/FrameAudioData.hpp>

#include <Renderer/RenderContext.hpp>
#include <Renderer/Texture.hpp>

#include <memory>
#include <string>

namespace libprojectM {

namespace Renderer {
class TextureManager;
} // namespace Renderer

class Preset
{
public:
    virtual ~Preset() = default;

    /**
     * @brief Pre-compiles CPU-only expression bytecode.
     *
     * This is pure CPU work (no GL dependency) that can safely be called
     * on any thread before Initialize().  If called, Initialize() / phase 0
     * will skip expression compilation, saving ~5-7ms on the render thread.
     *
     * The default implementation is a no-op.
     */
    virtual void CompileExpressions() {}

    /**
     * @brief Pre-decodes texture image files needed by this preset.
     *
     * Pure CPU work (stbi_load) — safe to call from any thread.
     * Must be called after the constructor (which populates sampler names).
     * The TextureManager stores the decoded pixel data; the subsequent
     * GL thread phase will just upload to GPU without disk I/O.
     *
     * @param textureManager The texture manager to preload into.
     */
    virtual void PreloadTextures(Renderer::TextureManager* textureManager) { (void)textureManager; }

    /**
     * @brief Marks expression compilation as done (or to be skipped).
     *
     * Called by the render thread before running InitializePhase(0) when
     * expression compilation has been submitted to the CPU worker thread.
     * This prevents Phase 0 from redundantly compiling expressions inline.
     */
    virtual void SetExpressionsCompiled(bool compiled) { (void)compiled; }

    /**
     * @brief Initializes additional preset resources.
     * @param renderContext A render context with the initial data.
     */
    virtual void Initialize(const Renderer::RenderContext& renderContext) = 0;

    /**
     * @brief Phased initialization for spreading GL work across frames.
     *
     * Returns the total number of phases.  The caller should call
     * InitializePhase(renderContext, 0), then InitializePhase(renderContext, 1),
     * etc., each on a separate frame, until phase == PhaseCount().
     *
     * The default implementation calls Initialize() in phase 0.
     */
    virtual int InitializePhaseCount() const { return 1; }

    /**
     * @brief Executes a single initialization phase.
     * @param renderContext A render context with the initial data.
     * @param phase The phase index (0-based).
     */
    virtual void InitializePhase(const Renderer::RenderContext& renderContext, int phase)
    {
        if (phase == 0)
        {
            Initialize(renderContext);
        }
    }

    /**
     * @brief Checks whether the current phase has completed its async work.
     *
     * Some phases may submit work (e.g. shader compilation) that completes
     * asynchronously.  The caller should poll this method each frame after
     * calling InitializePhase() and only advance to the next phase once
     * it returns true.
     *
     * @param phase The phase index to check.
     * @return true if the phase is complete (default: always true).
     */
    virtual bool IsPhaseComplete(int phase) const
    {
        (void)phase;
        return true;
    }

    /**
     * @brief Returns whether Initialize() has been called successfully.
     * @return True if the preset has been initialized.
     */
    bool IsInitialized() const { return m_initialized; }

protected:
    /**
     * @brief Sets the initialized flag.  Call from Initialize() implementations.
     */
    void SetInitialized() { m_initialized = true; }

public:
    /**
     * @brief Renders the preset into the current framebuffer.
     * @param audioData Audio data to be used by the preset.
     * @param renderContext The current render context data.
     */
    virtual void RenderFrame(const libprojectM::Audio::FrameAudioData& audioData,
                             const Renderer::RenderContext& renderContext) = 0;

    /**
     * @brief Returns a pointer to the current rendering output texture.
     * This pointer (the actual texture) may change from frame to frame, so this pointer should not be stored for use
     * across multiple frames. Instead, a new pointer should be requested whenever needed.
     * @return A pointer to the current output texture of the preset.
     */
    virtual auto OutputTexture() const -> std::shared_ptr<Renderer::Texture> = 0;

    /**
     * @brief Draws an initial image into the preset, e.g. the last frame of a previous preset.
     * It's not guaranteed a preset supports using a previously rendered image. If not
     * supported, this call is simply a no-op.
     * @param image The texture to be copied as the initial preset image.
     * @param renderContext The current render context data.
     */
    virtual void DrawInitialImage(const std::shared_ptr<Renderer::Texture>& image,
                                  const Renderer::RenderContext& renderContext) = 0;

    /**
     * @brief Bind the preset's internal framebuffer.
     * This framebuffer contains the image passed on to the next frame, onto which warp effects etc.
     * are then applied. Has no effect if the framebuffer isn't initialized yet, e.g. before drawing
     * the first frame.
     *
     * Can be used to draw anything on top of the preset's image, "burning in" additional shapes,
     * images or text. Depending on the preset, it's not guaranteed though that the image actually
     * is used in the next frame, or completely painted over. That said, the effect varies between
     * presets.
     */
    virtual void BindFramebuffer() = 0;

    inline void SetFilename(const std::string& filename)
    {
        m_filename = filename;
    }

    inline auto Filename() const -> const std::string&
    {
        return m_filename;
    }

private:
    std::string m_filename;
    bool m_initialized{false};
};

} // namespace libprojectM
