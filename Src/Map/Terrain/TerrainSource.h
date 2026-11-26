// Exposes read-only terrain metadata and source-tile resolution for MiniMap.
#pragma once

#include <cstdint>
#include "RegisteredTerrainImage.h"

#if defined(MODLOADER_CLIENT_BUILD)

struct IPluginSelf;

namespace SDK
{
    class UTexture2D;
    struct FVector;
}

namespace MiniMapTerrainSource
{
    struct FieldOffsets
    {
        int32_t DevSettingsTerrainData = -1;
        int32_t TerrainDataPivot = -1;
        int32_t TerrainDataSegmentSize = -1;
        int32_t TerrainDataSegments = -1;
        int32_t SegmentGridIndex = -1;
        int32_t SegmentTexture = -1;
        int32_t SegmentTextureRadiation2 = -1;
        int32_t WorldGameState = -1;
        int32_t GameStateReplicationHelper = -1;
        int32_t RadiationLevel = -1;
        uint32_t TerrainSegmentStride = 0;
    };

    enum class Variant
    {
        Ordinary = 0,
        Radiation2 = 1
    };


    /** Resolution result shares immutable registered pixels and borrows native Texture only synchronously. */
    struct SourceTile
    {
        // CPU storage is shared with the immutable image owner; native textures remain synchronous borrows.
        std::optional<RegisteredTerrainImage> RegisteredImage;
        int GridX = 0;
        int GridY = 0;

        double LocalU = 0.0;
        double LocalV = 0.0;

        int RadiationLevel = 0;
        Variant SelectedVariant = Variant::Ordinary;

        SDK::UTexture2D* Texture = nullptr;
    };

    void SetFieldOffsets(const FieldOffsets& offsets);

    const FieldOffsets& GetFieldOffsets();


    bool Initialize(
        IPluginSelf* self);

    void Shutdown();

    // Returns authored terrain coverage dimensions in native centimetre map units.
    // Outputs are cleared on failure; the bounds use actual records, not inferred world limits.
    bool TryGetWorldDimensions(
        double& outWidthMapUnits,
        double& outHeightMapUnits);

    // Copies the terrain-aligned sparse-index origin from the loaded terrain
    // definition. Game-thread only; no terrain texture or chunk work is done.
    bool TryGetWorldOrigin(double& outX, double& outY);

    bool TryResolveSourceTile(
        const SDK::FVector& worldPosition,
        SourceTile& outSourceTile,
        bool verbose = false);
}

#endif
