#pragma once

#include "PlatformLoader.h"

#include <cstdint>
#include <mutex>

namespace libprojectM
{
namespace Renderer
{

/**
 * @brief Backend describing which API/provider the current context appears to be using.
 */
enum class Backend : std::uint8_t
{
    None = 0,
    EglGles,
    GlxGl,
    WglGl
};

/**
 * @brief Optional user resolver callback.
 *
 * If provided, it is consulted first when resolving procedure addresses.
 * Return nullptr to allow the loader to continue probing.
 */
using UserResolver = void* (*)(const char* name, void* userData);

/**
 * @brief Cross-platform runtime GL/GLES loader.
 *
 * This loader:
 *  - Must be initialized after a GL/GLES context has been created and made current.
 *  - Probes for EGL/GLX/WGL by checking for a current context.
 *  - Uses GLAD2 non-MX entrypoints (gladLoadGL / gladLoadGLES2) via a universal resolver.
 *  - Resolves symbols using the following order:
 *      1) User resolver callback (if any)
 *      2) Global symbol table (RTLD_DEFAULT / main module)
 *      3) eglGetProcAddress / glXGetProcAddress* / wglGetProcAddress (when available)
 *      4) Symbols from opened libEGL / libGL / opengl32
 */
class CrossGlLoader
{
public:
    CrossGlLoader() = default;
    ~CrossGlLoader() = default;

    CrossGlLoader(const CrossGlLoader&) = delete;
    auto operator=(const CrossGlLoader&) -> CrossGlLoader& = delete;
    CrossGlLoader(CrossGlLoader&&) = delete;
    auto operator=(CrossGlLoader&&) -> CrossGlLoader& = delete;

    /**
     * @brief Returns the process-wide loader instance.
     */
    static auto Instance() -> CrossGlLoader&;

    /**
     * @brief Initializes the loader.
     *
     * @param resolver Optional user resolver callback.
     * @param userData Optional user pointer passed to resolver.
     * @return true if GLAD successfully loaded a backend, false otherwise.
     */
    auto Initialize(UserResolver resolver = nullptr, void* userData = nullptr) -> bool;

    /**
     * @brief Shuts down the loader and releases library handles.
     */
    void Shutdown();

    /**
     * @brief Returns true if the loader was successfully initialized.
     */
    auto IsLoaded() const -> bool;

    /**
     * @brief Returns the currently detected backend.
     */
    auto CurrentBackend() const -> Backend;

    /**
     * @brief Resolves a function pointer using the loader's universal resolver.
     *
     * @param name Function name.
     * @return Procedure address or nullptr.
     */
    auto GetProcAddress(const char* name) const -> void*;

    /**
     * @brief Resolves a function pointer using the loader's universal resolver from a static context.
     *
     * @param name Function name.
     * @return Procedure address or nullptr.
     */
    static auto GladResolverThunk(const char* name) -> void*;
private:
    using GetProcFunc = void* (*)(const char* name);

    void OpenNativeLibraries();
    void ResolveProviderFunctions();
    void DetectBackend();
    auto LoadViaGlad() -> bool;
    auto Resolve(const char* name) const -> void*;

    mutable std::mutex m_mutex;

    bool m_loaded{};
    Backend m_backend{ Backend::None };

    UserResolver m_userResolver{};
    void* m_userData{};

    Platform::DynamicLibrary m_eglLib;
    Platform::DynamicLibrary m_glLib;

    GetProcFunc m_eglGetProcAddress{};
    GetProcFunc m_glxGetProcAddress{};
    GetProcFunc m_wglGetProcAddress{};
};

} // namespace Renderer
} // namespace libprojectM
