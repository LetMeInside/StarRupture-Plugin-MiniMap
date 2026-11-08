// Exposes read-only terrain metadata and source-tile resolution for MiniMap.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

struct IPluginSelf;

namespace SDK
{
    class UTexture2D;
    struct FVector;
}

namespace MiniMapTerrainSource
{
    enum class Variant
    {
        Ordinary = 0,
        Radiation2 = 1
    };


    struct SourceTile
    {
        int GridX = 0;
        int GridY = 0;

        double LocalU = 0.0;
        double LocalV = 0.0;

        int RadiationLevel = 0;
        Variant SelectedVariant = Variant::Ordinary;

        SDK::UTexture2D* Texture = nullptr;
    };


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
