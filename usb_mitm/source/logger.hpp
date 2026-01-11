#pragma once

#include <stratosphere.hpp>
#include <cstdio>
#include <utility>
#include <mutex>
#include <source_location>

#ifndef RELEASE
#define DEBUG(...) ::usb::util::Log(__VA_ARGS__)
#else
#define DEBUG(...) AMS_UNUSED(__VA_ARGS__)
#endif

namespace usb::util
{

    namespace impl
    {
        extern ams::os::Mutex s_LogMutex;
        extern char s_FormatBuffer[500];

        void Log(const char *text, size_t length);
    }

    void Initialize();
    void Finalize();

    template <typename... Args>
    void LogUnsafe(const char *fmt, Args &&...args)
    {
        /* Format string, passing the templated arguments. */
        auto length = std::snprintf(impl::s_FormatBuffer, sizeof(impl::s_FormatBuffer), fmt, std::forward<Args>(args)...);
        /* Call underlying implementation. */
        impl::Log(impl::s_FormatBuffer, length);
    }

    inline void VLogUnsafe(const char *fmt, va_list args)
    {
        /* Format string, passing va list. */
        auto length = std::vsnprintf(impl::s_FormatBuffer, sizeof(impl::s_FormatBuffer), fmt, args);
        /* Call underlying implementation. */
        impl::Log(impl::s_FormatBuffer, length);
    }

    template <typename... Args>
    void Log(const char *fmt, Args &&...args)
    {
        /* Acquire lock to ensure thread safety. */
        std::scoped_lock lk(impl::s_LogMutex);
        LogUnsafe(fmt, std::forward<Args>(args)...);
    }

    inline void VLog(const char *fmt, va_list args)
    {
        /* Acquire lock to ensure thread safety. */
        std::scoped_lock lk(impl::s_LogMutex);
        VLogUnsafe(fmt, args);
    }

    ALWAYS_INLINE void Checkpoint(const std::source_location &loc = std::source_location::current())
    {
        Log("%s:%d\t | %s\n", loc.file_name(), loc.line(), loc.function_name());
    }

    // template<typename... Args>
    // ALWAYS_INLINE void Checkpoint(const char* fmt, Args &&... args, const std::source_location& loc = std::source_location::current()) {
    //     std::scoped_lock lk(impl::s_LogMutex);
    //     LogUnsafe("%s:%d\t | %s \t| ", loc.file_name(), loc.line(), loc.function_name());
    //     LogUnsafe(fmt, std::forward<Args>(args)...);
    //     LogUnsafe("\n");
    // }
}
