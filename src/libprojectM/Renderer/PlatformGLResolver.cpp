// Cross-platform runtime GL/GLES loader using GLAD2 C API (non-MX).
//
// Provides a universal resolver to find GL function pointers.

#include "PlatformGLResolver.hpp"

#include "OpenGL.h"
#include "PlatformGLContextCheck.hpp"

#include "SOIL2/SOIL2.h"
#include "SOIL2/SOIL2_gl_bridge.h"

#include <Logging.hpp>

#include <array>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#endif

namespace libprojectM {
namespace Renderer {
namespace Platform {

namespace
{

/**
 * Current GL context probe results.
 */
struct CurrentContextProbe
{
    bool eglLibOpened{false};
    bool eglAvailable{false};
    bool eglCurrent{false};

    bool glxLibOpened{false};
    bool glxAvailable{false};
    bool glxCurrent{false};

    bool wglLibOpened{false};
    bool wglAvailable{false};
    bool wglCurrent{false};

    bool cglLibOpened{false};
    bool cglAvailable{false};
    bool cglCurrent{false};
};

auto ProbeCurrentContext(const DynamicLibrary& eglLib,
                                const DynamicLibrary& glLib,
                                const DynamicLibrary& glxLib) -> CurrentContextProbe
{
    CurrentContextProbe result;

#ifdef __EMSCRIPTEN__
    (void)eglLib;
    (void)glLib;
    (void)glxLib;
    return result;
#else
    // EGL (including ANGLE, GLES, and desktop EGL)
    {
        result.eglLibOpened = eglLib.IsOpen();
        using EglGetCurrentContextFn = void* (PLATFORM_EGLAPIENTRY*)();

        void* sym = nullptr;
        if (eglLib.IsOpen())
        {
            sym = eglLib.GetSymbol("eglGetCurrentContext");
        }
        if (sym == nullptr)
        {
            // POSIX: if EGL is already linked/loaded by the host, this can still succeed without dlopen.
            // Windows: FindGlobalSymbol only checks main module/opengl32, so this is best-effort.
            sym = DynamicLibrary::FindGlobalSymbol("eglGetCurrentContext");
        }

        auto func = SymbolToFunction<EglGetCurrentContextFn>(sym);
        if (func != nullptr)
        {
            result.eglAvailable = true;
            result.eglCurrent = func() != nullptr;
        }
    }

    // GLX (Linux/Unix via libGLX/libGL or GLVND dispatch)
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
    {
        result.glxLibOpened = glxLib.IsOpen() || glLib.IsOpen();
        using GlxGetCurrentContextFn = void* (*)();

        void* sym = nullptr;
        if (glxLib.IsOpen())
        {
            sym = glxLib.GetSymbol("glXGetCurrentContext");
        }
        if (sym == nullptr && glLib.IsOpen())
        {
            sym = glLib.GetSymbol("glXGetCurrentContext");
        }
        if (sym == nullptr)
        {
            // If GLX is already loaded/linked by the host, probe via global symbols (GLVND setups).
            sym = DynamicLibrary::FindGlobalSymbol("glXGetCurrentContext");
        }

        auto func = SymbolToFunction<GlxGetCurrentContextFn>(sym);
        if (func != nullptr)
        {
            result.glxAvailable = true;
            result.glxCurrent = func() != nullptr;
        }
    }
#endif

    // WGL (Windows desktop GL)
#ifdef _WIN32
    {
        const HMODULE glModule = ::GetModuleHandleA("opengl32.dll");
        result.wglLibOpened = glModule != nullptr;

        if (glModule != nullptr)
        {
            const FARPROC proc = ::GetProcAddress(glModule, "wglGetCurrentContext");
            using WglGetCurrentContextFn = HGLRC (WINAPI*)();
            auto func = SymbolToFunction<WglGetCurrentContextFn>(WinProcToSymbol(proc));
            if (func != nullptr)
            {
                result.wglAvailable = true;
                result.wglCurrent = func() != nullptr;
            }
        }
    }
#endif

    // CGL (macOS native OpenGL)
#if defined(__APPLE__) && !defined(_WIN32)
    {
        result.cglLibOpened = glLib.IsOpen();
        using CglGetCurrentContextFn = void* (*)();

        void* sym = nullptr;
        if (glLib.IsOpen())
        {
            sym = glLib.GetSymbol("CGLGetCurrentContext");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("CGLGetCurrentContext");
        }

        auto func = SymbolToFunction<CglGetCurrentContextFn>(sym);
        if (func != nullptr)
        {
            result.cglAvailable = true;
            result.cglCurrent = func() != nullptr;
        }
    }
#endif

    return result;
#endif // __EMSCRIPTEN__
}

auto PickCurrentBackend(const CurrentContextProbe& probe) -> Backend
{
#ifdef __EMSCRIPTEN__
    (void)probe;
    return Backend::WebGL;
#else
    if (probe.eglCurrent)
    {
        return Backend::EGL;
    }
#ifdef _WIN32
    if (probe.wglCurrent)
    {
        return Backend::WGL;
    }
#elif defined(__APPLE__)
    if (probe.cglCurrent)
    {
        return Backend::CGL;
    }
#elif !defined(__ANDROID__)
    if (probe.glxCurrent)
    {
        return Backend::GLX;
    }
#endif
    return Backend::None;
#endif
}

auto HasSpaceSeparatedToken(const char* list, const char* token) -> bool
{
    if (list == nullptr || token == nullptr)
    {
        return false;
    }

    const std::size_t tokenLen = std::strlen(token);
    if (tokenLen == 0u)
    {
        return false;
    }

    // Extension strings are defined as space-separated tokens.
    // Match whole tokens only to avoid false positives (e.g. "FOO" matching "FOOBAR").
    const char* p = list;
    while (*p != 0)
    {
        while (*p == ' ')
        {
            ++p;
        }
        if (*p == 0)
        {
            break;
        }

        const char* start = p;
        while (*p != 0 && *p != ' ')
        {
            ++p;
        }
        const std::size_t len = static_cast<std::size_t>(p - start);
        if (len == tokenLen && std::strncmp(start, token, tokenLen) == 0)
        {
            return true;
        }
    }

    return false;
}

auto IsLikelyExtensionName(const char* name) -> bool
{
    // Common extension suffixes.
    static constexpr std::array<const char*, 33> kSuffixes = {
        "ARB", "EXT", "KHR", "OES", "NV", "NVX", "AMD", "APPLE",
        "ANGLE", "INTEL", "MESA", "QCOM", "IMG", "ARM", "ATI", "IBM",
        "SUN", "SGI", "SGIX", "OML", "GREMEDY", "HP", "3DFX", "S3",
        "PVR", "VIV", "ARM", "OVR", "NOK", "MSFT", "SEC", "DMP", "FJ" };

    const std::size_t len = std::strlen(name);
    for (const char* suffix : kSuffixes)
    {
        const std::size_t slen = std::strlen(suffix);
        if (len >= slen && std::strcmp(name + (len - slen), suffix) == 0)
        {
            return true;
        }
    }
    return false;
}


#ifndef __EMSCRIPTEN__

/**
 * @brief Returns true if a GL/EGL symbol name is likely to be an extension entry point.
 *
 * Per EGL_KHR_get_all_proc_addresses (and its client variant), eglGetProcAddress
 * is not required to return addresses for non-extension EGL or client API functions
 * unless the extension is advertised. When the extension is not available, a
 * conservative policy is to query eglGetProcAddress only for extension-style
 * entry points (ARB/EXT/KHR/etc.), while resolving core symbols via direct exports.
 */
auto ShouldUseEglGetProcAddressForName(const char* name) -> bool
{
    if (name == nullptr)
    {
        return false;
    }

    return IsLikelyExtensionName(name);
}

#endif // __EMSCRIPTEN__

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)

/**
 * @brief Returns true if a GL symbol name is likely to be an extension or GLX entry point.
 *
 * GLX loaders can return a non-null pointer for unknown functions. A conservative
 * mitigation is to only accept glXGetProcAddress* results for names that appear to
 * be extension entry points (ARB/EXT/KHR/etc.), while resolving core entry points
 * via direct exports (dlsym / libOpenGL/libGL).
 */
auto ShouldUseGlxGetProcAddressForName(const char* name) -> bool
{
    if (name == nullptr)
    {
        return false;
    }

    // GLX entry points themselves are always resolved via GLX libraries.
    if (std::strncmp(name, "glX", 3) == 0)
    {
        return true;
    }

    if (IsLikelyExtensionName(name))
    {
        return true;
    }

    return false;
}

#endif

} // anonymous namespace


auto GLResolver::Instance() -> GLResolver&
{
    static GLResolver instance;
    return instance;
}

void GLResolver::SetBackendDefault()
{
    if (m_backend != Backend::None)
    {
        return;
    }

#ifdef __EMSCRIPTEN__
    m_backend = Backend::WebGL;
    return;
#endif

    // Only assign a default when it is unambiguous and does not reduce resolver coverage.
    // Leaving Backend::None enables the generic resolver path (tries all available providers).
    const bool hasEglProvider = (m_eglGetProcAddress != nullptr);

#if defined(__APPLE__) && !defined(_WIN32)
    // macOS native OpenGL: no provider GetProcAddress; treat as CGL if nothing else is available.
    const bool hasAnyProvider = hasEglProvider;
    if (!hasAnyProvider)
    {
        m_backend = Backend::CGL;
        return;
    }
#endif

#ifndef _WIN32
    const bool hasGlxProvider = (m_glxGetProcAddress != nullptr);
    if (hasEglProvider && !hasGlxProvider)
    {
        m_backend = Backend::EGL;
        return;
    }
    if (hasGlxProvider && !hasEglProvider)
    {
        m_backend = Backend::GLX;
        return;
    }
#else
    const bool hasWglProvider = (m_wglGetProcAddress != nullptr);
    if (hasEglProvider && !hasWglProvider)
    {
        m_backend = Backend::EGL;
        return;
    }
    if (hasWglProvider && !hasEglProvider)
    {
        m_backend = Backend::WGL;
        return;
    }
#endif
}

auto GLResolver::CheckGLRequirementsUnlocked() -> GLContextCheckResult
{
    GLContextCheck::Builder glCheck;
#ifdef USE_GLES
    glCheck
        .WithApi(GLApi::OpenGLES)
        .WithMinimumVersion(3, 0)
        .WithRequireCoreProfile(false);
#else
    glCheck
        .WithApi(GLApi::OpenGL)
        .WithMinimumVersion(3, 3)
        // Accept both core and compatibility contexts. A 3.3+ compatibility context is a valid
        // configuration on many drivers/stacks, and profile filtering would reject it unnecessarily.
        .WithRequireCoreProfile(false);
#endif

    return  glCheck.Check();
}

auto GLResolver::Initialize(UserResolver resolver, void* userData) -> bool
{
    // Prevent concurrent Initialize()
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_loaded)
    {
        return true;
    }

    m_initCv.wait(lock, [this]() { return !m_initializing; });

    if (m_loaded)
    {
        return true;
    }

    m_initializing = true;

    m_userResolver = resolver;
    m_userData = userData;

    // Find source for gl functions.
#ifndef __EMSCRIPTEN__
    OpenNativeLibraries();
    ResolveProviderFunctions();
#endif

    // Precondition: caller must have a current context on this thread.
    {
        std::string reason;
        if (!HasCurrentContext(reason))
        {
            m_loaded = false;
            m_backend = Backend::None;

            m_initializing = false;
            m_initCv.notify_all();
            lock.unlock();

            LOG_FATAL(std::string("[GLResolver] No current GL context present: ") + reason);
            return false;
        }
    }

    DetectBackend();

    // Emit a concise diagnostics line for troubleshooting mixed-stack processes.
    {
        std::string diag = std::string("[GLResolver] Resolver policy: backend=\"") +
                           BackendToString(m_backend) + "\""
#ifndef __EMSCRIPTEN__
                           +
                           " egl=\"" + m_eglLib.LoadedName() + "\"" +
                           " gl=\"" + m_glLib.LoadedName() + "\"" +
                           " glx=\"" + m_glxLib.LoadedName() + "\"" +
                           " egl_get_proc=\"" + (m_eglGetProcAddress != nullptr ? "yes" : "no") + "\"" +
                           " egl_all_proc=\"" + (m_eglGetAllProcAddresses ? "yes" : "no") + "\""
#endif
        ;

#ifdef _WIN32
        diag += std::string(" wgl_get_proc=\"") + (m_wglGetProcAddress != nullptr ? "yes" : "no") + "\"";
#else
        diag += std::string(" glx_get_proc=\"") + (m_glxGetProcAddress != nullptr ? "yes" : "no") + "\"";
        diag += " glx_policy=\"ext-only\"";
#endif
        diag += std::string(" user_resolver=\"") + (m_userResolver != nullptr ? "yes" : "no") + "\"";

        LOG_DEBUG(diag);
    }

    // Do not hold m_mutex while calling into GLAD.
    lock.unlock();
    const bool loaded = LoadGladUnlocked();
    lock.lock();

    if (!loaded)
    {
        m_loaded = false;
    }
    else
    {
        // Set default in case detection failed, but loading succeeded.
        SetBackendDefault();

        lock.unlock();
        const auto glDetails = CheckGLRequirementsUnlocked();
        lock.lock();

        LOG_INFO(std::string("[GLResolver] GL Info: ") +
                 GLContextCheck::FormatCompactLine(glDetails.info) +
                 " backend=\"" + BackendToString(m_backend) + "\"" +
                 " user_resolver=\"" + (m_userResolver != nullptr ? "yes" : "no") + "\"");

        if (!glDetails.success)
        {
            LOG_FATAL(std::string("[GLResolver] GL requirements check failed: ") + glDetails.reason);
        }

        lock.unlock();

        const bool ready = glDetails.success;
        if (ready)
        {
            SOIL_GL_SetResolver(&GLResolver::GladResolverThunk);
            SOIL_GL_Init();
        }

        lock.lock();
        m_loaded = ready;
    }

    m_initializing = false;
    m_initCv.notify_all();
    lock.unlock();

    return m_loaded;
}

auto GLResolver::HasCurrentContext(std::string& outReason) const -> bool
{
#ifdef __EMSCRIPTEN__
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
    bool webglCurrent = ctx != 0;
    if (webglCurrent)
    {
        return true;
    }
    outReason = "WebGL: no current context";
    return false;
#else
    auto probe = ProbeCurrentContext(m_eglLib, m_glLib, m_glxLib);
    if (probe.eglCurrent || probe.glxCurrent || probe.wglCurrent || probe.cglCurrent)
    {
        return true;
    }

    std::string reason;

    if (probe.eglAvailable)
    {
        reason += "EGL: no current context; ";
    }
    else if (probe.eglLibOpened)
    {
        reason += "EGL: eglGetCurrentContext missing; ";
    }

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
    if (probe.glxAvailable)
    {
        reason += "GLX: no current context; ";
    }
    else if (probe.glxLibOpened)
    {
        reason += "GLX: glXGetCurrentContext missing; ";
    }
#endif

#ifdef _WIN32
    if (probe.wglAvailable)
    {
        reason += "WGL: no current context; ";
    }
    else if (probe.wglLibOpened)
    {
        reason += "WGL: wglGetCurrentContext missing; ";
    }
    else
    {
        reason += "WGL: opengl32.dll not loaded; ";
    }
#endif

#if defined(__APPLE__) && !defined(_WIN32)
    if (probe.cglAvailable)
    {
        reason += "CGL: no current context; ";
    }
    else if (probe.cglLibOpened)
    {
        reason += "CGL: CGLGetCurrentContext missing; ";
    }
    else
    {
        reason += "CGL: CGLGetCurrentContext symbol not available; ";
    }
#endif

    if (reason.empty())
    {
        // If nothing was available, libraries may not have been opened yet.
        reason = "No platform current-context query available (libraries not loaded?)";
    }
    else
    {
        // Trim trailing "; "
        if (reason.size() >= 2)
        {
            reason.resize(reason.size() - 2);
        }
    }

    outReason = reason;
    return false;
#endif // #ifdef __EMSCRIPTEN__
}

auto GLResolver::IsLoaded() const -> bool
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_loaded;
}

auto GLResolver::CurrentBackend() const -> Backend
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_backend;
}

auto GLResolver::GetProcAddress(const char* name) const -> void*
{
    if (name == nullptr)
    {
        return nullptr;
    }

    // Avoid holding m_mutex while calling user callbacks or driver/loader code.
    // Only hold m_mutex while reading internal state or using library handles.
    std::unique_lock<std::mutex> lock(m_mutex);

    const UserResolver userResolver = m_userResolver;
    void* const userData = m_userData;
    const Backend backend = m_backend;

    EglGetProcAddressFn eglFn = m_eglGetProcAddress;
    bool eglGetAllProcAddresses = m_eglGetAllProcAddresses;
    bool allowEglProvider = true;

#ifndef _WIN32
    GlxGetProcAddressFn glxFn = nullptr;
    bool allowGlxProvider = true;
#else
    WglGetProcAddressFn wglFn = nullptr;
    bool allowWglProvider = true;
#endif

#ifndef __EMSCRIPTEN__
    CurrentContextProbe probe;
    bool haveProbe = false;
#endif

    {

#ifndef _WIN32

#if defined(__APPLE__) || defined(__ANDROID__)
        glxFn = nullptr;
#else
        glxFn = m_glxGetProcAddress;
#endif

#else
        wglFn = m_wglGetProcAddress;
#endif // #ifndef _WIN32

#ifndef __EMSCRIPTEN__
        if (backend == Backend::None)
        {
            probe = ProbeCurrentContext(m_eglLib, m_glLib, m_glxLib);
            haveProbe = true;
        }
#endif
    }

#ifndef __EMSCRIPTEN__
    // Safety: if backend detection failed, restrict provider calls to the provider matching the
    // actually-current context on this thread. This avoids returning pointers from a different
    // loader/stack when multiple GL stacks are present in-process.
    if (backend == Backend::None && haveProbe)
    {
        allowEglProvider = probe.eglCurrent;
#ifndef _WIN32
        allowGlxProvider = probe.glxCurrent;
#else
        allowWglProvider = probe.wglCurrent;
#endif
    }
    else
    {
        // Backend is known: allow the matching provider. ResolveUnlocked still gates by backend.
        allowEglProvider = (backend == Backend::EGL);
#ifndef _WIN32
        allowGlxProvider = (backend == Backend::GLX);
#else
        allowWglProvider = (backend == Backend::WGL);
#endif
    }
#endif

    lock.unlock();

#ifndef _WIN32
    void* resolved = ResolveUnlocked(
        name,
        userResolver,
        userData,
        backend,
        eglFn,
        eglGetAllProcAddresses,
        allowEglProvider,
        glxFn,
        allowGlxProvider);
#else
    void* resolved = ResolveUnlocked(
        name,
        userResolver,
        userData,
        backend,
        eglFn,
        eglGetAllProcAddresses,
        allowEglProvider,
        wglFn,
        allowWglProvider);
#endif

    if (resolved != nullptr)
    {
        return resolved;
    }

    lock.lock();

    // Global symbol table (works if the process already linked/loaded GL libs).
    void* global = DynamicLibrary::FindGlobalSymbol(name);
    if (global != nullptr)
    {
        return global;
    }

    // Direct library symbol lookup.
    if (m_eglLib.IsOpen())
    {
        void* ptr = m_eglLib.GetSymbol(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }
    if (m_glLib.IsOpen())
    {
        void* ptr = m_glLib.GetSymbol(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }
    if (m_glxLib.IsOpen())
    {
        void* ptr = m_glxLib.GetSymbol(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }


    return nullptr;
}

void GLResolver::OpenNativeLibraries()
{
    // Best-effort: macOS or minimal EGL setups may fail to open.
    std::string reason;

#ifdef _WIN32
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.dll", "EGL.dll", nullptr};

    static constexpr std::array<const char*, 2> kGlDesktopNames = {"opengl32.dll", nullptr};
    static constexpr std::array<const char*, 5> kGlGlesNames = {"libGLESv3.dll", "GLESv3.dll", "libGLESv2.dll", "GLESv2.dll", nullptr};
#elif defined(__APPLE__)
    // macOS native OpenGL uses CGL (OpenGL.framework). ANGLE (and other portability layers).
    // commonly provide EGL/GLES dylibs in the application bundle / @rpath.
    static constexpr std::array<const char*, 4> kEglNames = {"libEGL.dylib", "libEGL.1.dylib", "EGL", nullptr};

    static constexpr std::array<const char*, 2> kGlCglNames = {
        "/System/Library/Frameworks/OpenGL.framework/OpenGL",
        nullptr
    };

    static constexpr std::array<const char*, 3> kGlGlesNames = {
        "libGLESv3.dylib",
        "libGLESv2.dylib",
        nullptr
    };
#elif defined(__ANDROID__)
    // Android uses EGL + GLES (no desktop libGL / GLX).
    static constexpr std::array<const char*, 2> kEglNames = {"libEGL.so", nullptr};
    static constexpr std::array<const char*, 3> kGlNames = { "libGLESv3.so", "libGLESv2.so", nullptr};
#else
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.so.1", "libEGL.so", nullptr};
#if defined(USE_GLES)
    // Linux / GLES:
    // Prefer libGLESv3/libGLESv2 sonames. Core GLES entry points are expected
    // to be available as library exports. eglGetProcAddress is not guaranteed
    // to return core symbols unless EGL_KHR_get_all_proc_addresses (or its
    // client variant) is advertised.
    static constexpr std::array<const char*, 6> kGlNames = {
        "libGLESv3.so.3",
        "libGLESv3.so",
        "libGLESv2.so.2",
        "libGLESv2.so.1",
        "libGLESv2.so",
        nullptr};
#else
    static constexpr std::array<const char*, 6> kGlNames = {
        "libGL.so.1",     // legacy/compat umbrella (often provided by GLVND)
        "libGL.so.0",     // sometimes shipped as .so.0
        "libOpenGL.so.1", // GLVND OpenGL dispatcher (core gl* entry points)
        "libOpenGL.so.0", // older GLVND soname
        "libGL.so",
        nullptr};
#endif

    // Linux / GLVND note:
    // Some environments (especially minimal/container) may not ship libGL.so.1 but do ship GLVND libs.
    // Keep legacy libGL first for backwards compatibility, but fall back to GLVND-facing libs if needed.
    static constexpr std::array<const char*, 3> kGlxNames = {
        "libGLX.so.1", // GLVND GLX dispatcher (glXGetProcAddress*)
        "libGLX.so.0", // older GLVND soname
        nullptr};
    reason = "";
    const bool glxOpened = m_glxLib.Open(kGlxNames.data(), reason);
    if (!glxOpened)
    {
        LOG_DEBUG(std::string("[GLResolver] Failed to open GLX library: ") + reason);
    }
#endif

    reason = "";
    const bool eglOpened = m_eglLib.Open(kEglNames.data(), reason);
    if (eglOpened == false)
    {
        LOG_DEBUG(std::string("[GLResolver] Failed to open EGL library: ") + reason);
    }

#ifdef _WIN32
#if defined(USE_GLES)
    const auto* glNames = kGlGlesNames.data();
#else
    // Desktop OpenGL build: always prefer opengl32.dll.
    // Note: ANGLE typically exposes GLES via EGL, and is expected to be used with USE_GLES builds.
    const auto* glNames = kGlDesktopNames.data();
#endif
    reason = "";
    const bool glOpened = m_glLib.Open(glNames, reason);
#elif defined(__APPLE__)
#if defined(USE_GLES)
    const auto* glNames = kGlGlesNames.data();
#else
    // Desktop OpenGL build: always prefer OpenGL.framework.
    // EGL/GLES stacks (e.g. ANGLE) are expected to be used with USE_GLES builds.
    const auto* glNames = kGlCglNames.data();
#endif
    reason = "";
    const bool glOpened = m_glLib.Open(glNames, reason);
#else
    reason = "";
    const bool glOpened = m_glLib.Open(kGlNames.data(), reason);
#endif
    if (!glOpened)
    {
        LOG_DEBUG(std::string("[GLResolver] Failed to open GL library: ") + reason);
    }

    // GLResolver is a process-lifetime singleton. Keep successfully loaded driver libraries
    // mapped until process exit to avoid shutdown-order issues during teardown.
    if (m_eglLib.IsOpen())
    {
        m_eglLib.SetCloseOnDestruct(false);
    }
    if (m_glLib.IsOpen())
    {
        m_glLib.SetCloseOnDestruct(false);
    }
    if (m_glxLib.IsOpen())
    {
        m_glxLib.SetCloseOnDestruct(false);
    }
}

void GLResolver::ResolveProviderFunctions()
{
    // EGL
    {
        // Note: eglGetProcAddress is the canonical mechanism for resolving EGL/GLES entry points,
        // but some older EGL implementations/spec interpretations may not return addresses for all
        // core functions. This resolver therefore also falls back to global and direct library
        // exports in GetProcAddress().
        void* sym = nullptr;
        if (m_eglLib.IsOpen())
        {
            sym = m_eglLib.GetSymbol("eglGetProcAddress");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("eglGetProcAddress");
        }
        if (sym != nullptr)
        {
            m_eglGetProcAddress = SymbolToFunction<EglGetProcAddressFn>(sym);
            if (m_eglGetProcAddress == nullptr)
            {
                LOG_DEBUG("[GLResolver] eglGetProcAddress found but could not be converted to a function pointer");
            }
        }
        else if (m_eglLib.IsOpen())
        {
            LOG_DEBUG("[GLResolver] eglGetProcAddress not found (EGL loaded but missing symbol)");
        }
    }
    // eglGetCurrentContext (used for backend probing)
    {
        void* sym = nullptr;
        if (m_eglLib.IsOpen())
        {
            sym = m_eglLib.GetSymbol("eglGetCurrentContext");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("eglGetCurrentContext");
        }
        m_eglGetCurrentContext = SymbolToFunction<GetCurrentContextFn>(sym);
    }

    // Detect EGL_KHR_get_all_proc_addresses / EGL_KHR_client_get_all_proc_addresses.
    // When advertised, eglGetProcAddress is allowed to return addresses for all client API functions,
    // including core entry points.
    // - Client extension: query via eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)
    // - Display extension: query via eglQueryString(current_display, EGL_EXTENSIONS)
    // See: Khronos registry extension text.
    {
        m_eglGetAllProcAddresses = false;

        // Avoid depending on platform EGL headers here; use opaque handles and well-known constants.
        // Note: on 32-bit Windows, EGL entry points use stdcall (EGLAPIENTRY).
        using EglDisplay = void*;
        using EglQueryStringFn = const char* (PLATFORM_EGLAPIENTRY*)(EglDisplay, int);
        using EglGetCurrentDisplayFn = EglDisplay (PLATFORM_EGLAPIENTRY*)();
        using EglGetErrorFn = int (PLATFORM_EGLAPIENTRY*)();

        constexpr int kEglExtensions = 0x3055; // EGL_EXTENSIONS
        constexpr int kEglBadDisplay = 0x3008; // EGL_BAD_DISPLAY
        constexpr EglDisplay kEglNoDisplay = nullptr; // EGL_NO_DISPLAY

        void* querySym = nullptr;
        if (m_eglLib.IsOpen())
        {
            querySym = m_eglLib.GetSymbol("eglQueryString");
        }
        if (querySym == nullptr)
        {
            querySym = DynamicLibrary::FindGlobalSymbol("eglQueryString");
        }

        auto eglQueryStringFn = SymbolToFunction<EglQueryStringFn>(querySym);
        if (eglQueryStringFn != nullptr)
        {
            const char* clientExt = eglQueryStringFn(kEglNoDisplay, kEglExtensions);
            if (clientExt != nullptr)
            {
                // Client extensions are only queryable when EGL_EXT_client_extensions is supported.
                // When available, EGL_KHR_client_get_all_proc_addresses implies that eglGetProcAddress
                // may return addresses for all client API functions (including core entry points).
                if (HasSpaceSeparatedToken(clientExt, "EGL_KHR_client_get_all_proc_addresses"))
                {
                    m_eglGetAllProcAddresses = true;
                }
            }
            else
            {
                // If EGL_EXT_client_extensions is not supported, this call is expected to fail.
                // Best-effort: drain eglGetError() for deterministic debug logs.
                void* errSym = nullptr;
                if (m_eglLib.IsOpen())
                {
                    errSym = m_eglLib.GetSymbol("eglGetError");
                }
                if (errSym == nullptr)
                {
                    errSym = DynamicLibrary::FindGlobalSymbol("eglGetError");
                }
                auto eglGetErrorFn = SymbolToFunction<EglGetErrorFn>(errSym);
                if (eglGetErrorFn != nullptr)
                {
                    const int err = eglGetErrorFn();
                    if (err != 0 && err != kEglBadDisplay)
                    {
                        char hexBuf[16] = {};
                        std::snprintf(hexBuf, sizeof(hexBuf), "%x", static_cast<unsigned int>(err));
                        LOG_DEBUG(std::string("[GLResolver] eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) failed with EGL error=0x") + hexBuf);
                    }
                }
            }

            // Display extension: requires a display. Best-effort: use eglGetCurrentDisplay if available.
            void* dpySym = nullptr;
            if (m_eglLib.IsOpen())
            {
                dpySym = m_eglLib.GetSymbol("eglGetCurrentDisplay");
            }
            if (dpySym == nullptr)
            {
                dpySym = DynamicLibrary::FindGlobalSymbol("eglGetCurrentDisplay");
            }

            auto eglGetCurrentDisplayFn = SymbolToFunction<EglGetCurrentDisplayFn>(dpySym);
            if (eglGetCurrentDisplayFn != nullptr)
            {
                EglDisplay dpy = eglGetCurrentDisplayFn();
                if (dpy != nullptr)
                {
                    const char* displayExt = eglQueryStringFn(dpy, kEglExtensions);
                    if (displayExt != nullptr)
                    {
                        if (HasSpaceSeparatedToken(displayExt, "EGL_KHR_get_all_proc_addresses"))
                        {
                            m_eglGetAllProcAddresses = true;
                        }
                    }
                }
            }
        }

        LOG_DEBUG(std::string("[GLResolver] EGL get_all_proc_addresses=") + (m_eglGetAllProcAddresses ? "yes" : "no"));
    }

#ifdef _WIN32
    // WGL
    {
        void* sym = nullptr;
        if (m_glLib.IsOpen())
        {
            sym = m_glLib.GetSymbol("wglGetProcAddress");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("wglGetProcAddress");
        }
        if (sym != nullptr)
        {
            m_wglGetProcAddress = SymbolToFunction<WglGetProcAddressFn>(sym);
            if (m_wglGetProcAddress == nullptr)
            {
                LOG_DEBUG("[GLResolver] wglGetProcAddress found but could not be converted to a function pointer");
            }
        }
        else if (m_glLib.IsOpen())
        {
            LOG_DEBUG("[GLResolver] wglGetProcAddress not found (GL library loaded but missing symbol)");
        }
    }
#else
    // GLX
    {
        void* sym = nullptr;

        // Prefer explicit library handles when we have them.
        if (m_glxLib.IsOpen())
        {
            sym = m_glxLib.GetSymbol("glXGetProcAddressARB");
            if (sym == nullptr)
            {
                sym = m_glxLib.GetSymbol("glXGetProcAddress");
            }
        }
        if (sym == nullptr && m_glLib.IsOpen())
        {
            sym = m_glLib.GetSymbol("glXGetProcAddressARB");
            if (sym == nullptr)
            {
                sym = m_glLib.GetSymbol("glXGetProcAddress");
            }
        }

        // Finally, try global lookup (host might already have GLX loaded).
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("glXGetProcAddressARB");
            if (sym == nullptr)
            {
                sym = DynamicLibrary::FindGlobalSymbol("glXGetProcAddress");
            }
        }

        if (sym != nullptr)
        {
            m_glxGetProcAddress = SymbolToFunction<GlxGetProcAddressFn>(sym);
            if (m_glxGetProcAddress == nullptr)
            {
                LOG_DEBUG("[GLResolver] glXGetProcAddress* found but could not be converted to a function pointer");
            }
        }
        else if (m_glxLib.IsOpen() || m_glLib.IsOpen())
        {
            LOG_DEBUG("[GLResolver] glXGetProcAddress* not found (GLX/GL loaded but missing symbol)");
        }
    }
#endif



#if defined(__APPLE__) && !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    // CGLGetCurrentContext (used for backend probing; optional)
    {
        void* sym = nullptr;
        if (m_glLib.IsOpen())
        {
            sym = m_glLib.GetSymbol("CGLGetCurrentContext");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("CGLGetCurrentContext");
        }
        m_cglGetCurrentContext = SymbolToFunction<GetCurrentContextFn>(sym);
    }
#endif
    LOG_DEBUG(std::string("[GLResolver] EGL  handle=") +
              std::to_string(reinterpret_cast<std::uintptr_t>(m_eglLib.Handle())) +
              " lib=\"" + m_eglLib.LoadedName() + "\"");

    LOG_DEBUG(std::string("[GLResolver] GL   handle=") +
              std::to_string(reinterpret_cast<std::uintptr_t>(m_glLib.Handle())) +
              " lib=\"" + m_glLib.LoadedName() + "\"");

    LOG_DEBUG(std::string("[GLResolver] GLX  handle=") +
              std::to_string(reinterpret_cast<std::uintptr_t>(m_glxLib.Handle())) +
              " lib=\"" + m_glxLib.LoadedName() + "\"");
}

void GLResolver::DetectBackend()
{
    // Detect current context provider
    // This is best-effort: on some platforms (e.g. macOS/CGL) it may report "none"

#ifdef __EMSCRIPTEN__
    m_backend = Backend::WebGL;
#else
    const CurrentContextProbe probe = ProbeCurrentContext(m_eglLib, m_glLib, m_glxLib);
    const Backend picked = PickCurrentBackend(probe);

    switch (picked)
    {
        case Backend::EGL:
            LOG_DEBUG("[GLResolver] Current context: EGL");
        break;
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
        case Backend::GLX:
            LOG_DEBUG("[GLResolver] Current context: GLX");
        break;
#endif
#ifdef _WIN32
        case Backend::WGL:
            LOG_DEBUG("[GLResolver] Current context: WGL");
        break;
#endif
#if defined(__APPLE__) && !defined(_WIN32)
        case Backend::CGL:
            LOG_DEBUG("[GLResolver] Current context: CGL");
        break;
#endif
        default:
            LOG_DEBUG("[GLResolver] Current context: (unknown, will try generic loader)");
        break;
    }

    m_backend = picked;

#endif // #ifdef __EMSCRIPTEN__
}

auto GLResolver::GladResolverThunk(const char* name) -> void*
{
    return Instance().GetProcAddress(name);
}

namespace {

/**
 * Resolves a GL function by name using GladResolverThunk.
 * Adapt external void* handle to GLAD type
 *
 * @param name GL function name.
 *
 * @return Function pointer as GLADapiproc.
 */
auto GladBridgeResolverThunk(const char* name) -> GLADapiproc
{
    return SymbolToFunction<GLADapiproc>(GLResolver::GladResolverThunk(name));
}

} // namespace

auto GLResolver::LoadGladUnlocked() -> bool
{
#ifndef USE_GLES
    const int result = gladLoadGL(&GladBridgeResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("[GLResolver] gladLoadGL() succeeded");
        return true;
    }
    LOG_FATAL(std::string("[GLResolver] gladLoadGL() failed (backend=") +
              BackendToString(m_backend) +
              ", egl='" + m_eglLib.LoadedName() + "', gl='" + m_glLib.LoadedName() +
              "', glx='" + m_glxLib.LoadedName() + "')");
    return false;
#else
    const int result = gladLoadGLES2(&GladBridgeResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("[GLResolver] gladLoadGLES2() succeeded");
        return true;
    }
#ifdef __EMSCRIPTEN__
    LOG_FATAL(std::string("[GLResolver] gladLoadGLES2() failed (backend=") +
              BackendToString(m_backend) + "')");
#else
    LOG_FATAL(std::string("[GLResolver] gladLoadGLES2() failed (backend=") +
          BackendToString(m_backend)
          +
          ", egl='" + m_eglLib.LoadedName() + "', gl='" + m_glLib.LoadedName() +
          "', glx='" + m_glxLib.LoadedName()
          + "')");
#endif

    return false;
#endif
}

auto GLResolver::ResolveUnlocked(const char* name,
                                 UserResolver userResolver,
                                 void* userData,
                                 Backend backend,
                                 EglGetProcAddressFn eglGetProcAddressFn,
                                 bool eglGetAllProcAddresses,
                                 bool allowEglProvider,
#ifndef _WIN32
                                 GlxGetProcAddressFn glxGetProcAddressFn,
                                 bool allowGlxProvider
#else
                                 WglGetProcAddressFn wglGetProcAddressFn,
                                 bool allowWglProvider
#endif
) const -> void*
{
    // 1) User resolver
    if (userResolver != nullptr)
    {
        void* ptr = userResolver(name, userData);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }

#ifdef __EMSCRIPTEN__
    // 2) Emscripten (WebGL)
    // Try to resolve using the function appropriate for the current context version first.
    void* ptr = nullptr;

    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
    int ctxMajor = 0;
    if (ctx != 0)
    {
        EmscriptenWebGLContextAttributes attrs;
        if (emscripten_webgl_get_context_attributes(ctx, &attrs) == EMSCRIPTEN_RESULT_SUCCESS)
        {
            ctxMajor = attrs.majorVersion;
        }
    }

    if (ctxMajor >= 2)
    {
        ptr = emscripten_webgl2_get_proc_address(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
        ptr = emscripten_webgl_get_proc_address(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }
    else
    {
        ptr = emscripten_webgl_get_proc_address(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
        ptr = emscripten_webgl2_get_proc_address(name);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }

    return nullptr;
#else

    // 2) Platform provider getProcAddress (preferred for extensions, GLVND dispatch)
    if (allowEglProvider && (backend == Backend::EGL || backend == Backend::None) && eglGetProcAddressFn != nullptr)
    {
        if (eglGetAllProcAddresses || ShouldUseEglGetProcAddressForName(name))
        {
            // eglGetProcAddress returns a function pointer type; convert only at the boundary.
            EglProc proc = eglGetProcAddressFn(name);
            if (proc != nullptr)
            {
                return FunctionToSymbol(proc);
            }
        }
    }

#ifndef _WIN32
#if !defined(__ANDROID__) && !defined(__APPLE__)
    if (allowGlxProvider && (backend == Backend::GLX || backend == Backend::None) && glxGetProcAddressFn != nullptr)
    {
        // GLX policy: only accept glXGetProcAddress* results for names that
        // look like extension entry points. Core symbols are resolved via direct exports
        // (libOpenGL/libGL) through the later global/library lookup path.
        //
        // Rationale: some GLX implementations may return a non-null pointer even for
        // unsupported or unknown symbols, which can crash when called.
        // See: https://dri.freedesktop.org/wiki/glXGetProcAddressNeverReturnsNULL/
        if (ShouldUseGlxGetProcAddressForName(name))
        {
            auto proc = glxGetProcAddressFn(reinterpret_cast<const unsigned char*>(name));
            if (proc != nullptr)
            {
                return FunctionToSymbol(proc);
            }
        }
    }
#endif
#else // #ifndef _WIN32
    if (allowWglProvider && (backend == Backend::WGL || backend == Backend::None) && wglGetProcAddressFn != nullptr)
    {
        PROC proc = wglGetProcAddressFn(name);
        if (proc != nullptr)
        {
            // wglGetProcAddress can return special sentinel values (1,2,3,-1) for core symbols.
            // Treat those as invalid and allow fallback to GetProcAddress/dlsym paths.
            const std::uintptr_t raw = FunctionToInteger(proc);
            if (raw != 1u && raw != 2u && raw != 3u && raw != std::numeric_limits<std::uintptr_t>::max())
            {
                // Prefer exports from opengl32.dll for core OpenGL 1.1 entry points.
                // In the wild, wglGetProcAddress may return a non-null pointer for some core symbols
                // that is not callable, so we check the export path first.
                // (Exports will be nullptr for extension/non-exported entry points, so this is cheap.)
                void* exportPtr = DynamicLibrary::FindGlobalSymbol(name);
                if (exportPtr != nullptr)
                {
                    return exportPtr;
                }
                return FunctionToSymbol(proc);
            }
        }
    }
#endif

    return nullptr;
#endif
}

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
