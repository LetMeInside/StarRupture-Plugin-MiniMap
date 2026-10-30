#include "Log.h"
#include "Compatibility.h"

#include <string>

namespace MiniMap::Adapters::AlienX
{
    void Log::Write(Services::Level level, std::string_view utf8) noexcept
    {
        auto* self = GetSelf();
        if (!self || !self->logger || !self->logger->Log) return;
        PluginLogLevel nativeLevel;
        switch (level)
        {
        case Services::Level::Trace: nativeLevel = PluginLogLevel::Trace; break;
        case Services::Level::Debug: nativeLevel = PluginLogLevel::Debug; break;
        case Services::Level::Info: nativeLevel = PluginLogLevel::Info; break;
        case Services::Level::Warn: nativeLevel = PluginLogLevel::Warn; break;
        case Services::Level::Error: nativeLevel = PluginLogLevel::Error; break;
        default: return;
        }
        try
        {
            // The SDK borrows a C string; it must not read beyond a string_view.
            // Raw Log preserves long messages and the existing plugin identity,
            // severity filtering and downstream sink behavior.
            const std::string message(utf8);
            self->logger->Log(nativeLevel, self, message.c_str());
        }
        catch (...) {} // Logging must never escape into a loader callback.
    }
}
