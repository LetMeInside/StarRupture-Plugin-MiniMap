#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "../plugin.h"

#include <array>
#include <cstdint>
#include <vector>

// Stage A only: read-only Mass inventory, with no rendering or asset loading.
namespace MiniMapBuildingInventory
{
    struct Record
    {
        // Identity is valid only within this world generation.
        uint64_t WorldGeneration = 0;
        int32_t Index = 0;
        int32_t SerialNumber = 0;
        bool HasTransform = false;
        std::array<double, 3> Position = {};
        std::array<double, 4> Rotation = {}; // Quaternion X/Y/Z/W, not Euler angles.
        std::array<double, 3> Scale = {};

        bool HasPlacement = false;
        uint32_t BuildingId = 0;
        uint8_t BuildingType = 0xFF; // ECrBuildingType; 0xFF means unresolved.
        int32_t PlacementNameIndex = 0;
        uint32_t PlacementNameNumber = 0;
        int32_t UniqueNameIndex = 0;
        uint32_t UniqueNameNumber = 0;

        bool HasNetworkIdentity = false;
        uint32_t NetworkId = 0; // Optional; not used as the local inventory key.
        uint8_t Representation = 0xFF; // EMassRepresentationType, or unresolved.
        uint8_t PreviousRepresentation = 0xFF;
        int16_t HighResTemplate = -1;
        int16_t LowResTemplate = -1;
        uint16_t MeshDescriptor = 0xFFFF;
        bool HasPlacementFragment = false;
        bool HasSpline = false;
        bool SplineCurvesReady = false;
        int32_t SplinePointCount = 0;
        float SplineLength = 0.0f;
        bool HasColorTint = false;
        std::array<float, 4> ColorTint = {};

        bool operator==(const Record&) const = default;
    };

    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();
    // Copied values only; no engine pointers escape the collector.
    std::vector<Record> CopySnapshot();
    void Shutdown();
}

#endif
