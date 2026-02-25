/**
 * @file Shader.hpp
 * @brief Implements an interface to a single shader program instance.
 */
#pragma once

#include "Renderer/Texture.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat3x4.hpp>
#include <glm/mat4x4.hpp>

#include <map>
#include <string>

namespace libprojectM {
namespace Renderer {

/**
 * @brief Shader compilation exception.
 */
class ShaderException : public std::exception
{
public:
    ShaderException(std::string message)
        : m_message(std::move(message))
    {
    }

    virtual ~ShaderException() = default;

    const char* what() const noexcept override
    {
        return m_message.c_str();
    }

    const std::string& message() const
    {
        return m_message;
    }

private:
    std::string m_message;
};


/**
 * @brief Base class containing a shader program, consisting of a vertex and fragment shader.
 */
class Shader
{
public:
    /**
     * GLSL version structure
     */
    struct GlslVersion {
        int major{}; //!< Major OpenGL shading language version
        int minor{}; //!< Minor OpenGL shading language version
    };

    /**
     * Creates a new shader.
     */
    Shader();

    /**
     * Destructor.
     */
    ~Shader();

    /**
     * @brief Compiles a vertex and fragment shader into a program (blocking).
     * @throws ShaderException Thrown if compilation of a shader or program linking failed.
     * @param vertexShaderSource The vertex shader source.
     * @param fragmentShaderSource The fragment shader source.
     */
    void CompileProgram(const std::string& vertexShaderSource,
                        const std::string& fragmentShaderSource);

    /**
     * @brief Submits vertex and fragment shaders for asynchronous compilation.
     *
     * When GL_KHR_parallel_shader_compile is available, the driver compiles
     * shaders on background threads.  The caller should poll IsCompileComplete()
     * on subsequent frames and call FinalizeCompile() once it returns true.
     *
     * If the extension is not available, this behaves identically to
     * CompileProgram() — compilation happens synchronously and
     * IsCompileComplete() will return true immediately.
     *
     * @throws ShaderException Thrown if shader creation or source upload fails.
     * @param vertexShaderSource The vertex shader source.
     * @param fragmentShaderSource The fragment shader source.
     */
    void SubmitCompileAsync(const std::string& vertexShaderSource,
                            const std::string& fragmentShaderSource);

    /**
     * @brief Polls whether an async compile/link submitted via SubmitCompileAsync() is done.
     *
     * Uses GL_COMPLETION_STATUS_KHR to check without blocking.
     * Returns true if:
     *   - No async compilation is pending (nothing was submitted, or already finalized).
     *   - The extension is not available (compile happened synchronously in SubmitCompileAsync).
     *   - Both shader compilation and program linking have completed.
     *
     * @return true if compilation is complete or no async work is pending.
     */
    auto IsCompileComplete() const -> bool;

    /**
     * @brief Finalizes an async compile, checking results and cleaning up.
     *
     * Must be called after IsCompileComplete() returns true.
     * Checks compile/link status and throws on failure (same as CompileProgram).
     *
     * @throws ShaderException Thrown if compilation or linking failed.
     */
    void FinalizeCompile();

    /**
     * @brief Validates that the program can run in the current state.
     * @param validationMessage The error message if validation failed.
     * @return true if the shader program is valid and can run, false if it broken.
     */
    bool Validate(std::string& validationMessage) const;

    /**
     * Binds the program into the current context.
     */
    void Bind() const;

    /**
     * Unbinds the program.
     */
    static void Unbind();

    /**
     * @brief Sets a single float uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param value The value to set.
     */
    void SetUniformFloat(const char* uniform, float value) const;

    /**
     * @brief Sets a single integer uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param value The value to set.
     */
    void SetUniformInt(const char* uniform, int value) const;

    /**
     * @brief Sets a float vec2 uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The values to set.
     */
    void SetUniformFloat2(const char* uniform, const glm::vec2& values) const;

    /**
     * @brief Sets an int vec2 uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The values to set.
     */
    void SetUniformInt2(const char* uniform, const glm::ivec2& values) const;

    /**
     * @brief Sets a float vec3 uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The values to set.
     */
    void SetUniformFloat3(const char* uniform, const glm::vec3& values) const;

    /**
     * @brief Sets an int vec3 uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The values to set.
     */
    void SetUniformInt3(const char* uniform, const glm::ivec3& values) const;

    /**
     * @brief Sets a float vec4 uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The values to set.
     */
    void SetUniformFloat4(const char* uniform, const glm::vec4& values) const;

    /**
     * @brief Sets an int vec4 uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The values to set.
     */
    void SetUniformInt4(const char* uniform, const glm::ivec4& values) const;

    /**
     * @brief Sets a float 3x4 matrix uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The matrix to set.
     */
    void SetUniformMat3x4(const char* uniform, const glm::mat3x4& values) const;

    /**
     * @brief Sets a float 4x4 matrix uniform.
     * The program must be bound before calling this method!
     * @param uniform The uniform name
     * @param values The matrix to set.
     */
    void SetUniformMat4x4(const char* uniform, const glm::mat4x4& values) const;

    /**
     * @brief Parses the shading language version string returned from OpenGL.
     * If this function does not return a good version (e.g. "major" not >0), then OpenGL is probably
     * not properly initialized or the context not made current.
     * @return The parsed version, or {0,0} if the version could not be parsed.
     */
    static auto GetShaderLanguageVersion() -> GlslVersion;

private:
    /**
     * @brief Compiles a single shader.
     * @throws ShaderException Thrown if compilation of the shader failed.
     * @param source The shader source.
     * @param type The shader type, e.g. GL_VERTEX_SHADER.
     * @return The shader ID.
     */
    auto CompileShader(const std::string& source, GLenum type) -> GLuint;

    /**
     * @brief Creates and submits a shader for compilation without checking results.
     * @param source The shader source.
     * @param type The shader type.
     * @return The shader ID.
     */
    auto SubmitShader(const std::string& source, GLenum type) -> GLuint;

    /**
     * @brief Checks compile status of a shader and throws on failure.
     * @throws ShaderException on compilation error.
     * @param shader The shader ID.
     * @param source The original source (for error reporting).
     * @param type The shader type.
     */
    void CheckShaderCompileStatus(GLuint shader, const std::string& source, GLenum type);

    /**
     * @brief Checks link status and throws on failure.
     * @throws ShaderException on link error.
     * @param vertexSource The vertex source (for error reporting).
     * @param fragmentSource The fragment source (for error reporting).
     */
    void CheckLinkStatus(const std::string& vertexSource, const std::string& fragmentSource);

    /**
     * @brief Advances async state from CompilingShaders to LinkingProgram.
     *
     * Checks shader compile results, attaches, and submits link.
     * Called internally by IsCompileComplete() when both shaders are done.
     */
    void AdvanceToLinking();

    /**
     * @brief Async compilation state tracking.
     */
    enum class AsyncState : uint8_t
    {
        None,              //!< No async compilation pending.
        CompilingShaders,  //!< Shaders submitted, waiting for compile completion.
        LinkingProgram,    //!< Shaders compiled, program link submitted, waiting for completion.
        Complete           //!< Link complete, ready for FinalizeCompile().
    };

    GLuint m_shaderProgram{}; //!< The program ID.

    // Async state
    AsyncState m_asyncState{AsyncState::None};
    GLuint m_asyncVertexShader{};              //!< Vertex shader ID during async compile.
    GLuint m_asyncFragmentShader{};            //!< Fragment shader ID during async compile.
    std::string m_asyncVertexSource;           //!< Kept for error reporting.
    std::string m_asyncFragmentSource;         //!< Kept for error reporting.
    bool m_asyncParallelAvailable{false};      //!< Whether extension was available at submit time.
};

} // namespace Renderer
} // namespace libprojectM
