#include "Shader.hpp"

#include "Platform/ParallelShaderProbe.hpp"

#include <Logging.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vector>

namespace libprojectM {
namespace Renderer {

Shader::Shader()
    : m_shaderProgram(glCreateProgram())
{
}

Shader::~Shader()
{
    // Clean up any in-flight async shaders.
    if (m_asyncVertexShader)
    {
        glDeleteShader(m_asyncVertexShader);
    }
    if (m_asyncFragmentShader)
    {
        glDeleteShader(m_asyncFragmentShader);
    }
    if (m_shaderProgram)
    {
        glDeleteProgram(m_shaderProgram);
    }
}

void Shader::CompileProgram(const std::string& vertexShaderSource,
                            const std::string& fragmentShaderSource)
{
    auto vertexShader = CompileShader(vertexShaderSource, GL_VERTEX_SHADER);
    auto fragmentShader = CompileShader(fragmentShaderSource, GL_FRAGMENT_SHADER);

    glAttachShader(m_shaderProgram, vertexShader);
    glAttachShader(m_shaderProgram, fragmentShader);

    glLinkProgram(m_shaderProgram);

    // Shader objects are no longer needed after linking, free the memory.
    glDetachShader(m_shaderProgram, vertexShader);
    glDetachShader(m_shaderProgram, fragmentShader);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    CheckLinkStatus(vertexShaderSource, fragmentShaderSource);
}

auto Shader::SubmitShader(const std::string& source, GLenum type) -> GLuint
{
    auto shader = glCreateShader(type);
    const auto* shaderSourceCStr = source.c_str();
    glShaderSource(shader, 1, &shaderSourceCStr, nullptr);
    glCompileShader(shader);
    return shader;
}

void Shader::CheckShaderCompileStatus(GLuint shader, const std::string& source, GLenum type)
{
    GLint shaderCompiled{};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &shaderCompiled);
    if (shaderCompiled == GL_TRUE)
    {
        return;
    }

    GLint infoLogLength{};
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
    std::vector<char> message(infoLogLength + 1);
    glGetShaderInfoLog(shader, infoLogLength, nullptr, message.data());

    std::string compileError = "[Shader] Error compiling " +
        std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") +
        " shader: " + std::string(message.data());
    LOG_ERROR(compileError);
    LOG_DEBUG("[Shader] Failed source: " + source);
    throw ShaderException(compileError);
}

void Shader::CheckLinkStatus(const std::string& vertexSource, const std::string& fragmentSource)
{
    GLint programLinked;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &programLinked);
    if (programLinked == GL_TRUE)
    {
        return;
    }

    GLint infoLogLength{};
    glGetProgramiv(m_shaderProgram, GL_INFO_LOG_LENGTH, &infoLogLength);
    std::vector<char> message(infoLogLength + 1);
    glGetProgramInfoLog(m_shaderProgram, infoLogLength, nullptr, message.data());

    std::string linkError = "[Shader] Error linking compiled shader program: " + std::string(message.data());
    LOG_ERROR(linkError);
    LOG_DEBUG("[Shader] Vertex shader source: " + vertexSource);
    LOG_DEBUG("[Shader] Fragment shader source: " + fragmentSource);
    throw ShaderException(linkError);
}

void Shader::SubmitCompileAsync(const std::string& vertexShaderSource,
                                const std::string& fragmentShaderSource)
{
    m_asyncParallelAvailable = Platform::ParallelShaderProbe::Instance().IsAvailable();

    // Submit both shaders for compilation.
    m_asyncVertexShader = SubmitShader(vertexShaderSource, GL_VERTEX_SHADER);
    m_asyncFragmentShader = SubmitShader(fragmentShaderSource, GL_FRAGMENT_SHADER);

    if (!m_asyncParallelAvailable)
    {
        // Extension not available — glCompileShader calls above may still
        // compile asynchronously in some drivers/browsers.  Rather than
        // immediately querying GL_COMPILE_STATUS (which blocks until done),
        // defer the check to give the driver at least one frame to work.
        // The status will be checked in FinalizeCompile() after
        // IsCompileComplete() returns true.
        m_asyncVertexSource = vertexShaderSource;
        m_asyncFragmentSource = fragmentShaderSource;
        m_asyncState = AsyncState::CompilingShaders;
        glFlush(); // Hint to the driver to start compilation now.
        return;
    }

    // Extension available — store source for error reporting and enter async state.
    m_asyncVertexSource = vertexShaderSource;
    m_asyncFragmentSource = fragmentShaderSource;
    m_asyncState = AsyncState::CompilingShaders;

    LOG_TRACE("[Shader] Async compile submitted (parallel_shader_compile available)");
}

auto Shader::IsCompileComplete() const -> bool
{
    switch (m_asyncState)
    {
        case AsyncState::None:
            return true;

        case AsyncState::CompilingShaders:
        {
            if (!m_asyncParallelAvailable)
            {
                // No extension — we deferred from SubmitCompileAsync to give the
                // driver at least one frame.  Now advance directly to link+complete.
                // These calls will block, but the compile may have already finished
                // in the background during the deferred frame(s).
                auto* self = const_cast<Shader*>(this);
                self->AdvanceToLinking();
                // Link also blocks without the extension, so go straight to Complete.
                self->m_asyncState = AsyncState::Complete;
                return true;
            }

            // Poll GL_COMPLETION_STATUS_KHR on both shaders.
            GLint vertexDone = GL_FALSE;
            GLint fragmentDone = GL_FALSE;
            glGetShaderiv(m_asyncVertexShader,
                          static_cast<GLenum>(Platform::PM_GL_COMPLETION_STATUS_KHR),
                          &vertexDone);
            glGetShaderiv(m_asyncFragmentShader,
                          static_cast<GLenum>(Platform::PM_GL_COMPLETION_STATUS_KHR),
                          &fragmentDone);
            if (vertexDone == GL_TRUE && fragmentDone == GL_TRUE)
            {
                // Both shaders compiled — transition to ReadyToLink and
                // yield this frame.  Calling glLinkProgram here would block
                // on some drivers, stalling the render thread.  By deferring
                // the link to the next frame we keep this poll cheap.
                auto* self = const_cast<Shader*>(this);
                self->m_asyncState = AsyncState::ReadyToLink;
                return false;
            }
            return false;
        }

        case AsyncState::ReadyToLink:
        {
            // Shaders are compiled.  Submit the link now — this is the
            // first poll after we detected compile completion, so any
            // blocking in glLinkProgram is isolated to this frame.
            auto* self = const_cast<Shader*>(this);
            self->AdvanceToLinking();
            glFlush(); // Hint driver to start linking immediately.
            return false; // Give the link at least one frame.
        }

        case AsyncState::LinkingProgram:
        {
            GLint linkDone = GL_FALSE;
            glGetProgramiv(m_shaderProgram,
                           static_cast<GLenum>(Platform::PM_GL_COMPLETION_STATUS_KHR),
                           &linkDone);
            if (linkDone == GL_TRUE)
            {
                auto* self = const_cast<Shader*>(this);
                self->m_asyncState = AsyncState::Complete;
                return true;
            }
            return false;
        }

        case AsyncState::Complete:
            return true;

        default:
            return true;
    }
}

void Shader::FinalizeCompile()
{
    if (m_asyncState == AsyncState::None)
    {
        return; // Nothing to finalize.
    }

    // At this point both compile + link should be done.
    // Check results and throw on failure, matching CompileProgram behavior.
    if (m_asyncVertexShader)
    {
        CheckShaderCompileStatus(m_asyncVertexShader, m_asyncVertexSource, GL_VERTEX_SHADER);
    }
    if (m_asyncFragmentShader)
    {
        CheckShaderCompileStatus(m_asyncFragmentShader, m_asyncFragmentSource, GL_FRAGMENT_SHADER);
    }

    CheckLinkStatus(m_asyncVertexSource, m_asyncFragmentSource);

    // Clean up shader objects.
    if (m_asyncVertexShader)
    {
        glDetachShader(m_shaderProgram, m_asyncVertexShader);
        glDeleteShader(m_asyncVertexShader);
        m_asyncVertexShader = 0;
    }
    if (m_asyncFragmentShader)
    {
        glDetachShader(m_shaderProgram, m_asyncFragmentShader);
        glDeleteShader(m_asyncFragmentShader);
        m_asyncFragmentShader = 0;
    }

    // Free stored source strings.
    std::string().swap(m_asyncVertexSource);
    std::string().swap(m_asyncFragmentSource);

    m_asyncState = AsyncState::None;

    LOG_TRACE("[Shader] Async compile finalized successfully");
}

bool Shader::Validate(std::string& validationMessage) const
{
    GLint result{GL_FALSE};
    int infoLogLength;

    glValidateProgram(m_shaderProgram);

    glGetProgramiv(m_shaderProgram, GL_VALIDATE_STATUS, &result);
    glGetProgramiv(m_shaderProgram, GL_INFO_LOG_LENGTH, &infoLogLength);
    if (infoLogLength > 0)
    {
        std::vector<char> validationErrorMessage(infoLogLength + 1);
        glGetProgramInfoLog(m_shaderProgram, infoLogLength, nullptr, validationErrorMessage.data());
        validationMessage = std::string(validationErrorMessage.data());
    }

    return result;
}

void Shader::Bind() const
{
    if (m_shaderProgram > 0)
    {
        glUseProgram(m_shaderProgram);
    }
}

void Shader::Unbind()
{
    glUseProgram(0);
}

void Shader::SetUniformFloat(const char* uniform, float value) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform1fv(location, 1, &value);
}

void Shader::SetUniformInt(const char* uniform, int value) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform1iv(location, 1, &value);
}

void Shader::SetUniformFloat2(const char* uniform, const glm::vec2& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform2fv(location, 1, glm::value_ptr(values));
}

void Shader::SetUniformInt2(const char* uniform, const glm::ivec2& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform2iv(location, 1, glm::value_ptr(values));
}

void Shader::SetUniformFloat3(const char* uniform, const glm::vec3& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform3fv(location, 1, glm::value_ptr(values));
}

void Shader::SetUniformInt3(const char* uniform, const glm::ivec3& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform3iv(location, 1, glm::value_ptr(values));
}

void Shader::SetUniformFloat4(const char* uniform, const glm::vec4& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform4fv(location, 1, glm::value_ptr(values));
}

void Shader::SetUniformInt4(const char* uniform, const glm::ivec4& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniform4iv(location, 1, glm::value_ptr(values));
}

void Shader::SetUniformMat3x4(const char* uniform, const glm::mat3x4& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniformMatrix3x4fv(location, 1, GL_FALSE, glm::value_ptr(values));
}

void Shader::SetUniformMat4x4(const char* uniform, const glm::mat4x4& values) const
{
    auto location = glGetUniformLocation(m_shaderProgram, uniform);
    if (location < 0)
    {
        return;
    }
    glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(values));
}

GLuint Shader::CompileShader(const std::string& source, GLenum type)
{
    auto shader = SubmitShader(source, type);
    CheckShaderCompileStatus(shader, source, type);
    return shader;
}

void Shader::AdvanceToLinking()
{
    // We reach here because GL_COMPLETION_STATUS_KHR was GL_TRUE for both
    // shaders, meaning compilation has finished (though it may have failed).
    // Attach and submit the link.  glLinkProgram returns immediately with
    // the parallel compile extension — we'll poll link completion status
    // via GL_COMPLETION_STATUS_KHR in subsequent IsCompileComplete() calls.
    //
    // If either shader failed to compile, glLinkProgram will also fail.
    // We detect this in FinalizeCompile() where we check both compile and
    // link status and throw the appropriate ShaderException.
    glAttachShader(m_shaderProgram, m_asyncVertexShader);
    glAttachShader(m_shaderProgram, m_asyncFragmentShader);
    glLinkProgram(m_shaderProgram);

    m_asyncState = AsyncState::LinkingProgram;
}

auto Shader::GetShaderLanguageVersion() -> Shader::GlslVersion
{
    const char* shaderLanguageVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (shaderLanguageVersion == nullptr)
    {
        return {};
    }

    std::string shaderLanguageVersionString(shaderLanguageVersion);

    // Some OpenGL implementations add non-standard-conforming text in front, e.g. WebGL, which returns "OpenGL ES GLSL ES 3.00 ..."
    // Find the first digit and start there.
    auto firstDigit = shaderLanguageVersionString.find_first_of("0123456789");
    if (firstDigit != std::string::npos && firstDigit != 0)
    {
        shaderLanguageVersionString = shaderLanguageVersionString.substr(firstDigit);
    }

    // Cut off the vendor-specific information, if any
    auto spacePos = shaderLanguageVersionString.find(' ');
    if (spacePos != std::string::npos)
    {
        shaderLanguageVersionString.resize(spacePos);
    }

    auto dotPos = shaderLanguageVersionString.find('.');
    if (dotPos == std::string::npos)
    {
        return {};
    }

    int versionMajor = std::stoi(shaderLanguageVersionString.substr(0, dotPos));
    int versionMinor = std::stoi(shaderLanguageVersionString.substr(dotPos + 1));

    return {versionMajor, versionMinor};
}

} // namespace Renderer
} // namespace libprojectM
