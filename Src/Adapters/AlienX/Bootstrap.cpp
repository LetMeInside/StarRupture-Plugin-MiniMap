#include "Bootstrap.h"
#include "Log.h"
#include "Configuration.h"
#include "Input.h"
#include "Objects.h"
#include "RequiredScanner.h"
#include "RuntimeEvents.h"
#include "NativeHooks.h"
#include "Viewport.h"
#include "Textures.h"
#include "Services/RuntimeBinding.h"
#include "plugin.h"
#include "plugin_helpers.h"

#include <optional>

#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/NativeApi.h"
#endif

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "1.2.0"
#endif

namespace
{
    // One adapter-owned identity and one immutable Runtime for this DLL instance.
    IPluginSelf* g_self = nullptr;
    MiniMap::Adapters::AlienX::Log g_log;
    MiniMap::Adapters::AlienX::Configuration g_configuration;
    MiniMap::Adapters::AlienX::Input g_input;
#if defined(MODLOADER_CLIENT_BUILD)
    MiniMap::Adapters::AlienX::RuntimeEvents g_events;
    MiniMap::Adapters::AlienX::NativeHooks g_hooks;
    MiniMap::Adapters::AlienX::Viewport g_viewport;
    MiniMap::Adapters::AlienX::Textures g_textures;
    const MiniMap::Services::Runtime g_runtime{g_log, &g_configuration, nullptr, &g_input,
        &MiniMap::Adapters::AlienX::GetObjects(), &g_events, &g_hooks, &g_viewport, &g_textures};
#else
    const MiniMap::Services::Runtime g_runtime{g_log, &g_configuration};
#endif
    std::optional<MiniMap::Services::Detail::RuntimeBinding> g_binding;

    struct HookBaseline
    {
        bool Captured = false;
        const void* Self = nullptr;
        const void* Hooks = nullptr;
        const void* Engine = nullptr;
        const void* ImGuiTextures = nullptr;
    } g_baseline;

#ifdef MODLOADER_SERVER_BUILD
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_SERVER
#else
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_CLIENT
#endif

    PluginInfo g_pluginInfo = {
        "MiniMap",
        MODLOADER_BUILD_TAG,
        "Kian369",
        "A configurable on-screen minimap for StarRupture",
        PLUGIN_INTERFACE_VERSION,
        MINIMAP_EXPORTS_TARGET
    };

    void ReleaseRuntime() noexcept
    {
        // Called only after the existing core cleanup/rollback has returned.
#if defined(MODLOADER_CLIENT_BUILD)
        if (!g_textures.Stop())
        {
            LOG_ERROR("MiniMap: texture drain incomplete; runtime retained");
            return;
        }
#endif
        g_baseline = {};
        g_binding.reset();
        g_self = nullptr;
    }
}

namespace MiniMap::Services
{
    ILog* TryLog() noexcept
    {
        if (const auto* runtime = TryRuntime()) return &runtime->log;
        // Explicit scan/bootstrap path: no partially initialized Runtime.
        return g_self ? &g_log : nullptr;
    }
}

namespace MiniMap::Adapters::AlienX
{
    IPluginSelf* GetSelf() noexcept { return g_self; }
    Configuration& GetConfiguration() noexcept { return g_configuration; }
    Input& GetInput() noexcept { return g_input; }
#if defined(MODLOADER_CLIENT_BUILD)
    RuntimeEvents& GetRuntimeEvents() noexcept { return g_events; }
    Viewport& GetViewport() noexcept { return g_viewport; }
    Textures& GetTextures() noexcept { return g_textures; }
#endif

    void CapturePluginInitHookBaseline(IPluginSelf* self) noexcept
    {
        auto* hooks = self ? self->hooks : nullptr;
        g_baseline = {true, self, hooks, hooks ? hooks->Engine : nullptr,
            hooks ? hooks->ImGuiTextures : nullptr};
    }

    void LogTerrainHookAvailability(IPluginSelf* self) noexcept
    {
        auto* hooks = self ? self->hooks : nullptr;
        auto* engine = hooks ? hooks->Engine : nullptr;
        auto* textures = hooks ? hooks->ImGuiTextures : nullptr;
        LOG_ERROR(
            "MiniMap: terrain initialization failed: required hooks are unavailable; "
            "available self=%d hooks=%d engine=%d imguiTextures=%d; "
            "matches PluginInit baseline captured=%d self=%d hooks=%d engine=%d imguiTextures=%d",
            int(self != nullptr), int(hooks != nullptr), int(engine != nullptr),
            int(textures != nullptr), int(g_baseline.Captured),
            int(g_baseline.Captured && self && g_baseline.Self && static_cast<const void*>(self) == g_baseline.Self),
            int(g_baseline.Captured && hooks && g_baseline.Hooks && static_cast<const void*>(hooks) == g_baseline.Hooks),
            int(g_baseline.Captured && engine && g_baseline.Engine && static_cast<const void*>(engine) == g_baseline.Engine),
            int(g_baseline.Captured && textures && g_baseline.ImGuiTextures &&
                static_cast<const void*>(textures) == g_baseline.ImGuiTextures));
    }
}

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo()
{
    return &g_pluginInfo;
}

extern "C" __declspec(dllexport) void OnPluginLoadHooks(
    IPluginSelf* self, IPluginHookScanner* scanner)
{
    // The loader calls this before PluginInit. Borrow self only for this scope;
    // the scanner adapter and its borrowed session exist only inside this call.
    if (g_binding) return;
    g_self = self;
    struct ScanLogScope { ~ScanLogScope() noexcept { g_self = nullptr; } } scope;
    try
    {
#if defined(MODLOADER_CLIENT_BUILD)
        if (!self || !scanner || !scanner->Resolve ||
            !scanner->ReportFailure || !scanner->HasFailures)
        {
            LOG_ERROR("MiniMap: native fingerprint resolution failed: invalid required scanner callback arguments");
            MiniMapNative::Shutdown();
            // A malformed callback can only be reported when the loader supplied
            // its identity and reporting operation. No alternative failure API.
            if (self && scanner && scanner->ReportFailure)
                scanner->ReportFailure(self, "MiniMap::NativeDependencies", "invalid required scanner callback arguments");
            return;
        }
        MiniMap::Adapters::AlienX::RequiredScanner requiredScanner(*self, *scanner);
        MiniMap::Adapters::AlienX::ResolveNativeDependencies(requiredScanner);
        if (requiredScanner.HasFailures() && MiniMapNative::IsInitialized())
            MiniMapNative::Shutdown();
#else
        (void)scanner; // Existing server/generic callback performs no native scans.
#endif
    }
    catch (...)
    {
        LOG_ERROR("MiniMap: exception during native dependency initialization");
#if defined(MODLOADER_CLIENT_BUILD)
        MiniMapNative::Shutdown();
#endif
        try
        {
            if (self && scanner && scanner->ReportFailure)
                scanner->ReportFailure(self, "MiniMap::NativeDependencies", "exception during native dependency initialization");
        }
        catch (...) {} // Never cross the exported loader boundary.
    }
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    if (g_binding) return false; // Never replace an active composition/identity.
    g_baseline = {};
    g_self = self;
    g_binding.emplace(g_runtime);
    if (!g_binding->IsBound())
    {
        ReleaseRuntime();
        return false;
    }
    try
    {
#if defined(MODLOADER_CLIENT_BUILD)
        g_textures.Initialize(self); // No native texture acquisitions/releases.
#endif
        if (MiniMap::Initialize()) return true;
        // Existing initialization guards/explicit rollback finish while bound.
        ReleaseRuntime();
    }
    catch (...)
    {
        LOG_ERROR("MiniMap: exception during core initialization");
#if defined(MODLOADER_CLIENT_BUILD)
        if (!g_textures.CloseAcquisitions())
        {
            LOG_ERROR("MiniMap: reentrant texture operation during initialization cleanup; runtime retained");
            return false;
        }
#endif
        try
        {
            // Exceptional rollback uses the existing core shutdown order, with
            // the same Runtime/identity still alive for every cleanup consumer.
            MiniMap::Shutdown();
            ReleaseRuntime();
        }
        catch (...)
        {
            // An interrupted cleanup is not completion. Do not unbind services
            // beneath remaining consumers or retry ambiguous native operations.
            LOG_ERROR("MiniMap: initialization cleanup exception; runtime retained because core cleanup is incomplete");
        }
    }
    return false;
}

extern "C" __declspec(dllexport) void PluginShutdown()
{
#if defined(MODLOADER_CLIENT_BUILD)
    if (!g_textures.CloseAcquisitions())
    {
        LOG_ERROR("MiniMap: reentrant texture operation during shutdown; runtime retained");
        return;
    }
#endif
    try
    {
        MiniMap::Shutdown();
    }
    catch (...)
    {
        LOG_ERROR("MiniMap: shutdown exception; runtime retained because core cleanup is incomplete");
        return;
    }
    ReleaseRuntime();
}
