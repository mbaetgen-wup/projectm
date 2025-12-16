// Cross-platform runtime GL/GLES loader using GLAD2 C API (non-MX).
//
// Only forward declares the GLAD loader entrypoints (gladLoadGL / gladLoadGLES2) and provides
// a universal resolver.

#include "CrossGlLoader.hpp"

#include <Logging.hpp>

#include <array>
#include <cstdio>

// forward declare glad interfaces to contain dependency to this cpp
namespace {

using GladLoadFunc = void* (*) (const char* name);

extern "C" {
#ifndef USE_GLES
int gladLoadGL(GladLoadFunc load);
#else
int gladLoadGLES2(GladLoadFunc load);
#endif
}

} // namespace

namespace libprojectM {
namespace Renderer {

auto CrossGlLoader::Instance() -> CrossGlLoader&
{
    static CrossGlLoader instance;
    return instance;
}

auto CrossGlLoader::Initialize(UserResolver resolver, void* userData) -> bool
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

    if (LoadViaGlad())
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

void CrossGlLoader::Shutdown()
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

auto CrossGlLoader::IsLoaded() const -> bool
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_loaded;
}

auto CrossGlLoader::CurrentBackend() const -> Backend
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_backend;
}

auto CrossGlLoader::GetProcAddress(const char* name) const -> void*
{
    // NOTE: This method is used during GLAD loading. Avoid taking the mutex here to
    // prevent deadlocks if GLAD calls back into us while Initialize() is holding the lock
    return Resolve(name);
}

void CrossGlLoader::OpenNativeLibraries()
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

void CrossGlLoader::ResolveProviderFunctions()
{
    // EGL
    if (m_eglLib.IsOpen())
    {
        void* sym = m_eglLib.GetSymbol("eglGetProcAddress");
        if (sym == nullptr)
        {
            sym = Platform::DynamicLibrary::FindGlobalSymbol("eglGetProcAddress");
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
            sym = Platform::DynamicLibrary::FindGlobalSymbol("wglGetProcAddress");
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
            sym = Platform::DynamicLibrary::FindGlobalSymbol("glXGetProcAddress");
        }
        m_glxGetProcAddress = reinterpret_cast<GetProcFunc>(sym);
#endif
    }

    LOG_DEBUG([&] {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "CrossGlLoader: egl=%p gl=%p",
            reinterpret_cast<void*>(m_eglLib.Handle()),
            reinterpret_cast<void*>(m_glLib.Handle()));
        return buf;
    }());

}

void CrossGlLoader::DetectBackend()
{
    // Detect current context provider
    // This is best-effort: on some platforms (e.g. macOS/CGL) it may report "unknown"

    const bool usingEgl = m_eglLib.IsOpen() && IsCurrentEgl(m_eglLib);

#ifndef _WIN32
    const bool usingGlx = m_glLib.IsOpen() && IsCurrentGlx(m_glLib);
#else
    const bool usingWgl = Platform::IsCurrentWgl();
#endif

    if (usingEgl)
    {
        LOG_DEBUG("CrossGlLoader: current context: EGL");
        m_backend = Backend::EglGles;
        return;
    }

#ifndef _WIN32
    if (usingGlx)
    {
        LOG_DEBUG("CrossGlLoader: current context: GLX");
        m_backend = Backend::GlxGl;
        return;
    }
#else
    if (usingWgl)
    {
        LOG_DEBUG("CrossGlLoader: current context: WGL");
        m_backend = Backend::WglGl;
        return;
    }
#endif

    LOG_DEBUG("CrossGlLoader: current context: (unknown, will try generic loader)");
    m_backend = Backend::None;
}

auto CrossGlLoader::GladResolverThunk(const char* name) -> void*
{
    return Instance().Resolve(name);
}

auto CrossGlLoader::LoadViaGlad() -> bool
{
    int result = 0;

#ifndef USE_GLES
    result = gladLoadGL(&CrossGlLoader::GladResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("CrossGlLoader: gladLoadGL() succeeded");
        return true;
    }
    LOG_DEBUG("CrossGlLoader: gladLoadGL() failed");
    return false;
#else
    result = gladLoadGLES2(&CrossGlLoader::GladResolverThunk);
    if (result != 0)
    {
        LOG_DEBUG("CrossGlLoader: gladLoadGLES2() succeeded");
        return true;
    }
    LOG_DEBUG("CrossGlLoader: gladLoadGLES2() failed");
    return false;
#endif
}

auto CrossGlLoader::Resolve(const char* name) const -> void*
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
    if (void* ptr = Platform::DynamicLibrary::FindGlobalSymbol(name))
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

} // namespace Renderer
} // namespace libprojectM
