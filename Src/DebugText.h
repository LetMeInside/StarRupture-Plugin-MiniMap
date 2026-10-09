#pragma once

#ifndef MINIMAP_DEBUG_UI
#define MINIMAP_DEBUG_UI 0
#endif

#if MINIMAP_DEBUG_UI && defined(MODLOADER_CLIENT_BUILD)
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// One optional text slot, with content and formatting owned by its producer.
namespace MiniMapDebugText
{
    inline std::mutex g_mutex;
    inline std::shared_ptr<const std::string> g_text;

    inline void Publish(std::string text)
    {
        auto snapshot = std::make_shared<const std::string>(std::move(text));
        std::scoped_lock lock(g_mutex);
        g_text = std::move(snapshot);
    }

    inline std::shared_ptr<const std::string> GetDebugText()
    {
        std::scoped_lock lock(g_mutex);
        return g_text;
    }

    inline void Clear()
    {
        std::scoped_lock lock(g_mutex);
        g_text.reset();
    }
}
#endif
