#pragma once

#include "PlatformGLContextCheck.hpp"
#include "PlatformLoader.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

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
    EGL = 1,

    /**
     * Detected GLX backend (GL build only).
     */
    GLX = 2,

    /**
     * Detected WGL backend (GL build only).
     */
    WGL = 3,

    /**
     * WebGL proc resolver (Emscripten only).
     */
    WebGL = 4,

    /**
     * MacOS native CGL.
     */
    CGL = 5
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
 *
 *  - Supports backends and wrappers: ANGLE, CGL, EGL (incl. GLES), GLX, libGLVND, WGL, WebGL, or a user supplied resolver.
 *
 *  - Supports platforms: Android, Emscripten, Linux, macOS, Windows.
 *
*   - Uses a compile-time decision to select one of the GL APIs:
 *    - When USE_GLES is defined, this resolver loads OpenGL ES entrypoints via gladLoadGLES2()
 *      and requires a current OpenGL ES/WebGL/EGL+GLES context.
 *    - Otherwise, it loads desktop OpenGL entrypoints via gladLoadGL() and requires a current
 *      desktop OpenGL context.
 *    - ANGLE typically exposes GLES via EGL, so it is expected to be used with ENABLE_GLES=ON builds.
 *
 *  - Must be initialized after a GL/GLES context has been created and made current on the calling thread.
 *
 *  - Detects the active backend by probing for a current context:
 *      - EGL via eglGetCurrentContext
 *      - GLX via glXGetCurrentContext
 *      - WGL via wglGetCurrentContext
 *      - macOS native falls back to CGL/NSOpenGL (no provider GetProcAddress)
 *
 *  - Uses GLAD2 non-MX entrypoints (gladLoadGL / gladLoadGLES2) with a single universal resolver.
 *
 *  - Supports libGLVND dispatch on Linux (libOpenGL.so + libGLX.so).
 *
 *  - Supports ANGLE when libEGL/libGLESv2 are present.
 *
 *  - Resolves symbols in the following order (native GL / GLES):
 *      1) User resolver callback (if provided)
 *      2) Platform provider:
 *           - eglGetProcAddress (EGL / ANGLE)
 *             (Note: on older EGL implementations/spec interpretations, eglGetProcAddress
 *              may not return addresses for all core functions; this resolver therefore
 *              also falls back to direct exports from loaded GL/GLES libraries.)
 *           - glXGetProcAddress / glXGetProcAddressARB (GLX / GLVND)
 *           - wglGetProcAddress (WGL)
 *      3) Global symbol table (RTLD_DEFAULT / main module)
 *      4) Symbols from explicitly opened libraries (libEGL, libGL, OpenGL.framework, opengl32)
 *
 *  - Resolves symbols in the following order (Emscripten):
 *      1) User resolver callback (if provided)
 *      2) emscripten_webgl_get_proc_address / emscripten_webgl2_get_proc_address
 *
 * @note On Emscripten/WebGL, procedure lookup support depends on the toolchain and export settings.
 *       (todo) Some entrypoints may not be discoverable via *_get_proc_address and may require static linkage
 *       and/or explicit exports in the build configuration.
 *
 *  ## Loader Overview
 *
 *  GLResolver::Initialize(userResolver?, userData?)
 *    |
 *   #ifndef __EMSCRIPTEN__
 *    |
 *    |-- OpenNativeLibraries()
 *    |     |
 *    |     +-- try open EGL library (best-effort)
 *    |     |      Windows:  libEGL.dll | EGL.dll
 *    |     |      macOS:    libEGL.dylib | libEGL.1.dylib | EGL
 *    |     |      Linux:    libEGL.so.1 | libEGL.so
 *    |     |
 *    |     +-- try open GL library (best-effort)
 *    |     |      Windows:  opengl32.dll
 *    |     |      macOS:    /System/.../OpenGL.framework/OpenGL
 *    |     |      Linux:    libGL.so.1 | libGL.so.0 | libOpenGL.so.1 | libOpenGL.so.0 | libGL.so
 *    |     |
 *    |     +-- (Linux only) try open GLX library (best-effort)
 *    |            libGLX.so.1 | libGLX.so.0
 *    |
 *    |-- ResolveProviderFunctions()
 *    |     |
 *    |     +-- EGL:
 *    |     |     - resolve eglGetProcAddress (from EGL lib or global symbols)
 *    |     |     - resolve eglGetCurrentContext (for probing)
 *    |     |     - detect EGL_KHR_get_all_proc_addresses /
 *    |     |              EGL_KHR_client_get_all_proc_addresses via eglQueryString
 *    |     |
 *    |     +-- GLX (Linux/Unix):
 *    |     |     - resolve glXGetProcAddressARB/glXGetProcAddress
 *    |     |
 *    |     +-- WGL (Windows):
 *    |           - resolve wglGetProcAddress
 *    |
 *    |     +-- CGL (macOS):
 *    |           - no provider functions
 * #endif
 *    |
 *    |-- Precondition: HasCurrentContext()
 *    |     |
 *    |     +-- ProbeCurrentContext() checks:
 *    |            EGL: eglGetCurrentContext() != nullptr                           ? (Android/Linux/macOS/Windows)
 *    |            GLX: glXGetCurrentContext() != nullptr                           ? (X11, not Android/macOS/Windows)
 *    |            WGL: wglGetCurrentContext() != nullptr                           ? (Windows)
 *    |            CGL: CGLGetCurrentContext() != nullptr                           ? (macOS)
 *    |            WebGL: emscripten_webgl_get_current_context() != nullptr         ? (Emscripten)
 *    |
 *    |-- DetectBackend()
 *    |     |
 *    |     +-- PickCurrentBackend priority:
 *    |            if EGL   current -> Backend::EGL          (Android/Linux/macOS/Windows)
 *    |            if WGL   current -> Backend::WGL          (Windows)
 *    |            if CGL   current -> Backend::CGL          (macOS)
 *    |            if GLX   current -> Backend::GLX          (Linux, not Android)
 *    |            if WebGL current -> Backend::WebGL        (Emscripten)
 *    |            else             -> Backend::None         (Unknown)
 *    |
 *    |-- gladLoadGL( GladBridgeResolverThunk )
 *    |        (GLAD calls back into GLResolver::GetProcAddress)
 *    |
 *    |-- SetBackendDefault()   (only if backend still None after load)
 *    |     |
 *    |     +-- macOS special-case:
 *    |           if no EGL provider available -> default Backend::CGL
 *    |     +-- otherwise: leave as None unless unambiguous
 *    |
 *    |-- CheckGLRequirementsUnlocked()
 *    |     |
 *    |     +-- requires: OpenGL >= 3.3
 *    |
 *    +-- if ready:
 *          SOIL_GL_SetResolver(&GLResolver::GladResolverThunk)
 *          SOIL_GL_Init()

 *  ## Resolver Overview
 *
 * GLResolver::GetProcAddress(name)
 *    |
 *    |-- Provider gating:
 *    |     if backend == None:
 *    |         probe current context *now* and allow only matching provider(s)
 *    |         (prevents cross-stack pointer returns in mixed-stack processes)
 *    |     else:
 *    |         allow only provider that matches backend
 *    |
 *    +--> (1) User resolver callback
 *    |
 *    +--> (2) WebGL providers [Emscripten only] --> DONE
 *    |   --OR--
 *    +--> (2) Provider GetProcAddress (backend-aware) [not Emscripten]:
 *    |      |
 *    |      +-- EGL path (if allowed, and backend is EGL or None):
 *    |      |     if eglGetAllProcAddresses == true
 *    |      |         OR name looks like an extension (suffix ARB/EXT/KHR/OES/NV/...):
 *    |      |            try eglGetProcAddress(name)
 *    |      |
 *    |      +-- GLX path (Linux, if allowed, and backend is GLX or None):
 *    |      |     POLICY: only call glXGetProcAddress* if:
 *    |      |         - name starts with "glX"  OR
 *    |      |         - name looks like an extension (ARB/EXT/KHR/OES/...)
 *    |      |
 *    |      +-- WGL path (Windows, if allowed, and backend is WGL or None):
 *    |            try wglGetProcAddress(name)
 *    |            if not sentinel {1,2,3,-1}
 *    |
 *    +--> (3) Global symbol table lookup
 *    |      |
 *    |      +-- POSIX: dlsym(RTLD_DEFAULT, name)
 *    |      +-- Windows: GetProcAddress(main exe), then known EGL/GLES modules, then opengl32.dll module
 *    |
 *    +--> (4) Direct library exports (if libraries were opened)
 *    |      |
 *    |      +-- m_eglLib.GetSymbol(name)   (*)
 *    |      +-- m_glLib.GetSymbol(name)    (Linux/Windows/macOS)
 *    |      +-- m_glxLib.GetSymbol(name)   (Linux)
 *    |
 *   \/
 * nullptr
 *
 */
class GLResolver
{
public:
    GLResolver() = default;

    /**
     * Process-lifetime singleton: destructor runs during process teardown.
     * Do not call into GL or unload GL driver libraries here:
     *  - GL context may already be destroyed on this thread.
     *  - Other threads may still be running.
     * OS will reclaim mappings on exit.
     */
    ~GLResolver() = default;

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
     *
     * This method must be called at least once before GetProcAddress() is used.
     *
     * Thread-safety: Initialize() is internally synchronized. Intended usage is to initialize
     * once during startup before any resolution occurs. GetProcAddress() assumes
     * initialization has completed successfully (per API contract) and does not wait
     * for an in-flight Initialize() call.
     *
     * May be called multiple times; initialization work is performed only when needed.
     *
     * @param resolver Optional user resolver callback.
     * @param userData Optional user pointer passed to resolver.
     * @return true if GLAD successfully loaded a backend, false otherwise.
     */
    auto Initialize(UserResolver resolver = nullptr, void* userData = nullptr) -> bool;

    /**
     * @brief Returns true if the resolver was successfully initialized.
     */
    auto IsLoaded() const -> bool;

    /**
     * @brief Returns the backend detected during the last successful Initialize() call.
     *
     */
    auto CurrentBackend() const -> Backend;

    /**
     * @brief Resolves a function pointer by consulting all sources in priority order.
     *
     * @note Intended usage assumes Initialize() has completed successfully before this is called.
     *
     * @note GLX policy: some implementations (notably Mesa/GLVND setups) may return a non-null
     *       pointer for unknown names from glXGetProcAddress*. To reduce the risk of calling
     *       invalid stubs, this resolver only consults glXGetProcAddress* for GLX entry points
     *       (glX*) and extension-style symbols. Desktop core GL symbols are resolved via direct
     *       exports (dlsym / libOpenGL / libGL) through the later lookup path.
     *
     * @param name Function name.
     * @return Procedure address or nullptr.
     */
    auto GetProcAddress(const char* name) const -> void*;

    /**
     * @brief Resolves a function pointer by consulting all sources in priority order from a static context.
     *
     * @param name Function name.
     * @return Procedure address or nullptr.
     */
    static auto GladResolverThunk(const char* name) -> void*;

private:

    // Basic EGL access signatures.
    using EglProc = void (PLATFORM_EGLAPIENTRY*)();
    using EglGetProcAddressFn = EglProc (PLATFORM_EGLAPIENTRY*)(const char* name);
    using EglGetCurrentContextFn = void* (PLATFORM_EGLAPIENTRY*)();

#ifdef _WIN32
    // Basic WGL access signatures.
    using WglGetProcAddressFn = PROC (WINAPI*)(LPCSTR);
    using WglGetCurrentContextFn = HGLRC (WINAPI*)();
#elif defined(__APPLE__)
        using CglGetCurrentContextFn = void* (*)();
#elif !defined(__ANDROID__)
    // Basic GLX access signatures.
    /**
     * glXGetProcAddress/glXGetProcAddressARB return a function pointer. \
    */
    using GlxGetProcAddressFn = void (*(*)(const unsigned char*))();
    using GlxGetCurrentContextFn = void* (*)();

#endif

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

        bool webglAvailable{false};
        bool webglCurrent{false};
    };

    auto OpenNativeLibraries() -> void;
    auto ResolveProviderFunctions() -> void;
    auto ProbeCurrentContext() const -> CurrentContextProbe;
    auto HasCurrentContext(const CurrentContextProbe& probe, std::string& outReason) -> bool;
    auto DetectBackend(const CurrentContextProbe& probe) -> void;
    auto CheckGLRequirementsUnlocked() -> GLContextCheckResult;
    auto LoadGladUnlocked() -> bool;
    auto ResolveUnlocked(const char* name,
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
                         ) const -> void*;

    mutable std::mutex m_mutex;                                     //!< Mutex to synchronize initialization and access.
    bool m_loaded{false};                                           //!< True if the resolver is initialized.
    bool m_initializing{false};                                     //!< True while an Initialize() attempt is in-flight.
    mutable std::condition_variable m_initCv;                       //!< Signals completion of Initialize()/Shutdown().
    Backend m_backend{ Backend::None };                             //!< Detected GL backend.

    UserResolver m_userResolver{nullptr};                           //!< User provided function resolver. Optional, may be null.
    void* m_userData{nullptr};                                      //!< User data to pass to user provided function resolver.

    DynamicLibrary m_eglLib;                                        //!< EGL library handle. Optional, may be empty.
    DynamicLibrary m_glLib;                                         //!< GL library handle. Optional, may be empty.
    DynamicLibrary m_glxLib;                                        //!< GLX library handle. Optional, may be empty.

    EglGetProcAddressFn m_eglGetProcAddress{nullptr};               //!< eglGetProcAddress handle.
    bool m_eglGetAllProcAddresses{false};                           //!< True if EGL_KHR_get_all_proc_addresses (or client variant) is advertised.
    EglGetCurrentContextFn m_eglGetCurrentContext{nullptr};         //!< eglGetCurrentContext handle.
#ifdef _WIN32
    WglGetProcAddressFn m_wglGetProcAddress{nullptr};               //!< wglGetProcAddress handle.
    WglGetCurrentContextFn m_wglGetCurrentContext{nullptr};         //!< wglGetCurrentContext handle.
#elif defined(__APPLE__)
    CglGetCurrentContextFn m_cglGetCurrentContext{nullptr};         //!< CGLGetCurrentContext handle.
#elif !defined(__ANDROID__)
    GlxGetProcAddressFn m_glxGetProcAddress{nullptr};               //!< glXGetProcAddress* handle.
    GlxGetCurrentContextFn m_glxGetCurrentContext{nullptr};         //!< glXGetCurrentContext handle.
#else
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
        case Backend::EGL:
        {
            return "EGL";
        }
        case Backend::GLX:
        {
            return "GLX";
        }
        case Backend::WGL:
        {
            return "WGL";
        }
        case Backend::WebGL:
        {
            return "WebGL";
        }
        case Backend::CGL:
        {
            return "CGL";
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
