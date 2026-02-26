
#include "ParallelShaderProbe.hpp"

#include "../OpenGL.h"
#include "GLProbe.hpp"
#include "GLResolver.hpp"
#include "Logging.hpp"

#include <algorithm>
#include <string>

namespace libprojectM {
namespace Renderer {
namespace Platform {

auto ParallelShaderProbe::Instance() -> ParallelShaderProbe&
{
    static ParallelShaderProbe instance;
    return instance;
}

void ParallelShaderProbe::Probe()
{
    const std::unique_lock<std::mutex> lock(m_mutex);
    if (m_probed)
    {
        return;
    }
    m_probed = true;

#ifdef __EMSCRIPTEN__
    // Disable GL_KHR_parallel_shader_compile on Emscripten.
    //
    // While Chrome/ANGLE exposes the extension in WebGL, Emscripten's GL
    // wrapper does not reliably support polling GL_COMPLETION_STATUS_KHR
    // via glGetShaderiv — the query may silently return GL_FALSE forever,
    // causing the preset switch state machine to spin indefinitely.
    //
    // The synchronous fallback path in Shader::SubmitCompileAsync already
    // defers the blocking status check by one frame (via glFlush), giving
    // the browser's internal background compiler time to work.  This is
    // the safest approach across all browsers and GPU/driver combos.
    LOG_INFO("[ParallelShaderProbe] Disabled on Emscripten"
             " (COMPLETION_STATUS_KHR polling unreliable via GL wrapper)");
    return;
#endif

    // --- Step 1: Retrieve the extension list via GLProbe. ---
    GLInfo info;
    std::string reason;
    GLProbe::InfoBuilder infoBuilder;
    if (!infoBuilder.Build(info, reason))
    {
        LOG_DEBUG("[ParallelShaderProbe] Could not retrieve GL info: " + reason);
        return;
    }

    // --- Step 2: Check for extension support or core GL >= 4.6. ---
    bool hasKHR = false;
    bool hasARB = false;

    for (const auto& ext : info.extensions)
    {
        if (ext == "GL_KHR_parallel_shader_compile")
        {
            hasKHR = true;
        }
        else if (ext == "GL_ARB_parallel_shader_compile")
        {
            hasARB = true;
        }
    }

    const bool coreGL46 = (info.api == GLApi::OpenGL && (info.major > 4 || (info.major == 4 && info.minor >= 6)));

    if (!hasKHR && !hasARB && !coreGL46)
    {
        LOG_INFO("[ParallelShaderProbe] GL_KHR_parallel_shader_compile not available"
                 " (GL " + std::to_string(info.major) + "." + std::to_string(info.minor) +
                 ", " + info.vendor + " " + info.renderer + ")");
        return;
    }

    // --- Step 3: Resolve the glMaxShaderCompilerThreads function pointer via GLResolver. ---
    // Try the entrypoints in priority order: core (4.6) → KHR → ARB.
    static const char* const entrypoints[] = {
        "glMaxShaderCompilerThreads",
        "glMaxShaderCompilerThreadsKHR",
        "glMaxShaderCompilerThreadsARB"
    };

    void* procAddr = nullptr;
    const char* resolvedName = nullptr;
    for (const auto* name : entrypoints)
    {
        procAddr = GLResolver::Instance().GetProcAddress(name);
        if (procAddr)
        {
            resolvedName = name;
            break;
        }
    }

    if (!procAddr)
    {
        LOG_DEBUG("[ParallelShaderProbe] Extension reported but could not resolve"
                  " glMaxShaderCompilerThreads* entry point");
        // The extension is advertised but the entrypoint is missing.
        // GL_COMPLETION_STATUS_KHR queries might still work; enable the feature
        // without calling the setter.  Some drivers enable parallelism by default
        // when the extension is present.
        m_available = true;
        LOG_INFO("[ParallelShaderProbe] Enabled (no thread-count setter;"
                 " relying on driver default)");
        return;
    }

    m_maxShaderCompilerThreads = reinterpret_cast<MaxShaderCompilerThreadsFn>(procAddr);

    // --- Step 4: Enable maximum parallelism. ---
    // 0xFFFFFFFF means "use the maximum supported by the implementation".
    m_maxShaderCompilerThreads(0xFFFFFFFFu);

    // Verify GL didn't error (some drivers accept the extension string but
    // error on the call).
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        LOG_DEBUG("[ParallelShaderProbe] " + std::string(resolvedName) +
                  "(0xFFFFFFFF) produced GL error 0x" +
                  ([err]() -> std::string {
                      char buf[16];
                      std::snprintf(buf, sizeof(buf), "%04X", static_cast<unsigned>(err));
                      return buf;
                  })());
        m_maxShaderCompilerThreads = nullptr;
        return;
    }

    m_available = true;
    LOG_INFO("[ParallelShaderProbe] Enabled via " + std::string(resolvedName) +
             " (KHR=" + (hasKHR ? "yes" : "no") +
             " ARB=" + (hasARB ? "yes" : "no") +
             " core46=" + (coreGL46 ? "yes" : "no") +
             " vendor=\"" + info.vendor +
             "\" renderer=\"" + info.renderer + "\")");
}

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
