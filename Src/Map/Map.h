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
    SDK::UWorld* GetWorld();
    bool HasWorld();

    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition);

    void Shutdown();
}

#endif
