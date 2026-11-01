#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace MiniMapBuildingCache { struct Sprite; }
namespace MiniMapBuildingComplexIndex { struct SourcePayload; }

namespace MiniMapSpriteQueues
{
    enum class RequestKind : uint8_t
    {
        MiniMap,
        BuildingComplex
    };

    enum class ResultStatus : uint8_t
    {
        Success,
        RetryableFailure,
        Unsupported
    };

    // One FIFO carries both consumers. MiniMap keeps its path/session contract;
    // BuildingComplex admission carries an immutable exact-source receipt.
    struct Request
    {
        RequestKind Kind = RequestKind::MiniMap;
        std::string BlueprintPath;
        uint64_t Session = 0;
        uint64_t RequestId = 0; // Caller-owned correlation/cancellation identity.
        int32_t MassIndex = -1;
        int32_t MassSerial = 0;
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0; // Stamped by the producer at admission.
        std::string CanonicalBlueprintIdentity;
        std::shared_ptr<const MiniMapBuildingComplexIndex::SourcePayload> SourceSnapshot;
    };

    struct CaptureResult
    {
        RequestKind Kind = RequestKind::MiniMap;
        uint64_t RequestId = 0;
        int32_t MassIndex = -1;
        int32_t MassSerial = 0;
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0;
        std::string CanonicalBlueprintIdentity;
        std::shared_ptr<const MiniMapBuildingComplexIndex::SourcePayload> SourceSnapshot;
        ResultStatus Status = ResultStatus::RetryableFailure;
        std::string BlueprintPath;
        uint64_t Session = 0;
        // Optional immutable processed CPU pixels and their actual capture bounds
        // and crop mapping, never a native/readback pointer. BuildingCache::Sprite
        // owns all of that metadata together with its RGBA bytes.
        std::shared_ptr<const MiniMapBuildingCache::Sprite> Payload;
    };
    using Feedback = CaptureResult; // Existing queue spelling remains source-compatible.

    class SpriteFifo
    {
    public:
        void Add(Request request)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Items.push_back(std::move(request));
        }

        bool HasPending()
        {
            std::lock_guard<std::mutex> lock(Mutex);
            return !Items.empty();
        }

        bool Remove(Request& request)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (Items.empty()) return false;
            request = std::move(Items.front());
            Items.pop_front();
            return true;
        }

        // Extract the earliest request for a tag while retaining the order of
        // all remaining entries. The native renderer uses unfiltered Remove()
        // so accepted capture work always follows the shared FIFO head.
        bool Remove(Request& request, RequestKind expectedKind)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            const auto item = std::find_if(Items.begin(), Items.end(),
                [expectedKind](const Request& candidate) { return candidate.Kind == expectedKind; });
            if (item == Items.end()) return false;
            request = std::move(*item);
            Items.erase(item);
            return true;
        }

        bool Remove(RequestKind expectedKind, uint64_t requestId)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            const auto item = std::find_if(Items.begin(), Items.end(),
                [expectedKind, requestId](const Request& candidate) {
                    return candidate.Kind == expectedKind && candidate.RequestId == requestId;
                });
            if (item == Items.end()) return false;
            Items.erase(item);
            return true;
        }

        // Session owners cancel only their own pending work. Erasing matching
        // entries preserves the FIFO order and ownership of every other tag.
        size_t RemoveAll(RequestKind kind)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            size_t removed = 0;
            for (auto item = Items.begin(); item != Items.end();)
            {
                if (item->Kind == kind) { item = Items.erase(item); ++removed; }
                else ++item;
            }
            return removed;
        }

        // True when any queued request satisfies the predicate. The predicate
        // runs under the queue mutex and must not call back into this queue.
        template<class Pred> bool HasAny(const Pred& pred)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            return std::any_of(Items.begin(), Items.end(), pred);
        }

        // Removes the lowest-ranked admissible request; a rank of nullopt skips
        // the entry, ties keep queue order. Rank runs under the queue mutex and
        // must not call back into this queue.
        template<class Rank> bool RemoveLowest(Request& request, const Rank& rank)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            auto best = Items.end();
            double bestRank = 0.0;
            for (auto it = Items.begin(); it != Items.end(); ++it)
            {
                const std::optional<double> itemRank = rank(*it);
                if (itemRank && (best == Items.end() || *itemRank < bestRank)) { best = it; bestRank = *itemRank; }
            }
            if (best == Items.end()) return false;
            request = std::move(*best);
            Items.erase(best);
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

        bool Remove(Feedback& feedback, RequestKind expectedKind)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            const auto item = std::find_if(Items.begin(), Items.end(),
                [expectedKind](const Feedback& candidate) { return candidate.Kind == expectedKind; });
            if (item == Items.end()) return false;
            feedback = std::move(*item);
            Items.erase(item);
            return true;
        }

        size_t RemoveAll(RequestKind kind)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            size_t removed = 0;
            for (auto item = Items.begin(); item != Items.end();)
            {
                if (item->Kind == kind) { item = Items.erase(item); ++removed; }
                else ++item;
            }
            return removed;
        }

    private:
        std::mutex Mutex;
        std::deque<Feedback> Items;
    };
}
