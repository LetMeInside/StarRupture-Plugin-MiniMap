#pragma once
#include <array>
#include <cstdint>

struct IPluginSelf;

namespace MiniMapConfig
{
    using Keybind = std::array<char, 128>;
    // Owned values only. Future sections can extend this value and the schema.
    struct Settings
    {
        bool VisibleAtStartup = true;
        Keybind ToggleMiniMap = {'F', '4', '\0'};
        bool FollowBuildDrone = true;
        bool RotatingMiniMap = true;
        bool ShowCoordinates = true;
        bool ShowCompass = true;
        bool ClearCache = false;
        bool ZoomWithMouseWheel = true;
        Keybind MouseWheelKey = {'L', 'e', 'f', 't', 'C', 'o', 'n', 't', 'r', 'o', 'l', '\0'};
        uint32_t WheelModifier = 1; // PluginWheelMod_LeftCtrl, validated with descriptor.
        Keybind ZoomInKey = {};
        Keybind ZoomOutKey = {};
        double ZoomStepFactor = 1.25;
        double InitialZoom = 100.0 / 300.0;
        uint64_t WheelRevision = 1;
        uint64_t TrackingRevision = 1;
    };

    // Purpose-specific owned values, copied together under the Settings mutex.
    struct TrackingSettings
    {
        bool FollowBuildDrone;
        uint64_t Revision;
    };
    struct RenderSettings
    {
        bool RotatingMiniMap;
        bool ShowCoordinates;
        bool ShowCompass;
        uint64_t TrackingRevision;
    };
    struct MouseWheelSettings
    {
        bool Enabled;
        uint32_t Modifier;
        uint64_t Revision;
    };

    bool Initialize(IPluginSelf* self);
    void Shutdown();
    Settings Get(); // Infrequent input registration only; returns owned bindings.
    bool VisibleAtStartup();
    bool ClearCache();
    double InitialZoom();
    double ZoomStepFactor();
    TrackingSettings GetTrackingSettings();
    RenderSettings GetRenderSettings();
    MouseWheelSettings GetMouseWheelSettings();
} // namespace MiniMapConfig
