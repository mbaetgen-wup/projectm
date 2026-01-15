#pragma once

#include "PlatformLoader.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace libprojectM
{
namespace Renderer
{
namespace Platform
{

/**
 * @brief Backend describing which API/provider the current context appears to be using.
 */
enum class Backend : std::uint8_t
{
    /**
     * Backend detection was not successful or no specific loader is needed (e.g. Apple).
     */
    None = 0,

    /**
     * Detected EGL backend (GL build), or GLES backend (ENABLE_GLES=ON build).
     */
    Egl = 1,

    /**
     * Detected GLX backend (GL build only).
     */
    Glx = 2,

    /**
     * Detected WGL backend (GL build only).
     */
    Wgl = 3,

    /**
     * WegGl proc resolver (Emscripten only).
     */
    WebGl = 4
};

/**
 * @brief Optional user resolver callback.
 *
 * If provided, it is consulted first when resolving procedure addresses.
 * Return nullptr to allow the resolver to continue probing.
 */
using UserResolver = void* (*)(const char* name, void* userData);

/**
 * @brief Cross-platform runtime GL/GLES resolver.
 *
 * This resolver:
 *  - Supports: EGL, GLES, GLX, WGL, WebGL or a user supplied resolver.
 *  - Platforms: Android, Emscripten, Linux, Mac, Windows.
 *  - Must be initialized after a GL/GLES context has been created and made current.
 *  - Probes for EGL/GLX/WGL by checking for a current context.
 *  - Uses GLAD2 non-MX entrypoints (gladLoadGL / gladLoadGLES2) via a universal resolver.
 *  - Resolves symbols using the following order (GL/GLES):
 *      1) User resolver callback (if any)
 *      2) Platform provider eglGetProcAddress / glXGetProcAddress* / wglGetProcAddress (when available)
 *      3) Global symbol table (RTLD_DEFAULT / main module)
 *      4) Symbols from opened libEGL / libGL / opengl32
 *  - Resolves symbols using the following order (Emscripten):
 *      1) User resolver callback (if any)
 *      2) emscripten_webgl_get_proc_address()
 */
class GLResolver
{
public:
    /**
     * Opaque handle for gl functions.
     */
    using GLapiproc = void*;

    GLResolver() = default;
    ~GLResolver();

    GLResolver(const GLResolver&) = delete;
    auto operator=(const GLResolver&) -> GLResolver& = delete;
    GLResolver(GLResolver&&) = delete;
    auto operator=(GLResolver&&) -> GLResolver& = delete;

    /**
     * @brief Returns the process-wide resolver instance.
     */
    static auto Instance() -> GLResolver&;

    /**
     * @brief Initializes the resolver.
     * This method has to be called at least once for each resolver instance before it is used.
     * May be called multiple times, initialization is done if needed only.
     *
     * @param resolver Optional user resolver callback.
     * @param userData Optional user pointer passed to resolver.
     * @return true if GLAD successfully loaded a backend, false otherwise.
     */
    auto Initialize(UserResolver resolver = nullptr, void* userData = nullptr) -> bool;

    /**
     * @brief Shuts down the resolver and releases library handles.
     */
    void Shutdown();

    /**
     * @brief Returns true if the resolver was successfully initialized.
     */
    auto IsLoaded() const -> bool;

    /**
     * @brief Returns the currently detected backend.
     */
    auto CurrentBackend() const -> Backend;

    /**
     * @brief Resolves a function pointer by consulting all sources in priority order.
     *
     * @param name Function name.
     * @return Procedure address or nullptr.
     */
    auto GetProcAddress(const char* name) const -> GLapiproc;

    /**
     * @brief Resolves a function pointer by consulting all sources in priority order from a static context.
     *
     * @param name Function name.
     * @return Procedure address or nullptr.
     */
    static auto GladResolverThunk(const char* name) -> GLapiproc;

private:
    void OpenNativeLibraries();
    void ResolveProviderFunctions();
    void DetectBackend();
    void SetBackendDefault();
    auto LoadGlad() -> bool;

    /* Provider function types (kept private to avoid header pollution elsewhere). */
    using EglGetProcAddressFn = void* (*)(const char*);

#ifndef _WIN32
    /* glXGetProcAddress/glXGetProcAddressARB return a function pointer. */
    using GlxGetProcAddressFn = void (*(*)(const unsigned char*))(void);
#else
    using WglGetProcAddressFn = PROC (WINAPI*)(LPCSTR);
#endif

    auto ResolveUnlocked(const char* name,
                         UserResolver userResolver,
                         void* userData,
                         Backend backend,
                         EglGetProcAddressFn eglGetProcAddressFn,
#ifndef _WIN32
                         GlxGetProcAddressFn glxGetProcAddressFn
#else
                         WglGetProcAddressFn wglGetProcAddressFn
#endif
                         ) const -> GLapiproc;

    mutable std::mutex m_mutex;                   //!< Mutex to synchronize initialization and access.
    bool m_loaded{false};                         //!< True if the resolver is initialized.
    bool m_initializing{false};                   //!< True while an Initialize() attempt is in-flight.
    mutable std::condition_variable m_initCv;     //!< Signals completion of Initialize()/Shutdown().
    Backend m_backend{ Backend::None };           //!< Detected GL backend.

    UserResolver m_userResolver{nullptr};         //!< User provided function resolver. Optional, may be null.
    void* m_userData{nullptr};                    //!< User data to pass to user provided function resolver.

    DynamicLibrary m_eglLib;                      //!< EGL library handle. Optional, may be empty.
    DynamicLibrary m_glLib;                       //!< GL library handle. Optional, may be empty.
    DynamicLibrary m_glxLib;                      //!< GLX library handle. Optional, may be empty.

    EglGetProcAddressFn m_eglGetProcAddress{nullptr}; //!< eglGetProcAddress handle.
#ifndef _WIN32
    GlxGetProcAddressFn m_glxGetProcAddress{nullptr}; //!< glXGetProcAddress* handle.
#else
    WglGetProcAddressFn m_wglGetProcAddress{nullptr}; //!< wglGetProcAddress handle.
#endif
};

/**
 * @brief Converts a Backend enum value to a human-readable string.
 *
 * @param backend Backend enum value.
 * @return Constant string representation of the backend.
 */
inline auto BackendToString(Backend backend) -> const char*
{
    switch (backend)
    {
        case Backend::None:
        {
            return "None";
        }
        case Backend::Egl:
        {
            return "EGL";
        }
        case Backend::Glx:
        {
            return "GLX";
        }
        case Backend::Wgl:
        {
            return "WGL";
        }
        case Backend::WebGl:
        {
            return "WebGL";
        }
        default:
        {
            return "Unknown";
        }
    }
}

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
