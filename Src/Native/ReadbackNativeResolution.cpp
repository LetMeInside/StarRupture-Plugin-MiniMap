#if defined(MODLOADER_CLIENT_BUILD)
#include "ReadbackNativeAdapter.h"
#include "NativeApi.h"
namespace MiniMapReadbackNative
{
    namespace
    {
        const char* status = "native prerequisites not resolved";
    }

    const char* Status() { return status; }
    void SetStatus(const char* message) { status = message; }

    bool ResolvePrerequisites()
    {
        if (!MiniMapNative::IsInitialized())
        {
            status = "native prerequisites not resolved";
            return false;
        }
        status = "native signatures resolved; GPU submission remains blocked";
        return true;
    }

}
#endif
