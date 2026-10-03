#include "plugin.h"
#include "plugin_helpers.h"
#include "Config/Config.h"

IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1.0"
#endif

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/MiniMapUI.h"
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
void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
{
    (void)self;
    (void)scanner;
}

#ifdef MODLOADER_CLIENT_BUILD

static void OnWorldBeginPlay(
    SDK::UWorld* world)
{
    (void)world;

    LOG_INFO(
        "MiniMap: game world began");

    MiniMapUI::Show();
}

static void OnAfterWorldEndPlay(
    SDK::UWorld* world,
    const char* worldName)
{
    (void)world;

    LOG_INFO(
        "MiniMap: world ended: %s",
        worldName != nullptr ? worldName : "<null>");

    MiniMapUI::Hide();
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

        g_self->hooks->World->RegisterOnWorldBeginPlay(
            &OnWorldBeginPlay);

        g_self->hooks->World->RegisterOnAfterWorldEndPlay(
            &OnAfterWorldEndPlay);

        LOG_INFO(
            "MiniMap: registered world lifecycle callbacks");

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
            g_self->hooks->World->UnregisterOnWorldBeginPlay(
                &OnWorldBeginPlay);

            g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                &OnAfterWorldEndPlay);
        }

        MiniMapUI::Shutdown();

#endif

        g_self = nullptr;
    }
}