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
#include <cstdint>
#include <cstdio>

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

GLResolver::~GLResolver()
{
    /*
     * Process-lifetime singleton: destructor runs during process teardown.
     * Do not call into GL or unload GL driver libraries here:
     *  - GL context may already be destroyed on this thread.
     *  - Other threads may still be running.
     * OS will reclaim mappings on exit.
     */
}

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

#ifndef _WIN32
    const bool hasGlxProvider = (m_glxGetProcAddress != nullptr);
#else
    const bool hasWglProvider = (m_wglGetProcAddress != nullptr);
#endif

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

auto GLResolver::Initialize(UserResolver resolver, void* userData) -> bool
{
    // Avoids holding m_mutex while calling into GLAD (prevents deadlocks),
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

    // need to find source for gl functions
    // open libs and detect backend
#ifndef __EMSCRIPTEN__
    OpenNativeLibraries();
    ResolveProviderFunctions();
#endif

    // precondition: caller must have a current context on this thread.
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

    // Do not hold m_mutex while calling into GLAD.
    lock.unlock();

    const bool loaded = LoadGlad();

    lock.lock();

    if (!loaded)
    {
        m_loaded = false;
    }
    else
    {
        // set default in case detection failed, but loading succeeded
        SetBackendDefault();

        lock.unlock();

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
            .WithRequireCoreProfile(true);
#endif

        const auto glDetails = glCheck.Check();

        lock.lock();

        LOG_INFO(std::string("[GLResolver] GL Info: ") +
                 GLContextCheck::FormatCompactLine(glDetails.info) +
                 " backend=\"" + BackendToString(m_backend) + "\"" +
                 " user_resolver=\"" + (m_userResolver != nullptr ? "yes" : "no") + "\"");
        if (!glDetails.success)
        {
            LOG_FATAL(std::string("[GLResolver] GL Check failed: ") + glDetails.reason);
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
    bool eglCurrent = false;
    bool glxCurrent = false;
    bool wglCurrent = false;
    bool cglCurrent = false;

    bool eglAvailable = false;
#if defined(_WIN32)
    bool wglAvailable = false;
#elif defined(__APPLE__)
    bool cglAvailable = false;
#else
    bool glxAvailable = false;
#endif

    // EGL (including ANGLE, GLES, and desktop EGL)
    if (m_eglLib.IsOpen())
    {
        eglAvailable = true;
        eglCurrent = IsCurrentEgl(m_eglLib);
    }
    else
    {
        // POSIX: if EGL is already linked/loaded by the host, this can still succeed without dlopen.
        // Windows: FindGlobalSymbol only checks main module/opengl32, so this is best-effort.
#if !defined(_WIN32)
        using EglGetCurrentContextFn = void* (*)();
        void* sym = DynamicLibrary::FindGlobalSymbol("eglGetCurrentContext");
        auto func = SymbolToFunction<EglGetCurrentContextFn>(sym);
        if (func != nullptr)
        {
            eglAvailable = true;
            eglCurrent = func() != nullptr;
        }
#endif
    }

    // GLX (Linux/Unix via libGLX/libGL or GLVND dispatch)
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
    if (m_glxLib.IsOpen())
    {
        glxAvailable = true;
        glxCurrent = IsCurrentGlx(m_glxLib);
    }
    if (m_glLib.IsOpen())
    {
        glxAvailable = true;
        glxCurrent = glxCurrent || IsCurrentGlx(m_glLib);
    }

    if (!glxAvailable)
    {
        // If GLX is already loaded/linked by the host, probe via global symbols (GLVND setups).
        using GlxGetCurrentContextFn = void* (*)();
        void* sym = DynamicLibrary::FindGlobalSymbol("glXGetCurrentContextARB");
        auto func = SymbolToFunction<GlxGetCurrentContextFn>(sym);
        if (func == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("glXGetCurrentContext");
            func = SymbolToFunction<GlxGetCurrentContextFn>(sym);
        }

        if (func != nullptr)
        {
            glxAvailable = true;
            glxCurrent = func() != nullptr;
        }
    }
#endif

    // WGL (Windows desktop GL)
#ifdef _WIN32
    // Note: ANGLE on Windows typically uses EGL; we probe that separately above.
    {
        const HMODULE glModule = ::GetModuleHandleA("opengl32.dll");
        wglAvailable = glModule != nullptr;
        wglCurrent = wglAvailable ? IsCurrentWgl() : false;
    }
#endif

    // CGL (macOS native OpenGL)
#if defined(__APPLE__) && !defined(_WIN32)
    {
        using CglGetCurrentContextFn = void* (*)();
        void* sym = nullptr;
        if (m_glLib.IsOpen())
        {
            sym = m_glLib.GetSymbol("CGLGetCurrentContext");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("CGLGetCurrentContext");
        }

        auto func = SymbolToFunction<CglGetCurrentContextFn>(sym);
        if (func != nullptr)
        {
            cglAvailable = true;
            cglCurrent = func() != nullptr;
        }
    }
#endif

    // If any backend reports a current context, the precondition is satisfied.
    if (eglCurrent || glxCurrent || wglCurrent || cglCurrent)
    {
        return true;
    }

    // Build a compact, actionable failure reason.
    std::string reason;

    if (eglAvailable)
    {
        reason += "EGL: no current context; ";
    }

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
    if (glxAvailable)
    {
        reason += "GLX: no current context; ";
    }
#endif

#ifdef _WIN32
    if (wglAvailable)
    {
        reason += "WGL: no current context; ";
    }
    else
    {
        reason += "WGL: opengl32.dll not loaded; ";
    }
#endif

#if defined(__APPLE__) && !defined(_WIN32)
    if (cglAvailable)
    {
        reason += "CGL: no current context; ";
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

    /*
     * Avoid holding m_mutex while calling user callbacks or driver/loader code.
     * Only hold m_mutex while reading internal state or using library handles.
     */
    std::unique_lock<std::mutex> lock(m_mutex);

    const UserResolver userResolver = m_userResolver;
    void* const userData = m_userData;
    const Backend backend = m_backend;

    const EglGetProcAddressFn eglFn = m_eglGetProcAddress;
#ifndef _WIN32
    const GlxGetProcAddressFn glxFn = m_glxGetProcAddress;
#else
    const WglGetProcAddressFn wglFn = m_wglGetProcAddress;
#endif

    lock.unlock();

#ifndef _WIN32
    void* resolved = ResolveUnlocked(name, userResolver, userData, backend, eglFn, glxFn);
#else
    void* resolved = ResolveUnlocked(name, userResolver, userData, backend, eglFn, wglFn);
#endif
    if (resolved != nullptr)
    {
        return resolved;
    }

    lock.lock();

    /* Global symbol table (works if the process already linked/loaded GL libs). */
    void* global = DynamicLibrary::FindGlobalSymbol(name);
    if (global != nullptr)
    {
        return global;
    }

    /* Direct library symbol lookup. */
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

#ifdef _WIN32
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.dll", "EGL.dll", nullptr};

    // Prefer GLES libraries when EGL is present (ANGLE / EGL-based GLES contexts).
    // Keep opengl32.dll as a fallback to allow WGL symbol discovery and global lookup.
    static constexpr std::array<const char*, 2> kGlDesktopNames = {"opengl32.dll", nullptr};
    static constexpr std::array<const char*, 4> kGlGlesNames = {"libGLESv3.dll", "libGLESv2.dll", "opengl32.dll", nullptr};
#elif defined(__APPLE__)
    // macOS native OpenGL uses CGL (OpenGL.framework). ANGLE (and other portability layers)
    // commonly provide EGL/GLES dylibs in the application bundle / @rpath.
    static constexpr std::array<const char*, 4> kEglNames = {"libEGL.dylib", "libEGL.1.dylib", "EGL", nullptr};

    static constexpr std::array<const char*, 2> kGlCglNames = {
        "/System/Library/Frameworks/OpenGL.framework/OpenGL",
        nullptr
    };

    static constexpr std::array<const char*, 4> kGlGlesNames = {
        "libGLESv3.dylib",
        "libGLESv2.dylib",
        "/System/Library/Frameworks/OpenGL.framework/OpenGL",
        nullptr
    };
#elif defined(__ANDROID__)
    // Android uses EGL + GLES (no desktop libGL / GLX)
    static constexpr std::array<const char*, 2> kEglNames = {"libEGL.so", nullptr};
    static constexpr std::array<const char*, 3> kGlNames = { "libGLESv3.so", "libGLESv2.so", nullptr};
#else
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.so.1", "libEGL.so", nullptr};
    static constexpr std::array<const char*, 6> kGlNames = {
        "libGL.so.1",     // legacy/compat umbrella (often provided by GLVND)
        "libGL.so.0",     // sometimes shipped as .so.0
        "libOpenGL.so.1", // GLVND OpenGL dispatcher (core gl* entry points)
        "libOpenGL.so.0", // older GLVND soname
        "libGL.so",
        nullptr};

    // Linux / GLVND note:
    // Some environments (especially minimal/container) may not ship libGL.so.1 but do ship GLVND libs.
    // Keep legacy libGL first for backwards compatibility, but fall back to GLVND-facing libs if needed.
    static constexpr std::array<const char*, 3> kGlxNames = {
        "libGLX.so.1", // GLVND GLX dispatcher (glXGetProcAddress*)
        "libGLX.so.0", // older GLVND soname
        nullptr};
    const bool glxOpened = m_glxLib.Open(kGlxNames.data());
    if (!glxOpened)
    {
        LOG_DEBUG(std::string("[GLResolver] Failed to open GLX library: ") + m_glxLib.LastError());
    }
#endif

    const bool eglOpened = m_eglLib.Open(kEglNames.data());
    if (eglOpened == false)
    {
        LOG_DEBUG(std::string("[GLResolver] Failed to open EGL library: ") + m_eglLib.LastError());
    }

#ifdef _WIN32
    const char* const* glNames = nullptr;
#if defined(USE_GLES)
    glNames = kGlGlesNames.data();
#else
    glNames = eglOpened ? kGlGlesNames.data() : kGlDesktopNames.data();
#endif
    const bool glOpened = m_glLib.Open(glNames);
#elif defined(__APPLE__)
    const char* const* glNames = nullptr;
#if defined(USE_GLES)
    glNames = kGlGlesNames.data();
#else
    glNames = eglOpened ? kGlGlesNames.data() : kGlCglNames.data();
#endif
    const bool glOpened = m_glLib.Open(glNames);
#else
    const bool glOpened = m_glLib.Open(kGlNames.data());
#endif
    if (!glOpened)
    {
        LOG_DEBUG(std::string("[GLResolver] Failed to open GL library: ") + m_glLib.LastError());
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
    bool usingEgl = false;

    if (m_eglLib.IsOpen())
    {
        usingEgl = IsCurrentEgl(m_eglLib);
    }
#if !defined(_WIN32)
    else
    {
        // POSIX: detect EGL even if the host preloaded/linked EGL and dlopen() failed.
        using EglGetCurrentContextFn = void* (*)();
        void* sym = DynamicLibrary::FindGlobalSymbol("eglGetCurrentContext");
        auto func = SymbolToFunction<EglGetCurrentContextFn>(sym);
        if (func != nullptr)
        {
            usingEgl = (func() != nullptr);
        }
    }
#endif

#ifndef _WIN32
    // Prefer GLX detection via libGLX when available (GLVND setups may load libOpenGL without GLX symbols).
    bool usingGlx =
        (m_glxLib.IsOpen() && IsCurrentGlx(m_glxLib)) ||
        (m_glLib.IsOpen() && IsCurrentGlx(m_glLib));

    if (!usingGlx)
    {
        // Probe via global symbols when the host has GLX loaded but we did not dlopen() it.
        using GlxGetCurrentContextFn = void* (*)();
        void* sym = DynamicLibrary::FindGlobalSymbol("glXGetCurrentContextARB");
        auto func = SymbolToFunction<GlxGetCurrentContextFn>(sym);
        if (func == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("glXGetCurrentContext");
            func = SymbolToFunction<GlxGetCurrentContextFn>(sym);
        }
        if (func != nullptr)
        {
            usingGlx = func() != nullptr;
        }
    }
#else
    const bool usingWgl = IsCurrentWgl();
#endif

    if (usingEgl)
    {
        LOG_DEBUG("[GLResolver] Current context: EGL");
        m_backend = Backend::EGL;
        return;
    }

#ifndef _WIN32
    if (usingGlx)
    {
        LOG_DEBUG("[GLResolver] Current context: GLX");
        m_backend = Backend::GLX;
        return;
    }
#else
    if (usingWgl)
    {
        LOG_DEBUG("[GLResolver] Current context: WGL");
        m_backend = Backend::WGL;
        return;
    }
#endif

    LOG_DEBUG("[GLResolver] Current context: (unknown, will try generic loader)");
    m_backend = Backend::None;
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
auto gladBridgeResolverThunk(const char* name) -> GLADapiproc
{
    return SymbolToFunction<GLADapiproc>(GLResolver::GladResolverThunk(name));
}

} // namespace

auto GLResolver::LoadGlad() -> bool
{
#ifndef USE_GLES
    const int result = gladLoadGL(&gladBridgeResolverThunk);
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
    const int result = gladLoadGLES2(&gladBridgeResolverThunk);
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
#ifndef _WIN32
                                 GlxGetProcAddressFn glxGetProcAddressFn
#else
                                 WglGetProcAddressFn wglGetProcAddressFn
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
    // 2) Emscripten
    void* ptr = emscripten_webgl_get_proc_address(name);
    if (ptr != nullptr)
    {
        return ptr;
    }
    ptr = emscripten_webgl2_get_proc_address(name);
    if (ptr != nullptr)
    {
        return ptr;
    }
    return nullptr;
#else

    // 2) Platform provider getProcAddress (prefer for correctness, extensions, GLVND dispatch)
    if ((backend == Backend::EGL || backend == Backend::None) && eglGetProcAddressFn != nullptr)
    {
        // eglGetProcAddress returns a function pointer type; convert only at the boundary.
        EglProc proc = eglGetProcAddressFn(name);
        if (proc != nullptr)
        {
            return FunctionToSymbol(proc);
        }
    }

#ifndef _WIN32
    if ((backend == Backend::GLX || backend == Backend::None) && glxGetProcAddressFn != nullptr)
    {
        auto proc = glxGetProcAddressFn(reinterpret_cast<const unsigned char*>(name));
        if (proc != nullptr)
        {
            return FunctionToSymbol(proc);
        }
    }
#else
    if ((backend == Backend::WGL || backend == Backend::None) && wglGetProcAddressFn != nullptr)
    {
        PROC proc = wglGetProcAddressFn(name);
        if (proc != nullptr)
        {
            // wglGetProcAddress can return special sentinel values (1,2,3,-1) for core symbols.
            // Treat those as invalid and allow fallback to GetProcAddress/dlsym paths.
            const std::uintptr_t raw = FunctionToInteger(proc);
            if (raw != 1u && raw != 2u && raw != 3u && raw != static_cast<std::uintptr_t>(~0u))
            {
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
