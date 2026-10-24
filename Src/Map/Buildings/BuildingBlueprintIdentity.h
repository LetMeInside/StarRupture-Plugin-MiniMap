#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <memory>
#include <string>
namespace SDK {class UAuActorPlacementData;}
namespace MiniMapBuildingBlueprintIdentity
{
    bool Initialize();
    void Shutdown();
    // Nonloading conversion within the definition owner's synchronous scope.
    std::shared_ptr<const std::string> Copy(const SDK::UAuActorPlacementData&);
}
#endif
