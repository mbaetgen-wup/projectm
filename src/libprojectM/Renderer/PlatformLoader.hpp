#pragma once

#include <cstddef>
#include <cstring>
#include <string>

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#endif

namespace libprojectM
{
namespace Renderer
{
namespace Platform
{

/**
 * @brief Platform library handle type.
 */
#ifdef _WIN32
using LibHandle = HMODULE;
#else
using LibHandle = void*;
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

    inline bool Open(const char* const*) { return false; }
    inline void Close() {}
    inline bool IsOpen() const { return false; }

    inline void* GetSymbol(const char*) const { return nullptr; }
    inline void* Handle() const { return nullptr; }
    static inline void* FindGlobalSymbol(const char*) { return nullptr; }
};

inline auto IsCurrentEgl(const DynamicLibrary&) -> bool { return false; }
#ifndef _WIN32
inline auto IsCurrentGlx(const DynamicLibrary&) -> bool { return false; }
#endif

#else // #ifdef __EMSCRIPTEN__
// -------------------------------------------------------------------------
// Native implementation (Windows / POSIX)
// -------------------------------------------------------------------------

/**
 * @brief RAII wrapper around a dynamic library handle.
 */
class DynamicLibrary
{
public:
    DynamicLibrary() = default;

    /**
     * @brief Constructs a DynamicLibrary by attempting to open the first available library name.
     * @param names Null-terminated list of candidate library names.
     */
    explicit DynamicLibrary(const char* const* names)
    {
        Open(names);
    }

    ~DynamicLibrary()
    {
        Close();
    }

    DynamicLibrary(const DynamicLibrary&) = delete;
    auto operator=(const DynamicLibrary&) -> DynamicLibrary& = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    auto operator=(DynamicLibrary&& other) noexcept -> DynamicLibrary&
    {
        if (this != &other)
        {
            Close();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    /**
     * @brief Attempts to open the first library from the given candidate list.
     * @param names Null-terminated list of candidate library names.
     *
     * @return true if a library was opened, false otherwise.
     */
    auto Open(const char* const* names) -> bool
    {
        Close();

        if (names == nullptr)
        {
            m_lastError = "No library names provided";
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
            ::SetLastError(0);
            m_handle = ::LoadLibraryA(name);
#else
            ::dlerror(); // clear any prior error
            m_handle = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#endif

            if (m_handle != nullptr)
            {
                m_loadedName = name;
                m_lastError.clear();
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

                m_lastError = "LoadLibraryA failed for ";
                m_lastError += name;
                m_lastError += " (";
                m_lastError += std::to_string(static_cast<unsigned long>(err));
                m_lastError += "): ";
                if (len != 0 && msg != nullptr)
                {
                    m_lastError += msg;
                }
                if (msg != nullptr)
                {
                    ::LocalFree(msg);
                }
            }
#else
            {
                const char* err = ::dlerror();
                m_lastError = "dlopen failed for ";
                m_lastError += name;
                m_lastError += ": ";
                if (err != nullptr)
                {
                    m_lastError += err;

                }
            }
#endif
        }

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
        m_lastError.clear();
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
     * @brief Returns the last error message from Open() (empty if none).
     */
    [[nodiscard]] auto LastError() const -> const std::string&
    {
        return m_lastError;
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
        return reinterpret_cast<void*>(::GetProcAddress(m_handle, name));
#else
        return ::dlsym(m_handle, name);
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
            if (void* ptr = reinterpret_cast<void*>(::GetProcAddress(mainModule, name)))
            {
                return ptr;
            }
        }

        // Then search the default OpenGL module.
        if (HMODULE glModule = ::GetModuleHandleA("opengl32.dll"))
        {
            if (void* ptr = reinterpret_cast<void*>(::GetProcAddress(glModule, name)))
            {
                return ptr;
            }
        }

        return nullptr;
#else
        return ::dlsym(RTLD_DEFAULT, name);
#endif
    }

private:
    LibHandle m_handle{};          //!< Library handle used to access the system library.
    std::string m_loadedName;      //< Successfully opened library name.
    std::string m_lastError;       //< Last Open() error message.
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

/**
 * @brief Converts a symbol pointer (void*) into a function pointer type without UB.
 *
 * dlsym/GetProcAddress return data pointers (void* / FARPROC). Converting those to function
 * pointers via reinterpret_cast is technically undefined behavior in C++.
 *
 * This helper uses memcpy to transfer the representation into a function pointer type.
 * If the platform uses different sizes for data pointers and function pointers, the
 * conversion fails and returns nullptr.
 *
 * @tparam Fn Function pointer type.
 * @param symbol Symbol pointer as void*.
 * @return Function pointer or nullptr.
 */
template <typename Fn>
auto SymbolToFunction(void* symbol) -> Fn
{
    if (symbol == nullptr)
    {
        return nullptr;
    }

    if (sizeof(Fn) != sizeof(void*))
    {
        return nullptr;
    }

    Fn func = nullptr;
    std::memcpy(&func, &symbol, sizeof(Fn));
    return func;
}

inline auto IsCurrentEgl(const DynamicLibrary& eglLib) -> bool
{
    if (!eglLib.IsOpen())
    {
        return false;
    }

    using EglGetCurrentContext = void* (*)();
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

    void* sym = glLib.GetSymbol("glXGetCurrentContextARB");
    auto func = SymbolToFunction<GlxGetCurrentContext>(sym);
    if (func == nullptr)
    {
        sym = glLib.GetSymbol("glXGetCurrentContext");
        func = SymbolToFunction<GlxGetCurrentContext>(sym);
    }

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

    using WglGetCurrentContext = void* (WINAPI*)(void);
    void* sym = reinterpret_cast<void*>(::GetProcAddress(glModule, "wglGetCurrentContext"));
    auto func = SymbolToFunction<WglGetCurrentContext>(sym);
    return func != nullptr && func() != nullptr;
}

#endif

#endif // #ifdef __EMSCRIPTEN__

} // namespace Platform
} // namespace Renderer
} // namespace libprojectM
