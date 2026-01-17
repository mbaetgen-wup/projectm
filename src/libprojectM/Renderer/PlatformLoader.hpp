#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <windows.h>

// -------------------------------------------------------------------------
// Windows DLL safe-search flags
// -------------------------------------------------------------------------
// Some toolchains/Windows SDKs may not define these constants even though
// the OS loader supports them (Windows 7 w/ KB2533623+, Win8+). We define
// them locally when absent to allow using LoadLibraryEx with safe search.
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_APPLICATION_DIR
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200
#endif
#ifndef LOAD_LIBRARY_SEARCH_USER_DIRS
#define LOAD_LIBRARY_SEARCH_USER_DIRS 0x00000400
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

// -------------------------------------------------------------------------
// Optional legacy DLL search fallback
// -------------------------------------------------------------------------
//
// If the OS loader does not support LOAD_LIBRARY_SEARCH_* flags (ERROR_INVALID_PARAMETER),
// this loader tries to load from explicit safe locations (application directory and
// System32 for known system DLLs).
//
// As a last resort, some applications may still want to fall back to LoadLibrary(name)
// (which can consult legacy search paths such as the process current working directory).
// This is disabled by default for security hardening.
//
// Define PLATFORM_ALLOW_UNSAFE_DLL_SEARCH=1 to re-enable the legacy fallback.
#ifndef PLATFORM_ALLOW_UNSAFE_DLL_SEARCH
#define PLATFORM_ALLOW_UNSAFE_DLL_SEARCH 0
#endif
#else
#include <dlfcn.h>
#endif
#endif

// -------------------------------------------------------------------------
// Minimal EGL calling-convention support
// -------------------------------------------------------------------------
//
// We avoid including EGL headers in this loader. On 32-bit Windows, EGL entry
// points use the stdcall calling convention (via EGLAPIENTRY/KHRONOS_APIENTRY).
// On 64-bit Windows the calling convention is ignored.
//
// This macro is used in our local EGL function pointer typedefs to ensure we
// call into the provider (ANGLE / driver EGL) using the correct ABI.
#if defined(_WIN32) && !defined(_WIN64)
#define PLATFORM_EGLAPIENTRY __stdcall
#else
#define PLATFORM_EGLAPIENTRY
#endif

// -------------------------------------------------------------------------
// Optional loader diagnostics
// -------------------------------------------------------------------------
//
// When enabled, the loader prints best-effort diagnostics for unusual ABI
// situations (e.g., platforms where data pointers and function pointers have
// different representations/sizes). Disabled by default to avoid noisy output
// in production applications.
#ifndef PLATFORM_LOADER_DIAGNOSTICS
#define PLATFORM_LOADER_DIAGNOSTICS 0
#endif

namespace libprojectM {
namespace Renderer {
namespace Platform {

/**
 * @brief Platform library handle type.
 */
#ifdef _WIN32
using LibHandle = HMODULE;
#else
using LibHandle = void*;
#endif

inline auto TrimTrailingWhitespace(std::string& s) -> void
{
    while (!s.empty())
    {
        const char c = s.back();
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
        {
            s.pop_back();
            continue;
        }
        break;
    }
}

// -------------------------------------------------------------------------
// Common (Windows / POSIX / Emscripten)
// -------------------------------------------------------------------------

#if PLATFORM_LOADER_DIAGNOSTICS
inline auto ReportFnPtrSizeMismatch(const char* where, std::size_t fnSize, std::size_t ptrSize) -> void
{
    std::fprintf(stderr, "[PlatformLoader] %s: sizeof(Fn)=%zu sizeof(void*)=%zu; cannot convert symbol/function pointer\n",
                 (where != nullptr ? where : "(unknown)"),
                 static_cast<std::size_t>(fnSize),
                 static_cast<std::size_t>(ptrSize));
}
#endif

/**
 * @brief Converts a symbol pointer (void*) into a function pointer type (best-effort).
 *
 * dlsym/GetProcAddress return untyped procedure addresses (void* on POSIX, FARPROC on Windows). Converting those to function
 * pointers via reinterpret_cast is not guaranteed to be portable in ISO C++.
 *
 * This helper uses memcpy to transfer the representation into a function pointer type. This is widely supported on common
 * platforms/ABIs (and is compatible with typical POSIX dlsym usage), but it is still a best-effort technique rather than a strict
 * ISO C++ guarantee.
 * If the platform uses different sizes for data pointers and function pointers, the
 * conversion fails and returns nullptr.
 *
 * @tparam Fn Function pointer type.
 * @param symbol Symbol pointer as void*.
 * @return Function pointer or nullptr.
 */
template<typename Fn>
auto SymbolToFunction(void* symbol) -> Fn
{
    if (symbol == nullptr)
    {
        return nullptr;
    }

    if (sizeof(Fn) != sizeof(void*))
    {
#if PLATFORM_LOADER_DIAGNOSTICS
        ReportFnPtrSizeMismatch("SymbolToFunction", sizeof(Fn), sizeof(void*));
#endif
        return nullptr;
    }

    Fn func = nullptr;
    std::memcpy(&func, &symbol, sizeof(Fn));
    return func;
}

/**
 * @brief Converts a function pointer into a symbol pointer representation (best-effort).
 *
 * The inverse of SymbolToFunction(). This is used at API boundaries where legacy
 * interfaces represent procedure addresses as void*.
 *
 * @tparam Fn Function pointer type.
 * @param func Function pointer.
 * @return Symbol pointer as void* or nullptr if not representable.
 */
template<typename Fn>
auto FunctionToSymbol(Fn func) -> void*
{
    if (func == nullptr)
    {
        return nullptr;
    }

    if (sizeof(Fn) != sizeof(void*))
    {
#if PLATFORM_LOADER_DIAGNOSTICS
        ReportFnPtrSizeMismatch("FunctionToSymbol", sizeof(Fn), sizeof(void*));
#endif
        return nullptr;
    }

    void* symbol = nullptr;
    std::memcpy(&symbol, &func, sizeof(void*));
    return symbol;
}

/**
 * @brief Converts a function pointer into an integer representation (best-effort).
 *
 * Useful for validating platform-specific sentinel values (e.g. Windows WGL).
 * Returns 0 if the conversion is not representable.
 */
template<typename Fn>
auto FunctionToInteger(Fn func) -> std::uintptr_t
{
    if (func == nullptr)
    {
        return 0;
    }

    if (sizeof(Fn) != sizeof(void*))
    {
#if PLATFORM_LOADER_DIAGNOSTICS
        ReportFnPtrSizeMismatch("FunctionToInteger", sizeof(Fn), sizeof(void*));
#endif
        return 0;
    }

    if (sizeof(std::uintptr_t) != sizeof(void*))
    {
        return 0;
    }

    std::uintptr_t value = 0;
    std::memcpy(&value, &func, sizeof(std::uintptr_t));
    return value;
}


#ifdef _WIN32
/**
 * @brief Converts a Windows FARPROC into the generic symbol representation (void*) (best-effort).
 *
 * Windows GetProcAddress returns a function pointer type (FARPROC). To avoid relying on a direct
 * reinterpret_cast between function pointers and object pointers, this helper copies the bit pattern
 * into a void* storage location when the sizes match.
 */
inline auto WinProcToSymbol(FARPROC proc) noexcept -> void*
{
    if (proc == nullptr)
    {
        return nullptr;
    }

    if (sizeof(proc) != sizeof(void*))
    {
        return nullptr;
    }

    void* sym = nullptr;
    std::memcpy(&sym, &proc, sizeof(void*));
    return sym;
}
#endif

#ifdef __EMSCRIPTEN__

// -------------------------------------------------------------------------
// Emscripten stub implementation
// -------------------------------------------------------------------------
class DynamicLibrary
{
public:
    DynamicLibrary() = default;
    ~DynamicLibrary() = default;

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    inline auto Open(const char* const*) -> bool
    {
        return false;
    }

    inline auto Close() -> void
    {
    }

    inline auto IsOpen() const -> bool
    {
        return false;
    }

    inline auto GetSymbol(const char*) const -> void*
    {
        return nullptr;
    }

    inline auto Handle() const -> void*
    {
        return nullptr;
    }

    inline auto SetCloseOnDestruct(bool enabled) -> void
    {
    }

    static inline auto FindGlobalSymbol(const char*) -> void*
    {
        return nullptr;
    }
};

inline auto IsCurrentEgl(const DynamicLibrary&) -> bool
{
    return false;
}

#ifndef _WIN32
inline auto IsCurrentGlx(const DynamicLibrary&) -> bool
{
    return false;
}
#endif

#else // #ifdef __EMSCRIPTEN__
// -------------------------------------------------------------------------
// Native implementation (Windows / POSIX)
// -------------------------------------------------------------------------

/**
 * @brief Wrapper around a dynamic library handle.
 */
class DynamicLibrary
{
public:
    DynamicLibrary() = default;

    ~DynamicLibrary()
    {
        if (m_closeOnDestruct)
        {
            Close();
        }
    }

    DynamicLibrary(const DynamicLibrary&) = delete;
    auto operator=(const DynamicLibrary&) -> DynamicLibrary& = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept
        : m_closeOnDestruct(other.m_closeOnDestruct)
        , m_handle(other.m_handle)
        , m_loadedName(std::move(other.m_loadedName))
    {
        other.m_handle = nullptr;
        other.m_loadedName.clear();
    }

    auto operator=(DynamicLibrary&& other) noexcept -> DynamicLibrary&
    {
        if (this != &other)
        {
            Close();
            m_closeOnDestruct = other.m_closeOnDestruct;
            m_handle = other.m_handle;
            m_loadedName = std::move(other.m_loadedName);
            other.m_handle = nullptr;
            other.m_loadedName.clear();
        }
        return *this;
    }

    /**
     * @brief Attempts to open the first library from the given candidate list.
     * @param names Null-terminated list of candidate library names.
     * @param reason User error message.
     *
     * @return true if a library was opened, false otherwise.
     */
    auto Open(const char* const* names, std::string& reason) -> bool
    {
        Close();

        if (names == nullptr)
        {
            reason = "No library names provided";
            return false;
        }

        for (const char* const* namePtr = names; *namePtr; ++namePtr)
        {
            const char* name = *namePtr;
            if (name == nullptr)
            {
                continue;
            }

#ifdef _WIN32
            // DLL loading policy:
            //  - Prefer LoadLibraryExA with LOAD_LIBRARY_SEARCH_* flags to avoid CWD/PATH hijacking.
            //  - If the OS loader doesn't support the flags (ERROR_INVALID_PARAMETER), fall back to:
            //      * application directory (explicit full path based on the running module)
            //      * System32 (for known system DLLs like opengl32.dll)
            //      * optionally, legacy LoadLibraryA(name) (disabled by default; see PLATFORM_ALLOW_UNSAFE_DLL_SEARCH).
            //
            // NOTE: We intentionally do not call SetDefaultDllDirectories() here. It changes
            // the process-wide DLL search behavior and can surprise a host application.
            // Instead, we rely on per-call LoadLibraryExA LOAD_LIBRARY_SEARCH_* flags when available.

            auto buildAppDirPath = [](const char* dllName) -> std::string {
                char exePath[MAX_PATH] = {};
                const DWORD n = ::GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(sizeof(exePath)));
                if (n == 0u || n >= sizeof(exePath))
                {
                    return std::string();
                }

                // Strip to directory.
                for (DWORD i = n; i > 0u; --i)
                {
                    const char c = exePath[i - 1u];
                    if (c == '\\' || c == '/')
                    {
                        exePath[i] = '\0';
                        break;
                    }
                }

                std::string full(exePath);
                full += dllName;
                return full;
            };

            auto buildSystem32Path = [](const char* dllName) -> std::string {
                char sysDir[MAX_PATH] = {};
                const UINT n = ::GetSystemDirectoryA(sysDir, static_cast<UINT>(sizeof(sysDir)));
                if (n == 0u || n >= sizeof(sysDir))
                {
                    return std::string();
                }
                std::string full(sysDir);
                full += "\\";
                full += dllName;
                return full;
            };

            auto tryLoadEx = [&](const char* dllName, DWORD flags) -> HMODULE {
                ::SetLastError(0);
                return ::LoadLibraryExA(dllName, nullptr, flags);
            };

            auto tryLoad = [&](const char* dllName) -> HMODULE {
                ::SetLastError(0);
                return ::LoadLibraryA(dllName);
            };

            // Detect whether the name contains a path. If so, LoadLibraryA is already safe (absolute/relative path is explicit).
            HMODULE handle = nullptr;
            const bool hasPath = (std::strchr(name, '\\') != nullptr) || (std::strchr(name, '/') != nullptr);

            if (hasPath)
            {
                // Prefer safe search flags for dependency resolution relative to the DLL location.
                handle = tryLoadEx(name, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (handle == nullptr && ::GetLastError() == ERROR_INVALID_PARAMETER)
                {
                    // Older OS loader: explicit path is already unambiguous.
                    handle = tryLoad(name);
                }
            }
            else
            {
                // Bare name: avoid CWD/PATH when possible.
                //
                // NOTE: When loading app-bundled DLLs (e.g. ANGLE's libEGL/libGLESv2), loading by
                // explicit full path improves dependency resolution by allowing the loader to search
                // the DLL's directory for its dependent DLLs (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR).
                // This keeps the search path restricted without changing process-wide state.
                const std::string appFull = buildAppDirPath(name);
                if (!appFull.empty())
                {
                    handle = tryLoadEx(appFull.c_str(), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                    if (handle == nullptr && ::GetLastError() == ERROR_INVALID_PARAMETER)
                    {
                        // Older OS loader: explicit path is already unambiguous.
                        handle = tryLoad(appFull.c_str());
                    }
                }

                if (handle == nullptr)
                {
                    handle = tryLoadEx(name, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                }

                if (handle == nullptr && ::GetLastError() == ERROR_INVALID_PARAMETER)
                {
                    // Flags unsupported: best-effort manual safe search.
                    // 1) Application directory
                    if (handle == nullptr && !appFull.empty())
                    {
                        handle = tryLoad(appFull.c_str());
                    }

                    // 2) System32 for known system DLLs
                    if (handle == nullptr && _stricmp(name, "opengl32.dll") == 0)
                    {
                        const std::string sysFull = buildSystem32Path("opengl32.dll");
                        if (!sysFull.empty())
                        {
                            handle = tryLoad(sysFull.c_str());
                        }
                    }

                    // 3) Legacy fallback (disabled by default).
                    //
                    // NOTE: LoadLibrary(name) without LOAD_LIBRARY_SEARCH_* flags can consult legacy
                    // search paths (including the current working directory) depending on process
                    // configuration. See Microsoft guidance on DLL search order hardening.
                    if (handle == nullptr && PLATFORM_ALLOW_UNSAFE_DLL_SEARCH != 0)
                    {
                        handle = tryLoad(name);
                    }
                }
                else if (handle == nullptr)
                {
                    // Flags supported but the restricted search didn't find it. Fall back to default dirs.
                    handle = tryLoadEx(name, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            }

            m_handle = handle;
#else
            ::dlerror(); // clear any prior error
            m_handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif

            if (m_handle != nullptr)
            {
                m_loadedName = name;
                return true;
            }

#ifdef _WIN32
            {
                const DWORD err = ::GetLastError();

                char* msg = nullptr;
                const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
                const DWORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);

                const DWORD len = ::FormatMessageA(flags,
                                                   nullptr,
                                                   err,
                                                   lang,
                                                   reinterpret_cast<LPSTR>(&msg),
                                                   0,
                                                   nullptr);

                reason = "LoadLibrary failed for ";
                reason += name;
                reason += " (";
                reason += std::to_string(static_cast<unsigned long>(err));
                reason += "): ";
                if (len != 0 && msg != nullptr)
                {
                    reason += msg;
                    TrimTrailingWhitespace(reason);
                }
                if (msg != nullptr)
                {
                    ::LocalFree(msg);
                }
            }
#else
        const char* err = ::dlerror();
        if (err != nullptr)
        {
            reason = "dlopen failed for ";
            reason += name;
            reason += ": ";
            reason += err;
        }
#endif
        } // for loop

        return false;
    }

    /**
     * @brief Closes the library if it is open.
     */
    void Close()
    {
        if (m_handle == nullptr)
        {
            return;
        }

#ifdef _WIN32
        ::FreeLibrary(m_handle);
#else
        ::dlclose(m_handle);
#endif
        m_handle = nullptr;
        m_loadedName.clear();
    }

    /**
     * @brief Controls whether the library is automatically closed in the destructor.
     *
     * Some libraries (notably OpenGL drivers) are safest to keep loaded until process exit,
     * especially when used from a process-lifetime singleton.
     *
     * @param enabled If false, the destructor will not call Close().
     */
    void SetCloseOnDestruct(bool enabled)
    {
        m_closeOnDestruct = enabled;
    }

    /**
     * @brief Returns true if the library handle is valid.
     */
    [[nodiscard]] auto IsOpen() const -> bool
    {
        return m_handle != nullptr;
    }

    /**
     * @brief Returns the name of the successfully opened library (empty if none).
     */
    [[nodiscard]] auto LoadedName() const -> const std::string&
    {
        return m_loadedName;
    }

    /**
     * @brief Returns the raw library handle.
     */
    [[nodiscard]] auto Handle() const -> LibHandle
    {
        return m_handle;
    }

    /**
     * @brief Resolves a symbol from this library.
     * @param name The symbol name.
     *
     * @return Symbol address or nullptr.
     */
    auto GetSymbol(const char* name) const -> void*
    {
        if (m_handle == nullptr || name == nullptr)
        {
            return nullptr;
        }

#ifdef _WIN32
        return WinProcToSymbol(::GetProcAddress(m_handle, name));
#else
        // clear any prior error
        ::dlerror();
        void* sym = ::dlsym(m_handle, name);
        const char* err = ::dlerror();
        if (err != nullptr)
        {
            return nullptr;
        }
        return sym;
#endif
    }

    /**
     * @brief Looks up a symbol in the global process scope.
     * @param name Symbol name.
     *
     * @return Symbol address or nullptr.
     */
    static auto FindGlobalSymbol(const char* name) -> void*
    {
        if (name == nullptr)
        {
            return nullptr;
        }

#ifdef _WIN32
        // Search the main executable first.
        if (HMODULE mainModule = ::GetModuleHandleA(nullptr))
        {
            if (void* ptr = WinProcToSymbol(::GetProcAddress(mainModule, name)))
            {
                return ptr;
            }
        }

        // If the host has already loaded EGL/GLES provider DLLs (e.g., ANGLE), probe those modules as well.
        // This is a best-effort enhancement for applications embedding this library where we may not have
        // opened the provider libraries ourselves.
        {
            static constexpr std::array<const char*, 6> moduleNames =
            {
                "libEGL.dll",
                "EGL.dll",
                "libGLESv2.dll",
                "GLESv2.dll",
                "libGLESv3.dll",
                "GLESv3.dll"
            };

            for (const auto& m : moduleNames)
            {
                if (HMODULE mod = ::GetModuleHandleA(m))
                {
                    if (void* ptr = WinProcToSymbol(::GetProcAddress(mod, name)))
                    {
                        return ptr;
                    }
                }
            }
        }

        // Then search the default OpenGL module.
        if (HMODULE glModule = ::GetModuleHandleA("opengl32.dll"))
        {
            if (void* ptr = WinProcToSymbol(::GetProcAddress(glModule, name)))
            {
                return ptr;
            }
        }

        return nullptr;
#else
        // clear any prior error
        ::dlerror();
        void* sym = ::dlsym(RTLD_DEFAULT, name);
        const char* err = ::dlerror();
        if (err != nullptr)
        {
            return nullptr;
        }
        return sym;
#endif
    }

private:
    bool m_closeOnDestruct{true}; //!< If true, Close() is called from the destructor.
    LibHandle m_handle{};         //!< Library handle used to access the system library.
    std::string m_loadedName;     //< Successfully opened library name.
};

/**
 * @brief Checks whether the current context is EGL-based.
 * @param eglLib The loaded EGL library.
 */
auto IsCurrentEgl(const DynamicLibrary& eglLib) -> bool;

/**
 * @brief Checks whether the current context is GLX-based (Linux/Unix).
 * @param glLib The loaded GL library.
 */
#ifndef _WIN32

auto IsCurrentGlx(const DynamicLibrary& glLib) -> bool;

#else

/**
 * @brief Checks whether the current context is WGL-based (Windows).
 */
auto IsCurrentWgl() -> bool;

#endif

// ---------------- Inline implementations -----------------------------------

inline auto IsCurrentEgl(const DynamicLibrary& eglLib) -> bool
{
    if (!eglLib.IsOpen())
    {
        return false;
    }

    using EglGetCurrentContext = void* (PLATFORM_EGLAPIENTRY*)();
    void* sym = eglLib.GetSymbol("eglGetCurrentContext");
    auto func = SymbolToFunction<EglGetCurrentContext>(sym);
    return func != nullptr && func() != nullptr;
}

#ifndef _WIN32
inline auto IsCurrentGlx(const DynamicLibrary& glLib) -> bool
{
    if (!glLib.IsOpen())
    {
        return false;
    }

    using GlxGetCurrentContext = void* (*)();

    // glXGetCurrentContext is the canonical API entry point.
    void* sym = glLib.GetSymbol("glXGetCurrentContext");
    auto func = SymbolToFunction<GlxGetCurrentContext>(sym);

    return func != nullptr && func() != nullptr;
}

#else
inline auto IsCurrentWgl() -> bool
{
    HMODULE glModule = ::GetModuleHandleA("opengl32.dll");
    if (glModule == nullptr)
    {
        return false;
    }

    using WglGetCurrentContext = void*(WINAPI*) (void);
    void* sym = WinProcToSymbol(::GetProcAddress(glModule, "wglGetCurrentContext"));
    auto func = SymbolToFunction<WglGetCurrentContext>(sym);
    return func != nullptr && func() != nullptr;
}

#endif

#endif // #ifdef __EMSCRIPTEN__

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
