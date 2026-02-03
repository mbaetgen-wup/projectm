#include "GLProbe.hpp"

#include "../OpenGL.h"
#include "DynamicLibrary.hpp"
#include "GLResolver.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace libprojectM {
namespace Renderer {
namespace Platform {

namespace {

enum : std::uint16_t
{
    PM_GL_MAJOR_VERSION        = 0x821B,
    PM_GL_MINOR_VERSION        = 0x821C,
    PM_GL_CONTEXT_FLAGS        = 0x821E,
    PM_GL_CONTEXT_PROFILE_MASK = 0x9126
};

enum : std::uint32_t
{
    PM_GL_CONTEXT_CORE_PROFILE_BIT          = 0x00000001u,
    PM_GL_CONTEXT_COMPATIBILITY_PROFILE_BIT = 0x00000002u
};

enum : std::uint32_t
{
    PM_GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT = 0x00000001u,
    PM_GL_CONTEXT_FLAG_DEBUG_BIT              = 0x00000002u,
    PM_GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT      = 0x00000004u
};

struct ResolvedGLFunctions
{
    using GetStringFn   = decltype(+glGetString);
    using GetErrorFn    = decltype(+glGetError);
    using GetIntegervFn = decltype(+glGetIntegerv);

    GetStringFn getString{};
    GetErrorFn getError{};
    GetIntegervFn getIntegerv{};
};

auto ResolveGLFunctions(const GLProbe::GLFunctions& handles,
                        ResolvedGLFunctions& out,
                        std::string& reason) -> bool
{
    auto* getString = handles.getString;
    auto* getError = handles.getError;
    auto* getIntegerv = handles.getIntegerv;

    if ((getString == nullptr || getError == nullptr) && !GLResolver::Instance().IsLoaded())
    {
        reason = "GL entrypoints not configured and GLResolver is not loaded";
        return false;
    }

    if (getString == nullptr)
    {
        getString = GLResolver::Instance().GetProcAddress("glGetString");
    }

    if (getError == nullptr)
    {
        getError = GLResolver::Instance().GetProcAddress("glGetError");
    }

    if (getIntegerv == nullptr)
    {
        getIntegerv = GLResolver::Instance().GetProcAddress("glGetIntegerv");
    }

    // Convert opaque procedure addresses into typed function pointers.
    out.getString   = SymbolToFunction<ResolvedGLFunctions::GetStringFn>(getString);
    out.getError    = SymbolToFunction<ResolvedGLFunctions::GetErrorFn>(getError);
    out.getIntegerv = SymbolToFunction<ResolvedGLFunctions::GetIntegervFn>(getIntegerv);

    if (out.getString == nullptr || out.getError == nullptr)
    {
        reason = "GL entrypoints not available";
        return false;
    }

    // glGetIntegerv is optional for the check (we can parse GL_VERSION as fallback).
    reason.clear();
    return true;
}

auto ClearGlErrors(const ResolvedGLFunctions& gl) -> void
{
    if (gl.getError == nullptr)
    {
        return;
    }

    for (int i = 0; i < 32; ++i)
    {
        const auto err = gl.getError();
        if (err == GL_NO_ERROR)
        {
            break;
        }
    }
}

auto SafeStr(const unsigned char* str) -> const char*
{
    if (str != nullptr)
    {
        return reinterpret_cast<const char*>(str);
    }
    return "";
}

auto StartsWith(const char* str, const char* prefix) -> bool
{
    if (str == nullptr || prefix == nullptr)
    {
        return false;
    }
    return std::strncmp(str, prefix, std::strlen(prefix)) == 0;
}

auto SanitizeString(const std::string& input) -> std::string
{
    std::string out = input;
    for (auto& c : out)
    {
        if (c == '\n' || c == '\r' || c == '\t')
        {
            c = ' ';
        }
    }
    return out;
}

auto FormatVersion(int major, int minor) -> std::string
{
    std::ostringstream oss;
    oss << major << "." << minor;
    return oss.str();
}

auto ApiString(GLApi api) -> const char*
{
    switch (api)
    {
        case GLApi::OpenGLES:
        {
            return "GLES";
        }
        case GLApi::OpenGL:
        {
            return "GL";
        }
        default:
        {
            return "Any";
        }
    }
}

auto HasBasicGLEntrypoints(const ResolvedGLFunctions& gl, std::string& reason) -> bool
{
    if (gl.getString == nullptr || gl.getError == nullptr)
    {
        reason = "GL entrypoints not loaded (call gladLoadGL/GLES with a current context first)";
        return false;
    }
    return true;
}

auto QueryMajorMinor(const ResolvedGLFunctions& gl, int& major, int& minor) -> bool
{
    if (gl.getIntegerv == nullptr)
    {
        major = 0;
        minor = 0;
        return false;
    }

    major = 0;
    minor = 0;

    ClearGlErrors(gl);

    gl.getIntegerv(PM_GL_MAJOR_VERSION, &major);
    gl.getIntegerv(PM_GL_MINOR_VERSION, &minor);

    if (gl.getError() != GL_NO_ERROR)
    {
        return false;
    }

    return major > 0;
}

auto ParseVersionString(const char* str, bool isGLES, int& major, int& minor) -> bool
{
    if (str == nullptr || *str == 0)
    {
        return false;
    }

    const char* p = str;

    if (isGLES)
    {
        const char* found = std::strstr(str, "OpenGL ES");
        if (found != nullptr)
        {
            p = found + std::strlen("OpenGL ES");
        }
    }

    while ((*p != 0) && (*p < '0' || *p > '9'))
    {
        ++p;
    }

    return (std::sscanf(p, "%d.%d", &major, &minor) == 2) && (major > 0);
}

auto VersionAtLeast(int major, int minor, int reqMajor, int reqMinor) -> bool
{
    if (major != reqMajor)
    {
        return major > reqMajor;
    }
    return minor >= reqMinor;
}

auto ProfileString(const ResolvedGLFunctions& gl) -> std::string
{
    if (gl.getIntegerv == nullptr)
    {
        return "n/a";
    }

    int mask = 0;

    ClearGlErrors(gl);

    gl.getIntegerv(PM_GL_CONTEXT_PROFILE_MASK, &mask);

    if (gl.getError() != GL_NO_ERROR)
    {
        return "n/a";
    }

    if ((mask & PM_GL_CONTEXT_CORE_PROFILE_BIT) != 0)
    {
        return "core";
    }

    if ((mask & PM_GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0)
    {
        return "compat";
    }

    return "unknown";
}

auto FlagsString(const ResolvedGLFunctions& gl) -> std::string
{
    if (gl.getIntegerv == nullptr)
    {
        return "n/a";
    }

    int flags = 0;

    ClearGlErrors(gl);

    gl.getIntegerv(PM_GL_CONTEXT_FLAGS, &flags);

    if (gl.getError() != GL_NO_ERROR)
    {
        return "n/a";
    }

    std::vector<std::string> bits;

    if ((flags & PM_GL_CONTEXT_FLAG_DEBUG_BIT) != 0)
    {
        bits.push_back("debug");
    }

    if ((flags & PM_GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) != 0)
    {
        bits.push_back("fwd");
    }

    if ((flags & PM_GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT) != 0)
    {
        bits.push_back("robust");
    }

    if (bits.empty())
    {
        return "none";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < bits.size(); ++i)
    {
        if (i != 0)
        {
            oss << ",";
        }
        oss << bits[i];
    }
    return oss.str();
}

auto QueryInfo(const ResolvedGLFunctions& gl, GLInfo& info, std::string& reason) -> bool
{
    if (!HasBasicGLEntrypoints(gl, reason))
    {
        return false;
    }

    ClearGlErrors(gl);

    const char* ver = SafeStr(gl.getString(GL_VERSION));
    if (*ver == 0)
    {
        reason = "No current GL context (glGetString(GL_VERSION) returned null/empty)";
        return false;
    }

    const bool isGLES =
#if defined(__EMSCRIPTEN__)
        // WebGL is a GLES-like API surface
        true;
#else
        StartsWith(ver, "OpenGL ES") ||
        std::strstr(ver, "OpenGL ES") != nullptr ||
        std::strstr(ver, "WebGL") != nullptr;
#endif

    info.api = isGLES ? GLApi::OpenGLES : GLApi::OpenGL;
    info.versionStr = ver;

    info.vendor = SanitizeString(SafeStr(gl.getString(GL_VENDOR)));
    info.renderer = SanitizeString(SafeStr(gl.getString(GL_RENDERER)));

    const char* glsl = SafeStr(gl.getString(GL_SHADING_LANGUAGE_VERSION));
    if (*glsl != 0)
    {
        info.glslStr = SanitizeString(glsl);
    }
    else
    {
        info.glslStr.clear();
    }

    if (!QueryMajorMinor(gl, info.major, info.minor))
    {
        if (!ParseVersionString(ver, isGLES, info.major, info.minor))
        {
            reason = std::string("Unable to determine GL version from GL_VERSION=\"") + SanitizeString(ver) + "\"";
            return false;
        }
    }

#if defined(__EMSCRIPTEN__)
    // Emscripten can be configured to return WebGL-format version strings (e.g. "WebGL 2.0")
    // instead of ES-format strings. For the purposes of *minimum version checking* (not feature
    // completeness), WebGL 2 most closely maps to a GLES 3.0-class API surface.
    // If version parsing yields 2.0 from a WebGL 2.0 string, lift it to 3.0 for requirement checks.
    if (std::strstr(ver, "WebGL 2") != nullptr)
    {
        if (info.major < 3)
        {
            info.major = 3;
            info.minor = 0;
        }
    }
#endif

    info.profile = ProfileString(gl);
    info.flags = FlagsString(gl);

    reason.clear();
    return true;
}

} /* anonymous namespace */

auto GLProbe::InfoBuilder::WithGLFunctions(const GLFunctions& glFunctions) -> InfoBuilder&
{
    m_gl = glFunctions;
    return *this;
}

auto GLProbe::InfoBuilder::Build(GLInfo& info, std::string& reason) -> bool
{
    std::string ret;

    ResolvedGLFunctions gl;
    if (!ResolveGLFunctions(m_gl, gl, ret))
    {
        reason = ret;
        return false;
    }

    if (!QueryInfo(gl, info, ret))
    {
        reason = ret;
        return false;
    }

    reason.clear();
    return true;
}

GLProbe::CheckBuilder::CheckBuilder()
{
    m_req.api = GLApi::Any;
    m_req.minMajor = 0;
    m_req.minMinor = 0;
    m_req.requireCoreProfile = false;
    m_req.minShaderMajor = 0;
    m_req.minShaderMinor = 0;
}

auto GLProbe::CheckBuilder::WithGLFunctions(const GLFunctions& glFunctions) -> CheckBuilder&
{
    m_gl = glFunctions;
    return *this;
}

auto GLProbe::CheckBuilder::WithApi(GLApi api) -> CheckBuilder&
{
    m_req.api = api;
    return *this;
}

auto GLProbe::CheckBuilder::WithMinimumVersion(int major, int minor) -> CheckBuilder&
{
    m_req.minMajor = major;
    m_req.minMinor = minor;
    return *this;
}

auto GLProbe::CheckBuilder::WithMinimumShaderLanguageVersion(int major, int minor) -> CheckBuilder&
{
    m_req.minShaderMajor = major;
    m_req.minShaderMinor = minor;
    return *this;
}

auto GLProbe::CheckBuilder::WithRequireCoreProfile(bool required) -> CheckBuilder&
{
    m_req.requireCoreProfile = required;
    return *this;
}

auto GLProbe::CheckBuilder::Check() const -> GLProbeResult
{
    GLProbeResult result;
    result.req = m_req;

    std::string reason;

    ResolvedGLFunctions gl;
    if (!ResolveGLFunctions(m_gl, gl, reason))
    {
        result.success = false;
        result.reason = reason;
        return result;
    }

    if (!QueryInfo(gl, result.info, reason))
    {
        result.success = false;
        result.reason = reason;
        return result;
    }

    if (m_req.api != GLApi::Any && result.info.api != m_req.api)
    {
        result.success = false;
        result.reason = std::string("Wrong API: ") + ApiString(result.info.api);
        return result;
    }

    if (!VersionAtLeast(result.info.major, result.info.minor, m_req.minMajor, m_req.minMinor))
    {
        result.success = false;
        result.reason = "Version too low: " + FormatVersion(result.info.major, result.info.minor);
        return result;
    }

    if (m_req.minShaderMajor > 0 || m_req.minShaderMinor > 0)
    {
        if (result.info.glslStr.empty())
        {
            result.success = false;
            result.reason = "No shading language version reported";
            return result;
        }

        int glslMajor = 0;
        int glslMinor = 0;
        const bool isGLES = (result.info.api == GLApi::OpenGLES);
        if (!ParseVersionString(result.info.glslStr.c_str(), isGLES, glslMajor, glslMinor))
        {
            result.success = false;
            result.reason = std::string("Unable to parse shading language version: ") + result.info.glslStr;
            return result;
        }

        if (!VersionAtLeast(glslMajor, glslMinor, m_req.minShaderMajor, m_req.minShaderMinor))
        {
            result.success = false;
            result.reason = "Shading language version too low: " + FormatVersion(glslMajor, glslMinor);
            return result;
        }
    }

    if (m_req.requireCoreProfile &&
        result.info.api == GLApi::OpenGL &&
        result.info.profile != "core")
    {
        result.success = false;
        result.reason = "Core profile required";
        return result;
    }

    result.success = true;
    return result;
}

auto GLProbe::FormatCompactLine(const GLInfo& info) -> std::string
{
    std::ostringstream oss;

    oss << "api=\"" << ApiString(info.api) << "\""
        << " ver=\"" << FormatVersion(info.major, info.minor) << "\""
        << " profile=\"" << info.profile << "\""
        << " flags=\"" << info.flags << "\"";

    if (!info.glslStr.empty())
    {
        oss << " glsl=\"" << info.glslStr << "\"";
    }

    if (!info.vendor.empty())
    {
        oss << " vendor=\"" << info.vendor << "\"";
    }

    if (!info.renderer.empty())
    {
        oss << " renderer=\"" << info.renderer << "\"";
    }

    return oss.str();
}

} /* namespace Platform */
} /* namespace Renderer */
} /* namespace libprojectM */
