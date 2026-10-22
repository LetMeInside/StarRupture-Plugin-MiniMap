#pragma once
#include "plugin.h"
inline IPluginSelf* GetSelf() { return g_self; }
inline IPluginHooks* GetHooks() { auto* s = GetSelf(); return s ? s->hooks : nullptr; }
inline IPluginConfig* GetConfig() { auto* s = GetSelf(); return s ? s->config : nullptr; }
#define LOG_TRACE(format, ...) if (auto s = GetSelf()) s->logger->Trace(s, format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto s = GetSelf()) s->logger->Debug(s, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  if (auto s = GetSelf()) s->logger->Info (s, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  if (auto s = GetSelf()) s->logger->Warn (s, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto s = GetSelf()) s->logger->Error(s, format, ##__VA_ARGS__)
