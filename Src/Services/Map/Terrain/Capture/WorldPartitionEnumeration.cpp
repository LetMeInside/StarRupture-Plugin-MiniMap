// Constructs and invokes genuine TFunctionRef without including generated SDK implementations.
// The ordinary callback boundary preserves synchronous native enumeration and early stopping.
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/ReadbackUEConfiguration.h"
#include "WorldPartitionEnumeration.h"
#include "Native/NativeApi.h"

static_assert(sizeof(TFunctionRef<bool(::UWorldPartition*)>) == 16);
static_assert(alignof(TFunctionRef<bool(::UWorldPartition*)>) == 8);

namespace Services::Map::Terrain::Capture
{
    /** Keeps the real function reference and its callable alive until native enumeration returns. */
    void WorldPartitionEnumeration::ForEach(SDK::UWorldPartitionSubsystem* subsystem,
        Visitor visitor, void* context)
    {
        auto forward = [visitor, context](::UWorldPartition* partition)
        {
            return visitor(context, reinterpret_cast<SDK::UWorldPartition*>(partition));
        };
        MiniMapNative::Get()->captureAdmission.enumeratePartitions(subsystem, forward);
    }
}
#endif
