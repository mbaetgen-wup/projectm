// Cross-platform runtime GL/GLES loader using GLAD2 C API (non-MX).
//
// Provides a universal resolver to find GL function pointers.

#include "PlatformGLResolver.hpp"

#include "OpenGL.h"
#include "SOIL2/SOIL2_gl_bridge.h"
#include "SOIL2/SOIL2.h"

#include <Logging.hpp>

#include <array>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

namespace libprojectM {

namespace Renderer {

namespace Platform {

GLResolver::~GLResolver()
{
    SOIL_GL_Destroy();

    // make sure handles are released
    m_eglLib.Close();
    m_glLib.Close();
    m_glxLib.Close();
}

auto GLResolver::Instance() -> GLResolver&
{
    static GLResolver instance;
    return instance;
}

void GLResolver::SetBackendDefault()
{
    if (m_backend == Backend::None)
    {
#ifdef __EMSCRIPTEN__
        m_backend = Backend::WebGl;
#elif defined(USE_GLES)
        m_backend = Backend::EglGles;
#elif defined(_WIN32)
        m_backend = Backend::WglGl;
#else
        m_backend = Backend::GlxGl;
#endif
    }
}

auto GLResolver::Initialize(UserResolver resolver, void* userData) -> bool
{
    // Avoids holding m_mutex while calling into GLAD (prevents deadlocks),
    // Prevent concurrent Initialize()/Shutdown(),
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_loaded)
    {
        return true;
    }

    while (m_initializing)
    {
        m_initCv.wait(lock);
    }

    if (m_loaded)
    {
        return true;
    }

    m_initializing = true;

    m_userResolver = resolver;
    m_userData = userData;

    if (m_userResolver == nullptr)
    {
        // need to find source for gl functions
        // open libs and detect backend
#ifndef __EMSCRIPTEN__
        OpenNativeLibraries();
        ResolveProviderFunctions();
#endif
        DetectBackend();
    }
    else
    {
        // user resolver was provided
        // we *should* be able to get all gl functions from the resolver
        // using user resolver + global symbol fallback
        m_backend = Backend::UserResolver;
    }

    // Do not hold m_mutex while calling into GLAD.
    lock.unlock();

    const bool loaded = LoadGlad();

    lock.lock();

    if (loaded)
    {
        // set default in case detection failed, but loading succeeded
        SetBackendDefault();

        lock.unlock();

        // init SOIL2 gl functions
        SOIL_GL_SetResolver(&GLResolver::GladResolverThunk);
        SOIL_GL_Init();

        lock.lock();

        m_loaded = true;
    }

    m_initializing = false;
    m_initCv.notify_all();
    lock.unlock();

    return m_loaded;
}

void GLResolver::Shutdown()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    while (m_initializing)
    {
        m_initCv.wait(lock);
    }

    SOIL_GL_Destroy();

    m_loaded = false;
    m_backend = Backend::None;

    m_userResolver = nullptr;
    m_userData = nullptr;

    m_eglGetProcAddress = nullptr;
    m_glxGetProcAddress = nullptr;
    m_wglGetProcAddress = nullptr;

    m_eglLib.Close();
    m_glLib.Close();
    m_glxLib.Close();
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

auto GLResolver::GetProcAddress(const char* name) const -> GLapiproc
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return Resolve(name);
}

void GLResolver::OpenNativeLibraries()
{
    // Best-effort: mac or minimal EGL setups may fail to open

#ifdef _WIN32
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.dll", "EGL.dll", nullptr};
    static constexpr std::array<const char*, 4> kGlNames = {"opengl32.dll", "libGLESv2.dll", "libGLESv3.dll", nullptr};
#elif defined(__APPLE__)
    static constexpr std::array<const char*, 2> kEglNames = {"libEGL.dylib", nullptr};
    static constexpr std::array<const char*, 2> kGlNames = {"/System/Library/Frameworks/OpenGL.framework/OpenGL", nullptr};
#elif defined(__ANDROID__)
    // Android uses EGL + GLES (no desktop libGL / GLX)
    static constexpr std::array<const char*, 2> kEglNames = {"libEGL.so", nullptr};
    static constexpr std::array<const char*, 3> kGlNames = {
        "libGLESv3.so",
        "libGLESv2.so",
        nullptr
    };
#else
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.so.1", "libEGL.so", nullptr};
    static constexpr std::array<const char*, 4> kGlNames = {
        "libGL.so.1",       // legacy/compat umbrella (often provided by GLVND)
        "libOpenGL.so.0",   // GLVND OpenGL dispatcher (core gl* entry points)
        "libGL.so",
        nullptr
    };

    // Linux / GLVND note:
    // Some environments (especially minimal/container) may not ship libGL.so.1 but do ship GLVND libs.
    // Keep legacy libGL first for backwards compatibility, but fall back to GLVND-facing libs if needed.
    static constexpr std::array<const char*, 2> kGlxNames = {
        "libGLX.so.0",      // GLVND GLX dispatcher (glXGetProcAddress*)
        nullptr
    };
    m_glxLib.Open(kGlxNames.data());
#endif

    m_eglLib.Open(kEglNames.data());
    m_glLib.Open(kGlNames.data());
}

void GLResolver::ResolveProviderFunctions()
{
    // EGL
    if (m_eglLib.IsOpen())
    {
        void* sym = m_eglLib.GetSymbol("eglGetProcAddress");
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("eglGetProcAddress");
        }
        m_eglGetProcAddress = reinterpret_cast<GetProcFunc>(sym);
    }

    // GLX / WGL
#ifdef _WIN32
    if (m_glLib.IsOpen())
    {
        void* sym = m_glLib.GetSymbol("wglGetProcAddress");
        if (!sym)
        {
            sym = DynamicLibrary::FindGlobalSymbol("wglGetProcAddress");
        }
        m_wglGetProcAddress = reinterpret_cast<GetProcFunc>(sym);
#else
    if (m_glxLib.IsOpen())
    {
        // Try libs first
        void* sym = m_glxLib.GetSymbol("glXGetProcAddressARB");
        if (sym == nullptr)
        {
            sym = m_glxLib.GetSymbol("glXGetProcAddress");
        }
        if (sym == nullptr)
        {
            // Try both names via global lookup as well. Depending on which GL/GLX libs
            // are already present in the process, only one may be exported.
            sym = DynamicLibrary::FindGlobalSymbol("glXGetProcAddressARB");
            if (sym == nullptr)
            {
                sym = DynamicLibrary::FindGlobalSymbol("glXGetProcAddress");
            }
        }
        m_glxGetProcAddress = reinterpret_cast<GetProcFunc>(sym);
#endif
    }

    {
        std::array<char, 256> buf{};
        std::snprintf(buf.data(), buf.size(), "[GLResolver] Library handles: egl=%p gl=%p glx=%p",
            reinterpret_cast<void*>(m_eglLib.Handle()),
            reinterpret_cast<void*>(m_glLib.Handle()),
            reinterpret_cast<void*>(m_glxLib.Handle()));
        LOG_DEBUG(std::string(buf.data()));
    }

}

void GLResolver::DetectBackend()
{
    // Detect current context provider
    // This is best-effort: on some platforms (e.g. macOS/CGL) it may report "unknown"

#ifdef __EMSCRIPTEN__
    m_backend = Backend::WebGl;
#else
    const bool usingEgl = m_eglLib.IsOpen() && IsCurrentEgl(m_eglLib);

#ifndef _WIN32
    // Prefer GLX detection via libGLX when available (GLVND setups may load libOpenGL without GLX symbols).
    const bool usingGlx =
        (m_glxLib.IsOpen() && IsCurrentGlx(m_glxLib)) ||
        (m_glLib.IsOpen() && IsCurrentGlx(m_glLib));
#else
    const bool usingWgl = IsCurrentWgl();
#endif

    if (usingEgl)
    {
        LOG_DEBUG("[GLResolver] Current context: EGL");
        m_backend = Backend::EglGles;
        return;
    }

#ifndef _WIN32
    if (usingGlx)
    {
        LOG_DEBUG("[GLResolver] Current context: GLX");
        m_backend = Backend::GlxGl;
        return;
    }
#else
    if (usingWgl)
    {
        LOG_DEBUG("[GLResolver] Current context: WGL");
        m_backend = Backend::WglGl;
        return;
    }
#endif

    LOG_DEBUG("[GLResolver] Current context: (unknown, will try generic loader)");
    m_backend = Backend::None;
#endif // #ifdef __EMSCRIPTEN__

}

auto GLResolver::GladResolverThunk(const char* name) -> GLapiproc
{
    return Instance().Resolve(name);
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
    return reinterpret_cast<GLADapiproc>(GLResolver::GladResolverThunk(name));
}

}

auto GLResolver::LoadGlad() -> bool
{
    int result = 0;

#ifndef USE_GLES
    result = gladLoadGL(&gladBridgeResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("[GLResolver] gladLoadGL() succeeded");
        return true;
    }
    LOG_FATAL("[GLResolver] gladLoadGL() failed");
    return false;
#else
    result = gladLoadGLES2(&gladBridgeResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("[GLResolver] gladLoadGLES2() succeeded");
        return true;
    }
    LOG_FATAL("[GLResolver] gladLoadGLES2() failed");
    return false;
#endif
}

auto GLResolver::Resolve(const char* name) const -> GLapiproc
{
    if (name == nullptr)
    {
        return nullptr;
    }

    // 1) User resolver
    if (m_userResolver != nullptr)
    {
        if (void* ptr = m_userResolver(name, m_userData))
        {
            return ptr;
        }
    }

#ifdef __EMSCRIPTEN__
    // 2) Emscripten
    if (void* ptr = emscripten_webgl_get_proc_address(name))
    {
        return ptr;
    }
    if (void* ptr = emscripten_webgl2_get_proc_address(name))
    {
        return ptr;
    }
#else

    // 2) Platform provider getProcAddress (prefer for correctness, extensions, GLVND dispatch)
    if ((m_backend == Backend::EglGles || m_backend == Backend::None) && m_eglGetProcAddress != nullptr)
    {
        if (void* ptr = m_eglGetProcAddress(name))
        {
            return ptr;
        }
    }
    if ((m_backend == Backend::GlxGl || m_backend == Backend::None) && m_glxGetProcAddress != nullptr)
    {
        if (void* ptr = m_glxGetProcAddress(name))
        {
            return ptr;
        }
    }
    if ((m_backend == Backend::WglGl || m_backend == Backend::None) && m_wglGetProcAddress != nullptr)
    {
        if (void* ptr = m_wglGetProcAddress(name))
        {
            return ptr;
        }
    }

    // 3) Global symbol table (works if the process already linked/loaded GL libs)
    if (void* ptr = DynamicLibrary::FindGlobalSymbol(name))
    {
        return ptr;
    }

    // 4) Direct library symbol lookup
    if (m_eglLib.IsOpen())
    {
        if (void* ptr = m_eglLib.GetSymbol(name))
        {
            return ptr;
        }
    }
    if (m_glLib.IsOpen())
    {
        if (void* ptr = m_glLib.GetSymbol(name))
        {
            return ptr;
        }
    }
    if (m_glxLib.IsOpen())
    {
        if (void* ptr = m_glxLib.GetSymbol(name))
        {
            return ptr;
        }
    }
#endif

    return nullptr;
}

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
