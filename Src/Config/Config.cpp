#include "Config.h"
#include "plugin_helpers.h"

namespace
{
    static const ConfigEntry CONFIG_ENTRIES[] =
    {
        {
            "General",
            "Enabled",
            ConfigValueType::Boolean,
            "true",
            "Enable or disable MiniMap"
        },
        {
            "Rendering",
            "SplineRenderingEnabled",
            ConfigValueType::Boolean,
            "false",
            "Draw spline-based walkways, rails and related geometry on MiniMap"
        }
    };

    static const ConfigSchema SCHEMA =
    {
        CONFIG_ENTRIES,
        sizeof(CONFIG_ENTRIES) / sizeof(CONFIG_ENTRIES[0])
    };

    bool g_enabled = MiniMapConfig::DefaultEnabled;
    bool g_splineRenderingEnabled = MiniMapConfig::DefaultSplineRenderingEnabled;
}

namespace MiniMapConfig
{
    bool Initialize(IPluginSelf* self)
    {
        if (self == nullptr || self->config == nullptr)
        {
            LOG_ERROR("MiniMap: configuration interface is unavailable");
            return false;
        }

        if (!self->config->InitializeFromSchema(self, &SCHEMA))
        {
            LOG_ERROR("MiniMap: config schema initialization failed");
            return false;
        }

        self->config->ValidateConfig(self, &SCHEMA);
        g_enabled = self->config->ReadBool(self, "General", "Enabled", DefaultEnabled);
        g_splineRenderingEnabled = self->config->ReadBool(self, "Rendering", "SplineRenderingEnabled", DefaultSplineRenderingEnabled);
        LOG_INFO("MiniMap: Enabled = %s", g_enabled ? "true" : "false");
        LOG_INFO("MiniMap: SplineRenderingEnabled = %s", g_splineRenderingEnabled ? "true" : "false");
        return true;
    }

    bool IsEnabled()
    {
        return g_enabled;
    }

    bool IsSplineRenderingEnabled()
    {
        return g_splineRenderingEnabled;
    }
}
