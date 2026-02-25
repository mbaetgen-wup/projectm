/**
 * @file ParallelShaderProbe.hpp
 * @brief Runtime detection and activation of GL_KHR_parallel_shader_compile.
 *
 * Uses the GLResolver / GLProbe infrastructure to detect the extension and
 * resolve function pointers at runtime.  The extension allows the driver to
 * compile shaders on background threads; callers submit compile/link requests
 * and poll GL_COMPLETION_STATUS_KHR on subsequent frames instead of blocking.
 *
 * Supports both the KHR and ARB variants of the extension, as well as the
 * core GL 4.6 entrypoint (glMaxShaderCompilerThreads).
 */
#pragma once

#include <cstdint>
#include <mutex>

namespace libprojectM {
namespace Renderer {
namespace Platform {

/**
 * @brief OpenGL enum values for GL_KHR_parallel_shader_compile.
 *
 * Defined locally following the same pattern as GLProbe.cpp (PM_GL_*)
 * to avoid depending on a particular GL header version.
 */
enum : std::uint32_t
{
    /**
     * Token for querying compile/link completion via glGetShaderiv / glGetProgramiv.
     * Value is the same for both KHR and ARB variants.
     */
    PM_GL_COMPLETION_STATUS_KHR = 0x91B1,

    /**
     * Token for glGetIntegerv to query maximum shader compiler threads.
     */
    PM_GL_MAX_SHADER_COMPILER_THREADS_KHR = 0x91B0,

    /**
     * Same token, ARB variant (identical value).
     */
    PM_GL_MAX_SHADER_COMPILER_THREADS_ARB = 0x91B0
};

/**
 * @brief Singleton probe for GL_KHR_parallel_shader_compile support.
 *
 * Call Probe() once after the GL context is fully initialized (after
 * GladLoader::Initialize()).  Subsequent calls to IsAvailable() are
 * lock-free reads.
 */
class ParallelShaderProbe final
{
public:
    /**
     * @brief Returns the process-wide singleton.
     */
    static auto Instance() -> ParallelShaderProbe&;

    /**
     * @brief Probes the current GL context for parallel shader compile support.
     *
     * Checks for GL_KHR_parallel_shader_compile, GL_ARB_parallel_shader_compile,
     * or core GL >= 4.6.  If found, resolves glMaxShaderCompilerThreads* and
     * enables maximum parallelism.
     *
     * Thread-safe; may be called multiple times.  Only the first call performs
     * actual detection.
     */
    void Probe();

    /**
     * @brief Returns true if parallel shader compile is available.
     *
     * Only valid after Probe() has been called.  Lock-free.
     */
    auto IsAvailable() const -> bool { return m_available; }

    /**
     * @brief Returns true if Probe() has been called.
     */
    auto IsProbed() const -> bool { return m_probed; }

private:
    ParallelShaderProbe() = default;

    /**
     * @brief Signature for glMaxShaderCompilerThreadsKHR / glMaxShaderCompilerThreadsARB.
     */
    using MaxShaderCompilerThreadsFn = void (*)(unsigned int count);

    std::mutex m_mutex;
    bool m_probed{false};
    bool m_available{false};
    MaxShaderCompilerThreadsFn m_maxShaderCompilerThreads{nullptr};
};

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
