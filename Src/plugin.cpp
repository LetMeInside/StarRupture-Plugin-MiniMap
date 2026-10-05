#include "plugin.h"
#include "plugin_helpers.h"
#include "Config/Config.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "Native/Fingerprints.h"
#include "Native/NativeApi.h"
#endif

IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

#ifdef MODLOADER_CLIENT_BUILD
static MiniMapFingerprints::ResolvedAddresses g_resolvedAddresses = {};
#endif

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1.0"
#endif

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/MiniMapUI.h"
#include "Map/Terrain.h"
#include "Map/Map.h"
#endif

#ifdef MODLOADER_SERVER_BUILD
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_SERVER
#else
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_CLIENT
#endif


static PluginInfo s_pluginInfo = {
    "MiniMap",
    MODLOADER_BUILD_TAG,
    "Kian369",
    "TODO: Add plugin description",
    PLUGIN_INTERFACE_VERSION,
    MINIMAP_EXPORTS_TARGET
};

extern "C" __declspec(dllexport)
void OnPluginLoadHooks(
    IPluginSelf* self,
    IPluginHookScanner* scanner)
{
#ifdef MODLOADER_CLIENT_BUILD

    g_resolvedAddresses = {};

    if (!MiniMapFingerprints::Resolve(
        self,
        scanner,
        g_resolvedAddresses))
    {
        LOG_ERROR(
            "MiniMap: native fingerprint resolution failed");

        MiniMapNative::Shutdown();
        return;
    }

    if (!MiniMapNative::Initialize(
        g_resolvedAddresses))
    {
        LOG_ERROR(
            "MiniMap: native API initialization failed");

        MiniMapNative::Shutdown();
        return;
    }

    LOG_INFO(
        "MiniMap: native API initialized");

#else

    (void)self;
    (void)scanner;

#endif
}

#ifdef MODLOADER_CLIENT_BUILD

static void OnWorldBeginPlay(SDK::UWorld* world)
{
    MiniMapMap::SetWorld(world);
    MiniMapTerrain::CancelPendingDiagnostic();

    LOG_INFO(
        "MiniMap: game world began: %p",
        world);

    MiniMapUI::Show();
}

static void OnExperienceLoadComplete()
{
    LOG_INFO("MiniMap: OnExperienceLoadComplete");

    if (!MiniMapTerrain::Initialize(g_self))
    {
        LOG_ERROR(
            "MiniMap: terrain initialization failed after experience load");
        return;
    }

    LOG_INFO(
        "MiniMap: terrain initialization complete");
}

static void OnAfterWorldEndPlay(
    SDK::UWorld* world,
    const char* worldName)
{
    LOG_INFO(
        "MiniMap: world ended: %s",
        worldName != nullptr ? worldName : "<null>");

    MiniMapUI::Hide();

    MiniMapMap::SetWorld(nullptr);
    MiniMapTerrain::Shutdown();
}

#endif

extern "C"
{
    __declspec(dllexport)
        PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport)
        bool PluginInit(IPluginSelf* self)
    {
        g_self = self;

        if (g_self == nullptr ||
            g_self->hooks == nullptr)
        {
            g_self = nullptr;
            return false;
        }

        if (!MiniMapConfig::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: configuration initialization failed");

            g_self = nullptr;
            return false;
        }

#ifdef MODLOADER_CLIENT_BUILD

        if (g_self->hooks->World == nullptr)
        {
            LOG_ERROR(
                "MiniMap: World hooks are unavailable");

            g_self = nullptr;
            return false;
        }

        if (!MiniMapUI::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: UI initialization failed");

            g_self = nullptr;
            return false;
        }

        LOG_INFO(
            "MiniMap: FSoftObjectPtr::LoadSynchronous = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.softObjectLoadSynchronous));

        LOG_INFO(
            "MiniMap: UWidgetBlueprintLibrary::"
            "GetBrushResourceAsTexture2D = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getBrushResourceAsTexture2D));

        LOG_INFO(
            "MiniMap: UStreamableRenderAsset::"
            "SetForceMipLevelsToBeResident = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.setForceMipLevelsToBeResident));

        LOG_INFO(
            "MiniMap: UStreamableRenderAsset::"
            "WaitForStreaming = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.waitForStreaming));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetNumResidentMips = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getNumResidentMips));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetNumMipsAllowed = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getNumMipsAllowed));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetNumMips = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getNumMips));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "StreamIn = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.streamIn));

        LOG_INFO(
            "MiniMap: UStreamableRenderAsset::"
            "WaitForPendingInitOrStreaming = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.waitForPendingInitOrStreaming));

        LOG_INFO(
            "MiniMap: UWorld::"
            "GetFirstPlayerController = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getFirstPlayerController));

        LOG_INFO(
            "MiniMap: AController::"
            "GetPawn<ACrCharacterPlayerBase> = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getPlayerPawn));

        LOG_INFO(
            "MiniMap: AController::"
            "GetControlRotation = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getControlRotation));

        LOG_INFO(
            "MiniMap: USceneComponent::"
            "K2_GetComponentLocation = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getComponentLocation));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetPlatformData = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getPlatformData));

        LOG_INFO(
            "MiniMap: FBulkData::"
            "GetBulkDataSize = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getBulkDataSize));

        LOG_INFO(
            "MiniMap: FBulkData::"
            "CanLoadFromDisk = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.canLoadFromDisk));

        LOG_INFO(
            "MiniMap: FBulkData::"
            "GetCopy = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getBulkDataCopy));

        LOG_INFO(
            "MiniMap: FMemory::"
            "Free = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.memoryFree));

        g_self->hooks->World->RegisterOnWorldBeginPlay(
            &OnWorldBeginPlay);

        g_self->hooks->World->RegisterOnAfterWorldEndPlay(
            &OnAfterWorldEndPlay);

        g_self->hooks->World->RegisterOnExperienceLoadComplete(
            &OnExperienceLoadComplete);

        LOG_INFO(
            "MiniMap: registered world lifecycle callbacks");

        if (!MiniMapTerrain::RegisterDiagnostics(g_self))
        {
            LOG_ERROR(
                "MiniMap: failed to register terrain diagnostics");
        }

#endif

        LOG_INFO(
            "MiniMap: initialization complete");

        return true;
    }

    __declspec(dllexport)
        void PluginShutdown()
    {
        LOG_INFO(
            "MiniMap: shutting down");

#ifdef MODLOADER_CLIENT_BUILD

        MiniMapUI::Hide();

        if (g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->World != nullptr)
        {
            g_self->hooks->World->UnregisterOnExperienceLoadComplete(
                &OnExperienceLoadComplete);

            g_self->hooks->World->UnregisterOnWorldBeginPlay(
                &OnWorldBeginPlay);

            g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                &OnAfterWorldEndPlay);
        }

        MiniMapTerrain::UnregisterDiagnostics();
        MiniMapTerrain::Shutdown();
        MiniMapMap::Shutdown();
        MiniMapUI::Shutdown();

        // NativeApi should be the last subsystem torn down.
        // Other subsystems depend on it for their own shutdown paths.
        MiniMapNative::Shutdown();
#endif

        g_self = nullptr;
    }
}