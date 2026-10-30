#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace MiniMap::Services
{
    enum class Level { Trace, Debug, Info, Warn, Error };

    class ILog
    {
    public:
        virtual ~ILog() = default;
        virtual void Write(Level level, std::string_view utf8) noexcept = 0;
    };

    // The selected bootstrap supplies its logger during native startup, and
    // the bound Runtime's logger during core initialization through shutdown.
    // Null before bootstrap entry and after teardown; never binds a Runtime.
    ILog* TryLog() noexcept;

    inline void LogMessage(Level level, std::string_view message) noexcept
    {
        if (auto* log = TryLog()) log->Write(level, message);
    }

    // Keep existing printf conventions without imposing a fixed message limit.
    // A failed diagnostic must not change gameplay or ownership decisions.
    template<class... Args>
    void Logf(ILog& log, Level level, const char* format, Args... args) noexcept
    {
        if (!format) return;
        try
        {
            const int length = std::snprintf(nullptr, 0, format, args...);
            if (length < 0) return;
            std::string message(std::size_t(length) + 1, '\0');
            const int written = std::snprintf(message.data(), message.size(), format, args...);
            if (written != length) return; // Never publish a truncated prefix.
            log.Write(level, std::string_view(message.data(), std::size_t(length)));
        }
        catch (...) {} // Allocation/formatting cannot escape a loader callback.
    }
}
