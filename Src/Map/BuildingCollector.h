#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../plugin.h"
#include "MapTransform.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniMapBuildingInventory { struct Record; }
namespace MiniMapBuildingCollector
{
    struct Bounds
    {
        double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        bool Valid = false;
    };
    enum class Coverage : uint8_t { Unresolved, OriginPadding, SplineHull };
    enum Source : uint8_t { Indexed = 1, OffGrid = 2, IndexedSpline = 4 };
    struct Record
    {
        uint64_t Generation = 0;
        int32_t Index = 0, Serial = 0;
        uint32_t BuildingId = 0;
        int32_t DefinitionIndex = 0;
        uint32_t DefinitionNumber = 0;
        int32_t DefinitionObjectIndex = 0; // World-scoped metadata identity; no UObject pointer.
        uint8_t Category = 0xFF, Sources = 0;
        bool TransformValid = false, DefinitionValid = false, HasSpline = false;
        std::array<double, 3> Position = {}, Scale = {};
        std::array<double, 4> Rotation = {};
        std::array<float, 4> Tint = { 1, 1, 1, 1 };
        Bounds Extent;
        Coverage BoundsKind = Coverage::Unresolved;
        double RefreshedAtSeconds = 0; // steady_clock; dynamic data/bounds sample time.
    };
    struct Snapshot
    {
        uint64_t Generation = 0;
        Bounds Region;
        bool Complete = false; // Cache warm-up/unresolved geometry is explicit.
        double PublishedAtSeconds = 0;
        std::vector<Record> Records;
    };
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();
    void SetViewport(const MiniMapMap::Transform& transform);
    std::shared_ptr<const Snapshot> GetSnapshot();
    void Shutdown();
    // Diagnostic sink only. Never used to populate production records/caches.
    bool BeginComparison(const SDK::UWorld* world);
    void ObserveBaseline(const MiniMapBuildingInventory::Record& record);
    void EndComparison();
}
#endif
