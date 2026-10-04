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

    bool TryResolveSourceTile(
        const SDK::FVector& worldPosition,
        SourceTile& outSourceTile,
        bool verbose = false);
}

#endif
