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
    struct SourceTile
    {
        int GridX = 0;
        int GridY = 0;

        double LocalU = 0.0;
        double LocalV = 0.0;

        SDK::UTexture2D* Texture = nullptr;
    };


    bool Initialize(
        IPluginSelf* self);

    void Shutdown();

    bool TryResolveSourceTile(
        const SDK::FVector& worldPosition,
        SourceTile& outSourceTile);
}

#endif
