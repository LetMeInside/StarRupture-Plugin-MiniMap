// Defines the validated Unreal runtime symbols used by the client native integration.
#if defined(MODLOADER_CLIENT_BUILD)
#include "ReadbackUEConfiguration.h"
#define RHI_API
#define RENDERCORE_API
#define WITH_MGPU 1
#define WITH_RHI_BREADCRUMBS 0
#include "RHIResources.h"
#include "NativeApi.h"
#include <cstdlib>

// V2 readback and header-inline descriptor defaults require this shared global. This is a validated
// snapshot of the native global, never a substituted single-GPU constant.
uint32 GNumExplicitGPUsForRendering = 0;

// Real UE header dependencies, dispatched only after native resolution. There
// are no global owning UE objects and no engine calls in static constructors.
void* FMemory::Malloc(SIZE_T size, uint32 alignment)
{
    const auto* api = MiniMapNative::Get();
    const auto fn = api ? api->asset.memoryMalloc : nullptr;
    if (!fn) std::abort();
    return fn(size, alignment);
}
void FMemory::Free(void* ptr)
{
    const auto* api = MiniMapNative::Get();
    const auto fn = api ? api->texture.memoryFree : nullptr;
    if (!fn) std::abort();
    fn(ptr);
}
void FRHIResource::MarkForDelete() const
{
    const auto* api = MiniMapNative::Get();
    const auto fn = api ? api->texture.markRHIResourceForDelete : nullptr;
    if (!fn) std::abort();
    fn(this);
}
FNameEntryId FNameEntryId::FromValidEName(EName name)
{
    const auto* api = MiniMapNative::Get();
    const auto fn = api ? api->asset.fromValidEName : nullptr;
    if (!fn) std::abort();
    FNameEntryId result;
    // CL-127004 0x014B68C0: RCX=result storage, EDX=EName, RAX=result.
    fn(&result, name);
    return result;
}
#endif // MODLOADER_CLIENT_BUILD
