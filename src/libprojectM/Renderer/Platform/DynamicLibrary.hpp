#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#ifndef __EMSCRIPTEN__

#ifdef _WIN32

#include <windows.h>

#else // #ifdef _WIN32

#include <dlfcn.h>

#endif // #ifdef _WIN32

#endif // #ifndef __EMSCRIPTEN__

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
// When enabled, the loader prints diagnostics for unusual ABI
// situations (e.g., platforms where data pointers and function pointers have
// different representations/sizes). Disabled by default to avoid noisy output.
#ifndef GLRESOLVER_LOADER_DIAGNOSTICS
#define GLRESOLVER_LOADER_DIAGNOSTICS 0
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

#if GLRESOLVER_LOADER_DIAGNOSTICS

inline auto ReportFnPtrSizeMismatch(const char* where, std::size_t fnSize, std::size_t ptrSize) -> void
{
    std::fprintf(stderr, "[PlatformLoader] %s: sizeof(Fn)=%zu sizeof(void*)=%zu; cannot convert symbol/function pointer\n",
                 (where != nullptr ? where : "(unknown)"),
                 static_cast<std::size_t>(fnSize),
                 static_cast<std::size_t>(ptrSize));
}

#endif

/**
 * @brief Converts a symbol pointer (void*) into a function pointer type .
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
    static_assert(std::is_pointer<Fn>::value, "Fn must be a pointer type");
    static_assert(std::is_function<typename std::remove_pointer<Fn>::type>::value,
                  "Fn must be a function pointer type");

    if (symbol == nullptr)
    {
        return nullptr;
    }

    if (sizeof(Fn) != sizeof(void*))
    {

#if GLRESOLVER_LOADER_DIAGNOSTICS

        ReportFnPtrSizeMismatch("SymbolToFunction", sizeof(Fn), sizeof(void*));

#endif

        return nullptr;
    }

    Fn func = nullptr;
    std::memcpy(&func, &symbol, sizeof(Fn));
    return func;
}

/**
 * @brief Converts a function pointer into a symbol pointer representation .
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
    static_assert(std::is_pointer<Fn>::value, "Fn must be a pointer type");
    static_assert(std::is_function<typename std::remove_pointer<Fn>::type>::value,
                  "Fn must be a function pointer type");

    if (func == nullptr)
    {
        return nullptr;
    }

    if (sizeof(Fn) != sizeof(void*))
    {

#if GLRESOLVER_LOADER_DIAGNOSTICS

        ReportFnPtrSizeMismatch("FunctionToSymbol", sizeof(Fn), sizeof(void*));

#endif

        return nullptr;
    }

    void* symbol = nullptr;
    std::memcpy(&symbol, &func, sizeof(void*));
    return symbol;
}

/**
 * @brief Converts a function pointer into an integer representation .
 *
 * Useful for validating platform-specific sentinel values (e.g. Windows WGL).
 * Returns 0 if the conversion is not representable.
 */
template<typename Fn>
auto FunctionToInteger(Fn func) -> std::uintptr_t
{
    static_assert(std::is_pointer<Fn>::value, "Fn must be a pointer type");
    static_assert(std::is_function<typename std::remove_pointer<Fn>::type>::value,
                  "Fn must be a function pointer type");

    if (func == nullptr)
    {
        return 0;
    }

    if (sizeof(Fn) != sizeof(void*))
    {

#if GLRESOLVER_LOADER_DIAGNOSTICS

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

/**
 * @brief Parses a bool-ish env var. Truthy: 1, y, yes, t, true, on (case-insensitive). Falsy: 0, n, no, f, false, off.
 */
auto EnvFlagEnabled(const char* name, bool defaultValue) -> bool;

#ifdef _WIN32

/**
 * @brief Converts a Windows FARPROC into the generic symbol representation (void*) .
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

    DynamicLibrary(DynamicLibrary&&) noexcept = default;
    DynamicLibrary& operator=(DynamicLibrary&&) noexcept = default;

    inline auto Open(const char* const*, std::string&) -> bool
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

    inline auto SetCloseOnDestruct(bool) -> void
    {
    }

    static inline auto FindGlobalSymbol(const char*) -> void*
    {
        return nullptr;
    }
};

#else // #ifdef __EMSCRIPTEN__

// -------------------------------------------------------------------------
// Native implementation (Windows / POSIX)
// -------------------------------------------------------------------------

/**
 * @brief Wrapper around a dynamic library handle. Once opened, the library needs to be closed by calling Close() if needed, it is not closed automatically by destruction.
 */
class DynamicLibrary
{
public:
    DynamicLibrary() = default;

    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;

    auto operator=(const DynamicLibrary&) -> DynamicLibrary& = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept;

    auto operator=(DynamicLibrary&& other) noexcept -> DynamicLibrary&;

    /**
     * @brief Attempts to open the first library from the given candidate list.
     * @param names Null-terminated list of candidate library names.
     * @param reason User error message.
     *
     * @return true if a library was opened, false otherwise.
     */
    auto Open(const char* const* names, std::string& reason) -> bool;

    /**
     * @brief Closes the library if it is open.
     */
    void Close();

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
     * @brief Controls whether the library is closed in the destructor.
     *
     * The default is false to avoid unloading GL/driver libraries during process teardown.
     * Enable only for short-lived helper loads where unloading is safe and desired.
     */
    auto SetCloseOnDestruct(bool enabled) -> void
    {
        m_closeOnDestruct = enabled;
    }

    /**
     * @brief Resolves a symbol from this library.
     * @param name The symbol name.
     *
     * @return Symbol address or nullptr.
     */
    auto GetSymbol(const char* name) const -> void*;

    /**
     * @brief Looks up a symbol in the global process scope.
     * @param name Symbol name.
     *
     * @return Symbol address or nullptr.
     */
    static auto FindGlobalSymbol(const char* name) -> void*;

private:
    LibHandle m_handle{};           //!< Library handle used to access the system library.
    std::string m_loadedName;       //< Successfully opened library name.
    bool m_closeOnDestruct{false};  //!< If true, Close() is called from the destructor.
};

#endif // #ifdef __EMSCRIPTEN__

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
