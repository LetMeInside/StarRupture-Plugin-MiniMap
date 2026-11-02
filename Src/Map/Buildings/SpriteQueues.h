#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace MiniMapBuildingCache { struct Sprite; }

namespace MiniMapSpriteQueues
{
    // The renderer FIFO is keyed only by canonical blueprint path.
    using Request = std::string;

    struct Feedback
    {
        std::string BlueprintPath;
        bool Success = false;
        uint64_t Session = 0;
        // Optional CPU-owned processed pixels. The canonical .sprite write has
        // already succeeded before the renderer reports successful feedback.
        std::shared_ptr<const MiniMapBuildingCache::Sprite> Payload;
    };

    class SpriteFifo
    {
    public:
        void Add(std::string path)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Items.push_back(std::move(path));
        }

        bool HasPending()
        {
            std::lock_guard<std::mutex> lock(Mutex);
            return !Items.empty();
        }

        bool Remove(std::string& path)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (Items.empty()) return false;
            path = std::move(Items.front());
            Items.pop_front();
            return true;
        }

    private:
        std::mutex Mutex;
        std::deque<Request> Items;
    };

    class SpriteFeedbackQueue
    {
    public:
        void Add(Feedback feedback)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Items.push_back(std::move(feedback));
        }

        bool Remove(Feedback& feedback)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (Items.empty()) return false;
            feedback = std::move(Items.front());
            Items.pop_front();
            return true;
        }

    private:
        std::mutex Mutex;
        std::deque<Feedback> Items;
    };
}
