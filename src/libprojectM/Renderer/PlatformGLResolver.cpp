// Cross-platform runtime GL/GLES loader using GLAD2 C API (non-MX).
//
// Provides a universal resolver to find GL function pointers.

#include "PlatformGLResolver.hpp"

#include "OpenGL.h"
#include <Logging.hpp>

#include <array>
#include <cstdio>

namespace libprojectM {

namespace Renderer {

namespace Platform {

auto GLResolver::Instance() -> GLResolver&
{
    static GLResolver instance;
    return instance;
}

auto GLResolver::Initialize(UserResolver resolver, void* userData) -> bool
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    if (m_loaded)
    {
        return true;
    }

    m_userResolver = resolver;
    m_userData = userData;

    OpenNativeLibraries();
    ResolveProviderFunctions();
    DetectBackend();

    if (LoadGlad())
    {
        // If detection failed, but loading succeeded
        // fall back to a sensible default
        if (m_backend == Backend::None)
        {
#ifdef _WIN32
            m_backend = Backend::WglGl;
#else
            m_backend = Backend::GlxGl;
#endif
        }

        m_loaded = true;
        return true;
    }

    return false;
}

void GLResolver::Shutdown()
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    m_loaded = false;
    m_backend = Backend::None;

    m_userResolver = nullptr;
    m_userData = nullptr;

    m_eglGetProcAddress = nullptr;
    m_glxGetProcAddress = nullptr;
    m_wglGetProcAddress = nullptr;

    m_eglLib.Close();
    m_glLib.Close();
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
    // NOTE: This method is used during GLAD loading. Avoid taking the mutex here to
    // prevent deadlocks if GLAD calls back into us while Initialize() is holding the lock
    return Resolve(name);
}

void GLResolver::OpenNativeLibraries()
{
    // Best-effort: macOS or minimal EGL setups may fail to open

#ifdef _WIN32
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.dll", "EGL.dll", nullptr};
    static constexpr std::array<const char*, 2> kGlNames = {"opengl32.dll", nullptr};
#elif defined(__APPLE__)
    static constexpr std::array<const char*, 2> kEglNames = {"libEGL.dylib", nullptr};
    static constexpr std::array<const char*, 2> kGlNames = {"/System/Library/Frameworks/OpenGL.framework/OpenGL", nullptr};
#else
    static constexpr std::array<const char*, 3> kEglNames = {"libEGL.so.1", "libEGL.so", nullptr};
    static constexpr std::array<const char*, 3> kGlNames = {"libGL.so.1", "libGL.so", nullptr};
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
    if (m_glLib.IsOpen())
    {
#ifdef _WIN32
        void* sym = m_glLib.GetSymbol("wglGetProcAddress");
        if (!sym)
        {
            sym = DynamicLibrary::FindGlobalSymbol("wglGetProcAddress");
        }
        m_wglGetProcAddress = reinterpret_cast<GetProcFunc>(sym);
#else
        void* sym = m_glLib.GetSymbol("glXGetProcAddressARB");
        if (sym == nullptr)
        {
            sym = m_glLib.GetSymbol("glXGetProcAddress");
        }
        if (sym == nullptr)
        {
            sym = DynamicLibrary::FindGlobalSymbol("glXGetProcAddress");
        }
        m_glxGetProcAddress = reinterpret_cast<GetProcFunc>(sym);
#endif
    }

     {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[PlatformGLResolver] Library handles: egl=%p gl=%p",
            reinterpret_cast<void*>(m_eglLib.Handle()),
            reinterpret_cast<void*>(m_glLib.Handle()));
        LOG_DEBUG(buf);
    }

}

void GLResolver::DetectBackend()
{
    // Detect current context provider
    // This is best-effort: on some platforms (e.g. macOS/CGL) it may report "unknown"

    const bool usingEgl = m_eglLib.IsOpen() && IsCurrentEgl(m_eglLib);

#ifndef _WIN32
    const bool usingGlx = m_glLib.IsOpen() && IsCurrentGlx(m_glLib);
#else
    const bool usingWgl = IsCurrentWgl();
#endif

    if (usingEgl)
    {
        LOG_DEBUG("[PlatformGLResolver] current context: EGL");
        m_backend = Backend::EglGles;
        return;
    }

#ifndef _WIN32
    if (usingGlx)
    {
        LOG_DEBUG("[PlatformGLResolver] current context: GLX");
        m_backend = Backend::GlxGl;
        return;
    }
#else
    if (usingWgl)
    {
        LOG_DEBUG("[PlatformGLResolver] current context: WGL");
        m_backend = Backend::WglGl;
        return;
    }
#endif

    LOG_DEBUG("[PlatformGLResolver] current context: (unknown, will try generic loader)");
    m_backend = Backend::None;
}

auto GLResolver::GladResolverThunk(const char* name) -> GLapiproc
{
    return Instance().Resolve(name);
}

namespace {
// adapt external void* handle to GLAD type
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
        LOG_DEBUG("[PlatformGLResolver] gladLoadGL() succeeded");
        return true;
    }
    LOG_FATAL("[PlatformGLResolver] gladLoadGL() failed");
    return false;
#else
    result = gladLoadGLES2(&gladBridgeResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("[PlatformGLResolver] gladLoadGLES2() succeeded");
        return true;
    }
    LOG_FATAL("[PlatformGLResolver] gladLoadGLES2() failed");
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

    // 2) Global symbol table
    if (void* ptr = DynamicLibrary::FindGlobalSymbol(name))
    {
        return ptr;
    }

    // 3) Platform provider getProcAddress
    if (m_eglGetProcAddress != nullptr)
    {
        if (void* ptr = m_eglGetProcAddress(name))
        {
            return ptr;
        }
    }
    if (m_glxGetProcAddress != nullptr)
    {
        if (void* ptr = m_glxGetProcAddress(name))
        {
            return ptr;
        }
    }
    if (m_wglGetProcAddress != nullptr)
    {
        if (void* ptr = m_wglGetProcAddress(name))
        {
            return ptr;
        }
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

    return nullptr;
}

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
