#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace MiniMapSpriteQueues
{
    struct Feedback
    {
        std::string BlueprintPath;
        bool Success = false;
    };

    class SpriteFifo
    {
    public:
        void Add(std::string blueprintPath)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Items.push_back(std::move(blueprintPath));
        }

        bool HasPending()
        {
            std::lock_guard<std::mutex> lock(Mutex);
            return !Items.empty();
        }

        bool Remove(std::string& blueprintPath)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (Items.empty()) return false;
            blueprintPath = std::move(Items.front());
            Items.pop_front();
            return true;
        }

    private:
        std::mutex Mutex;
        std::deque<std::string> Items;
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
