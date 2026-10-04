#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

namespace SDK
{
    class UWorld;
    struct FVector;
}

namespace MiniMapMap
{
    void SetWorld(SDK::UWorld* world);
    bool HasWorld();

    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition);

    void Shutdown();
}

#endif