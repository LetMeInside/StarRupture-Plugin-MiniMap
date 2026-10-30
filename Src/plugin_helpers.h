#pragma once
#include "Adapters/AlienX/Compatibility.h"
#include "Services/Log.h"

inline IPluginSelf* GetSelf() { return MiniMap::Adapters::AlienX::GetSelf(); }
inline IPluginHooks* GetHooks() { auto* s = GetSelf(); return s ? s->hooks : nullptr; }
inline IPluginConfig* GetConfig() { auto* s = GetSelf(); return s ? s->config : nullptr; }

#define MINIMAP_LOG(level, ...) do { if (auto* miniMapLog = MiniMap::Services::TryLog()) \
    MiniMap::Services::Logf(*miniMapLog, MiniMap::Services::Level::level, __VA_ARGS__); } while (false)
#define LOG_TRACE(...) MINIMAP_LOG(Trace, __VA_ARGS__)
#define LOG_DEBUG(...) MINIMAP_LOG(Debug, __VA_ARGS__)
#define LOG_INFO(...)  MINIMAP_LOG(Info, __VA_ARGS__)
#define LOG_WARN(...)  MINIMAP_LOG(Warn, __VA_ARGS__)
#define LOG_ERROR(...) MINIMAP_LOG(Error, __VA_ARGS__)
