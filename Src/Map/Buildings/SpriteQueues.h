#pragma once

#include <cstdint>
#include <memory>
#include <deque>
#include <mutex>
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

    // One FIFO carries both consumers. MiniMap uses only Kind, BlueprintPath,
    // and the renderer-stamped Session. BuildingComplex fields are copied at
    // admission and remain dormant until that consumer is implemented.
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

        // A consumer may service only its tag without skipping an earlier
        // request owned by another consumer; shared FIFO order stays intact.
        bool Remove(Request& request, RequestKind expectedKind)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (Items.empty() || Items.front().Kind != expectedKind) return false;
            request = std::move(Items.front());
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

        bool Remove(Feedback& feedback, RequestKind expectedKind)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (Items.empty() || Items.front().Kind != expectedKind) return false;
            feedback = std::move(Items.front());
            Items.pop_front();
            return true;
        }

    private:
        std::mutex Mutex;
        std::deque<Feedback> Items;
    };
}
