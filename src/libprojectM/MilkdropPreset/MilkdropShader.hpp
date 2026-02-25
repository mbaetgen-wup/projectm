/**
 * @file MilkdropShader.hpp
 * @brief Holds a warp or composite shader of Milkdrop presets.
 *
 * This class wraps the conversion from HLSL shader code to GLSL and also manages the
 * drawing.
 */
#pragma once

#include "BlurTexture.hpp"

#include <Renderer/Shader.hpp>
#include <Renderer/TextureManager.hpp>

#include <array>
#include <set>

namespace libprojectM {
namespace MilkdropPreset {

class PerFrameContext;
class PresetState;

/**
 * @brief Holds a warp or composite shader of Milkdrop presets.
 * Also does the required shader translation from HLSL to GLSL using hlslparser.
 */
class MilkdropShader
{
public:
    enum class ShaderType
    {
        WarpShader,     //!< Warp shader
        CompositeShader //!< Composite shader
    };

    /**
     * constructor.
     * @param type The preset shader type.
     */
    explicit MilkdropShader(ShaderType type);

    /**
     * @brief Translates and compiles the shader code.
     * @param presetShaderCode The preset shader code.
     */
    void LoadCode(const std::string& presetShaderCode);

    /**
     * @brief Performs HLSL to GLSL transpilation using only sampler names.
     *
     * This is a CPU-only operation that does not require a GL context.
     * It generates stub uniform declarations from @c m_samplerNames (populated
     * by LoadCode()), runs the HLSL parser and GLSL generator, and stores the
     * resulting fragment shader source in @c m_transpiledFragmentShader.
     *
     * Should be called after LoadCode() and before LoadTexturesAndCompile().
     * If called, LoadTexturesAndCompile() will skip transpilation and use the
     * cached result directly.
     */
    void TranspilePresetCode();

    /**
     * @brief Loads the required texture references into the shader.
     * Binds the underlying shader program.
     * @param presetState The preset state to pull the values and textures from.
     */
    void LoadTexturesAndCompile(PresetState& presetState);

    /**
     * @brief Loads textures and submits shader for async compilation.
     *
     * Same as LoadTexturesAndCompile(), but uses SubmitCompileAsync()
     * instead of CompileProgram().  The caller should poll
     * IsCompileComplete() and call FinalizeCompile() on the returned
     * Shader before using it.
     *
     * @param presetState The preset state to pull the values and textures from.
     */
    void LoadTexturesAndCompileAsync(PresetState& presetState);

    /**
     * @brief Polls whether an async shader compile is complete.
     * @return true if no async compile is in flight or if it has finished.
     */
    auto IsCompileComplete() const -> bool;

    /**
     * @brief Finalizes an async shader compile, checking results.
     * @throws ShaderException if compilation or linking failed.
     */
    void FinalizeCompile();

    /**
     * @brief Loads all required shader variables into the uniforms.
     * Binds the underlying shader program.
     * @param presetState The preset state to pull the values from.
     * @param perFrameContext The per-frame context with dynamically calculated values.
     */
    void LoadVariables(const PresetState& presetState, const PerFrameContext& perFrameContext);

    /**
     * @brief Returns the contained shader.
     * @return The shader program wrapper.
     */
    auto Shader() -> Renderer::Shader&;

private:
    /**
     * @brief Prepares the shader code to be translated into GLSL.
     * @param program The program code to work on.
     */
    void PreprocessPresetShader(std::string& program);

    /**
     * @brief Searches for sampler references in the program and stores them in m_samplerNames.
     * @param program The program code to work on.
     */
    void GetReferencedSamplers(const std::string& program);

    /**
     * @brief Translates the HLSL shader into GLSL.
     * @param presetState The preset state to pull the blur textures from.
     * @param program The shader to transpile.
     */
    void TranspileHLSLShader(const PresetState& presetState, std::string& program);

    /**
     * @brief Translates HLSL to GLSL and submits for async compilation.
     * @param presetState The preset state to pull the blur textures from.
     * @param program The shader to transpile.
     */
    void TranspileHLSLShaderAsync(const PresetState& presetState, std::string& program);

    /**
     * @brief Generates HLSL uniform declarations from sampler names alone.
     *
     * Uses sampler2D for all textures except known 3D noise volumes.
     * This allows transpilation to proceed without loading actual texture
     * objects (which require a GL context).
     *
     * @param[out] samplerDeclarations Set to receive sampler declarations.
     * @param[out] texSizeDeclarations Set to receive texsize declarations.
     */
    void GenerateStubDeclarations(std::set<std::string>& samplerDeclarations,
                                  std::set<std::string>& texSizeDeclarations) const;

    /**
     * @brief Updates the requested blur level if higher than before.
     * Also adds the required samplers.
     * @param requestedLevel The requested blur level.
     */
    void UpdateMaxBlurLevel(BlurTexture::BlurLevel requestedLevel);

    ShaderType m_type{ShaderType::WarpShader}; //!< Type of this shader.
    std::string m_fragmentShaderCode;          //!< The original preset fragment shader code.
    std::string m_preprocessedCode;            //!< The preprocessed preset shader code.
    std::string m_transpiledFragmentShader;    //!< GLSL fragment shader from TranspilePresetCode(). Empty if not yet transpiled.

    std::set<std::string> m_samplerNames;                                        //!< All sampler names referenced in the shader code.
    std::vector<Renderer::TextureSamplerDescriptor> m_mainTextureDescriptors;              //!< Descriptors for all main texture references.
    std::vector<Renderer::TextureSamplerDescriptor> m_textureSamplerDescriptors;           //!< Descriptors of all referenced samplers in the shader code.
    BlurTexture::BlurLevel m_maxBlurLevelRequired{BlurTexture::BlurLevel::None}; //!< Max blur level of main texture required by this shader.

    std::array<float, 4> m_randValues{};               //!< Random values which don't change every frame.
    std::array<glm::vec3, 20> m_randTranslation{};     //!< Random translation vectors which don't change every frame.
    std::array<glm::vec3, 20> m_randRotationCenters{}; //!< Random rotation center vectors which don't change every frame.
    std::array<glm::vec3, 20> m_randRotationSpeeds{};  //!< Random rotation speeds which don't change every frame.

    Renderer::Shader m_shader;
};

} // namespace MilkdropPreset
} // namespace libprojectM
