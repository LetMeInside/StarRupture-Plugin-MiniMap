// Key-scan build only (MiniMapKeyScanOnly): stands in for ReadbackNativeAdapter.cpp,
// which needs Unreal Engine headers. Every call reports "unavailable".
// Status, SetStatus and ResolvePrerequisites live in ReadbackNativeResolution.cpp.
#if defined(MODLOADER_CLIENT_BUILD)
#include "ReadbackNativeAdapter.h"
namespace MiniMapReadbackNative
{
    bool Preflight() { return false; }
    Job* CreateExternalJob(void*, uint32_t, uint32_t) { return nullptr; }
    bool DiscardExternalBeforeCapture(Job*&) { return false; }
    bool MarkExternalCaptureCommitted(Job*) { return false; }
    bool ExternalRejectedWithoutReadback(const Job*) { return false; }
    bool CompleteExternalCaptureRetirement(Job*) { return false; }
    bool CaptureRetirementRendererAvailable() { return false; }
    const MiniMapFloatPixels::Stats* FloatStats(const Job*) { return nullptr; }
    const std::vector<uint8_t>* FloatPixels(const Job*) { return nullptr; }
    void Service(Job*) {}
    void Cancel(Job*) {}
    Report Inspect(const Job*) { return {}; }
    bool DestroyIfRetired(Job*&) { return false; }
}
#endif
