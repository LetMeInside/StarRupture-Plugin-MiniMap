#include "Config.h"
#include "plugin_helpers.h"
#include "Services/Configuration.h"
#include "Services/Runtime.h"
#include "Services/Input.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Input/MouseWheel.h"
#include "UI/MiniMapUI.h"
#include "Map/Terrain/Terrain.h"
#endif
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <limits>
#include <string_view>
#include <utility>

namespace
{
    using MiniMapConfig::Keybind;
    using MiniMapConfig::Settings;
    namespace S = MiniMap::Services;
    // Descriptions supply the existing human labels for the loader editor.
    constexpr S::SettingSpec kEntries[] = {
        {"General", "VisibleAtStartup", S::SettingKind::Boolean, "true",
         "Visible at Startup: show the widget when entering the next gameplay world."},
        {"General", "ToggleMiniMap", S::SettingKind::KeyBinding, "F4",
         "Toggle MiniMap: show or hide the widget for this world."},
        {"General", "FollowBuildDrone", S::SettingKind::Boolean, "true",
         "MiniMap Follows Build Drone: track the active building drone; otherwise track the physical character."},
        {"General", "RotatingMiniMap", S::SettingKind::Boolean, "true",
         "Rotating MiniMap: heading up when enabled; source-map north up otherwise."},
        {"General", "ShowCoordinates", S::SettingKind::Boolean, "true",
         "Show Coordinates: display the physical character's coordinates in the selected units."},
        {"General", "CoordsInUEUnits", S::SettingKind::Boolean, "false",
         "Coordinates in UE Units: show physical character X/Y/Z in Unreal Engine units (centimeters); otherwise show X/Y in meters."},
        {"General", "ShowCompass", S::SettingKind::Boolean, "true",
         "Show Compass: draw cardinal labels independently of coordinates."},
        {"General", "ClearCache", S::SettingKind::Boolean, "false",
         "Clear Cache on Startup/Exit: clear MiniMap terrain/building caches at plugin startup and safe shutdown, not "
         "immediately on edit."},
        {"Zoom", "ZoomWithMouseWheel", S::SettingKind::Boolean, "true",
         "Zoom with Mouse Wheel: vertical wheel zoom while the activation modifier is held and the widget is visible."},
        {"Zoom", "MouseWheelKey", S::SettingKind::KeyBinding, "LeftControl",
         "Mouse Wheel Activation Key: one sided Left/Right Control, Shift or Alt; no other keys or combinations."},
        {"Zoom", "ZoomInKey", S::SettingKind::KeyBinding, "",
         "Zoom In: one configured step per press during visible gameplay; independent of wheel enable."},
        {"Zoom", "ZoomOutKey", S::SettingKind::KeyBinding, "",
         "Zoom Out: one configured step per press during visible gameplay; independent of wheel enable."},
        {"Zoom", "ZoomStepFactor", S::SettingKind::Float, "1.25",
         "Zoom Step Factor: divide meters per pixel by this factor per positive wheel notch or zoom-in press; inverse "
         "for "
         "zoom-out.",
         1.01f, 2.0f},
        {"Zoom", "InitialZoom", S::SettingKind::Float, "0.3333333333333333",
         "Initial Zoom: meters per pixel for the next terrain session. Native Float editing rounds values; the default "
         "retains exact 100/300 zoom. Editor: 0.13-0.66; INI: 0.125 through 2/3 meters per pixel.",
         0.13f, 0.66f},
        {"Buildings", "ShowLayer", S::SettingKind::Boolean, "true",
         "Show Building Layer: display building sprites and splines; retain pending capture work while hidden."},
        {"Buildings", "RenderDroneRails", S::SettingKind::Boolean, "true",
         "Render Drone Rails: display drone rail splines when the building layer is shown."},
        {"Fog Of War", "DisableFog", S::SettingKind::Boolean, "false",
         "Disable Fog of War: bypass MiniMap visual fog and fog-based marker hiding; native exploration is unchanged."},
        {"Fog Of War", "FeatherRange", S::SettingKind::Float, "55.0",
         "Visibility Feather Range: outer radius in meters; must exceed the fixed 40 m clear radius.",
         41.0f, 200.0f},
        {"Alien Actors", "ShowLayer", S::SettingKind::Boolean, "true",
         "Show Alien Actor Layer: display observed active enemies without changing observation."},
        {"Foundables", "ShowLayer", S::SettingKind::Boolean, "true",
         "Show Foundables Layer: display foundable icons and container loot indicators."},
        {"Foundables", "ShowLootInBases", S::SettingKind::Boolean, "true",
         "Show Loot in Abandoned Bases: display loot indicators for containers in abandoned bases."},
        {"Foundables", "MarkerScale", S::SettingKind::Float, "1.0",
         "Foundable Marker Scale: positive size multiplier for foundable icons and loot dots; positions stay fixed.",
         0.25f, 3.0f},
        {"Points Of Interest", "ShowLayer", S::SettingKind::Boolean, "true",
         "Show POI Layer: display eligible POI markers without changing discovery."},
        {"Points Of Interest", "MarkerScale", S::SettingKind::Float, "1.0",
         "POI Marker Scale: positive icon-size multiplier; positions and discovery stay fixed.",
         0.25f, 3.0f}};
    enum Field
    {
        Visible,
        Toggle,
        Follow,
        Rotating,
        Coordinates,
        CoordinateUnits,
        Compass,
        ClearCache,
        WheelEnabled,
        WheelKey,
        ZoomIn,
        ZoomOut,
        Step,
        Initial,
        BuildingLayer,
        DroneRails,
        DisableFog,
        Feather,
        AlienLayer,
        FoundableLayer,
        LootInBases,
        FoundableScale,
        POILayer,
        POIScale,
        FieldCount
    };
    static_assert(FieldCount == std::size(kEntries));
    static_assert(FieldCount == 24);
    static_assert(ClearCache + 1 == 8); // General entries precede Zoom.
    std::mutex g_settingsMutex;
    Settings g_settings;
    bool g_listening = false;     // Lifecycle only.
    S::Registration g_observation{}; // Borrowed cancellation identity, not settings ownership.
    bool g_acceptChanges = false; // Protected with Settings, including shutdown admission.

    bool Equal(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
        for (size_t i = 0; i < a.size(); ++i)
            if (lower(a[i]) != lower(b[i]))
                return false;
        return true;
    }
    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);
        return text;
    }
    bool Copy(const char* text, Keybind& out)
    {
        if (!text)
            return false;
        const size_t length = strnlen_s(text, out.size());
        if (length == out.size())
            return false;
        out = {};
        std::memcpy(out.data(), text, length);
        return true;
    }
    // Exact descriptor vocabulary from the public v70 enum and loader table.
    // clang-format off
constexpr const char* kKeys[] = {
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
    "Zero","One","Two","Three","Four","Five","Six","Seven","Eight","Nine",
    "Escape","Tab","CapsLock","SpaceBar","Enter","BackSpace","Delete","Insert",
    "LeftShift","RightShift","LeftControl","RightControl","LeftAlt","RightAlt",
    "Up","Down","Left","Right","Home","End","PageUp","PageDown",
    "Tilde","Hyphen","Equals","LeftBracket","RightBracket","Backslash","Semicolon","Apostrophe","Comma","Period","Slash",
    "NumPadZero","NumPadOne","NumPadTwo","NumPadThree","NumPadFour","NumPadFive","NumPadSix","NumPadSeven","NumPadEight","NumPadNine",
    "Add","Subtract","Multiply","Divide","Decimal","LeftMouseButton","RightMouseButton","MiddleMouseButton","ThumbMouseButton","ThumbMouseButton2"
};
    // clang-format on
    static_assert(std::size(kKeys) == 101); // Reviewed vocabulary; native enum check is adapter-owned.
    bool SidedModifier(std::string_view token)
    {
        return Equal(token, "LeftControl") || Equal(token, "RightControl") || Equal(token, "LeftShift") ||
               Equal(token, "RightShift") || Equal(token, "LeftAlt") || Equal(token, "RightAlt");
    }
    bool ValidBinding(std::string_view text, bool optional)
    {
        if (text.empty())
            return optional;
        unsigned modifiers = 0;
        while (true)
        {
            const size_t plus = text.find('+');
            const auto token = Trim(text.substr(0, plus));
            if (token.empty())
                return false;
            if (plus == std::string_view::npos)
            {
                for (auto key : kKeys)
                    if (Equal(token, key))
                        return !modifiers || !SidedModifier(token);
                return false;
            }
            const unsigned bit = Equal(token, "Ctrl") || Equal(token, "Control") ? 1
                                 : Equal(token, "Shift")                         ? 2
                                 : Equal(token, "Alt")                           ? 4
                                                                                 : 0;
            if (!bit || (modifiers & bit))
                return false;
            modifiers |= bit;
            text.remove_prefix(plus + 1);
        }
    }
    struct Parsed
    {
        bool Boolean = false;
        double Number = 0;
        Keybind Binding{};
        uint32_t Modifier = 0;
    };
    const char* ValidationRule(int field)
    {
        switch (field)
        {
        case Toggle:
            return "one supported key, optionally prefixed by Ctrl/Shift/Alt";
        case ZoomIn:
        case ZoomOut:
            return "empty or one supported key with optional Ctrl/Shift/Alt prefixes";
        case WheelKey:
            return "one of LeftControl, RightControl, LeftShift, RightShift, LeftAlt, RightAlt";
        case Step:
            return "finite number in [1.01, 2.0]";
        case Initial:
            return "finite meters-per-pixel number in [0.125, 2/3]";
        case Feather:
            return "finite outer radius greater than 40 meters, within finite float range";
        case FoundableScale:
        case POIScale:
            return "finite positive float-convertible scale with finite positive marker geometry";
        default:
            return "true/false, yes/no or 1/0";
        }
    }
    bool Parse(int field, const Keybind& owned, Parsed& out)
    {
        const auto text = Trim(owned.data());
        switch (field)
        {
        case Toggle:
        case ZoomIn:
        case ZoomOut:
            out.Binding = owned; // Exact string identity is required by native rebinding.
            return ValidBinding(owned.data(), field != Toggle);
        case WheelKey: {
            constexpr const char* names[] = {"LeftControl", "RightControl", "LeftShift",
                                             "RightShift",  "LeftAlt",      "RightAlt"};
            constexpr uint32_t bits[] = {uint32_t(S::WheelModifier::LeftControl), uint32_t(S::WheelModifier::RightControl),
                uint32_t(S::WheelModifier::LeftShift), uint32_t(S::WheelModifier::RightShift),
                uint32_t(S::WheelModifier::LeftAlt), uint32_t(S::WheelModifier::RightAlt)};
            for (size_t i = 0; i < std::size(names); ++i)
                if (Equal(owned.data(), names[i]))
                {
                    out.Binding = owned;
                    out.Modifier = bits[i];
                    return true;
                }
            return false;
        }
        case Step:
        case Feather:
        case FoundableScale:
        case POIScale:
        case Initial: {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out.Number);
            if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                !std::isfinite(out.Number))
                return false;
            if (field == Step)
                return out.Number >= 1.01 && out.Number <= 2.0;
            if (field == Feather)
            {
                // Float editor compatibility; radius*100 and its square remain
                // finite in the existing double-precision feather calculation.
                return out.Number > 40.0 && out.Number <= double((std::numeric_limits<float>::max)());
            }
            if (field == FoundableScale || field == POIScale)
            {
                // Bound BEFORE conversion/multiplication: out-of-range float
                // casts/products must not be used to detect overflow after it occurs.
                // Supported zoom yields 16..24 px icons; Foundables dots use 2.5/5 px.
                const double maximum = double((std::numeric_limits<float>::max)()) / 24.0;
                if (!(out.Number > 0.0) || out.Number > maximum)
                    return false;
                const float scale = static_cast<float>(out.Number);
                if (!(scale > 0.0f) || !std::isfinite(scale) ||
                    double(scale) * 24.0 > double((std::numeric_limits<float>::max)()))
                    return false;
                const auto validGeometry = [scale](float base)
                {
                    const float size = base * scale;
                    return std::isfinite(size) && size > 0.0f;
                };
                return validGeometry(16.0f) && validGeometry(24.0f) &&
                    (field != FoundableScale || (validGeometry(2.5f) && validGeometry(5.0f)));
            }
            // Schema formatter normalizes Float defaults to six decimal places.
            // Recognize that default and the Float representation of 100/300.
            if (out.Number == 0.333333 || out.Number == double(float(100.0 / 300.0)))
                out.Number = 100.0 / 300.0;
            return out.Number >= 0.125 && out.Number <= 2.0 / 3.0;
        }
        default:
            if (Equal(text, "true") || Equal(text, "yes") || text == "1")
            {
                out.Boolean = true;
                return true;
            }
            if (Equal(text, "false") || Equal(text, "no") || text == "0")
            {
                out.Boolean = false;
                return true;
            }
            return false;
        }
    }
    void Apply(Settings& s, int field, const Parsed& v)
    {
        switch (field)
        {
        case Visible:
            s.VisibleAtStartup = v.Boolean;
            break;
        case Toggle:
            s.ToggleMiniMap = v.Binding;
            break;
        case Follow:
            if (s.FollowBuildDrone != v.Boolean)
                ++s.TrackingRevision;
            s.FollowBuildDrone = v.Boolean;
            break;
        case Rotating:
            s.RotatingMiniMap = v.Boolean;
            break;
        case Coordinates:
            s.ShowCoordinates = v.Boolean;
            break;
        case CoordinateUnits:
            s.CoordsInUEUnits = v.Boolean;
            break;
        case Compass:
            s.ShowCompass = v.Boolean;
            break;
        case ClearCache:
            s.ClearCache = v.Boolean;
            break;
        case WheelEnabled:
            s.ZoomWithMouseWheel = v.Boolean;
            ++s.WheelRevision;
            break;
        case WheelKey:
            s.MouseWheelKey = v.Binding;
            s.WheelModifier = v.Modifier;
            ++s.WheelRevision;
            break;
        case ZoomIn:
            s.ZoomInKey = v.Binding;
            break;
        case ZoomOut:
            s.ZoomOutKey = v.Binding;
            break;
        case Step:
            s.ZoomStepFactor = v.Number;
            break;
        case Initial:
            s.InitialZoom = v.Number;
            break;
        case BuildingLayer:
            s.ShowBuildingLayer = v.Boolean;
            break;
        case DroneRails:
            s.RenderDroneRails = v.Boolean;
            break;
        case DisableFog:
            s.DisableFogOfWar = v.Boolean;
            break;
        case Feather:
            s.VisibilityFeatherRange = v.Number;
            break;
        case AlienLayer:
            s.ShowAlienActorLayer = v.Boolean;
            break;
        case FoundableLayer:
            s.ShowFoundablesLayer = v.Boolean;
            break;
        case LootInBases:
            s.ShowLootInBases = v.Boolean;
            break;
        case FoundableScale:
            s.FoundableMarkerScale = v.Number;
            break;
        case POILayer:
            s.ShowPOILayer = v.Boolean;
            break;
        case POIScale:
            s.POIMarkerScale = v.Number;
            break;
        }
    }
    S::EditDecision OnChanged(void*, S::SettingName name, std::string_view newValue)
    {
        Keybind owned{};
        const bool copied = newValue.data() && newValue.size() < owned.size();
        if (copied) std::memcpy(owned.data(), newValue.data(), newValue.size()); // Copy borrowed callback memory first.
        if (!name.section.data() || !name.key.data())
            return S::EditDecision::Ignored;
        for (int field = 0; field < FieldCount; ++field)
        {
            const auto& entry = kEntries[field];
            if (!Equal(name.section, entry.section) || !Equal(name.key, entry.key))
                continue;
            Parsed parsed{};
            if (!copied || !Parse(field, owned, parsed))
            {
                LOG_WARN(
                    "MiniMap: rejected config [%s].%s='%s'; expected %s (maximum 127 bytes); keeping runtime value",
                    entry.section.data(), entry.key.data(), copied ? owned.data() : "<null/overlong>", ValidationRule(field));
                return S::EditDecision::Rejected;
            }
            MiniMapConfig::MouseWheelSettings wheel{};
            {
                std::lock_guard lock(g_settingsMutex);
                if (!g_acceptChanges)
                    return S::EditDecision::Rejected;
                Apply(g_settings, field, parsed); // CURRENT state, never replace from a stale copy.
                if (field == WheelEnabled || field == WheelKey)
                    wheel = {g_settings.ZoomWithMouseWheel, g_settings.WheelModifier, g_settings.WheelRevision};
            }
#if defined(MODLOADER_CLIENT_BUILD)
            if (field == WheelEnabled || field == WheelKey)
                MiniMapMouseWheel::Configure(wheel.Enabled, wheel.Modifier, wheel.Revision);
            if (field == Toggle || field == ZoomIn || field == ZoomOut)
                MiniMapUI::RefreshKeybinds();
            if (field == Follow)
                MiniMapTerrain::InvalidatePresentation();
#endif
            return S::EditDecision::Accepted;
        }
        // Loader-owned Blocking companion fields are not MiniMap preferences.
        return S::EditDecision::Ignored;
    }
} // namespace
namespace MiniMapConfig
{
    Settings Get()
    {
        std::lock_guard lock(g_settingsMutex);
        return g_settings;
    }
    bool VisibleAtStartup()
    {
        std::lock_guard lock(g_settingsMutex);
        return g_settings.VisibleAtStartup;
    }
    bool ClearCache()
    {
        std::lock_guard lock(g_settingsMutex);
        return g_settings.ClearCache;
    }
    double InitialZoom()
    {
        std::lock_guard lock(g_settingsMutex);
        return g_settings.InitialZoom;
    }
    double ZoomStepFactor()
    {
        std::lock_guard lock(g_settingsMutex);
        return g_settings.ZoomStepFactor;
    }
    TrackingSettings GetTrackingSettings()
    {
        std::lock_guard lock(g_settingsMutex);
        return {g_settings.FollowBuildDrone, g_settings.TrackingRevision};
    }
    RenderSettings GetRenderSettings()
    {
        std::lock_guard lock(g_settingsMutex);
        return {g_settings.RotatingMiniMap, g_settings.ShowCoordinates, g_settings.CoordsInUEUnits, g_settings.ShowCompass,
                g_settings.TrackingRevision,
                {g_settings.ShowBuildingLayer, g_settings.RenderDroneRails},
                {g_settings.DisableFogOfWar, g_settings.VisibilityFeatherRange},
                g_settings.ShowAlienActorLayer,
                {g_settings.ShowFoundablesLayer, g_settings.ShowLootInBases,
                 static_cast<float>(g_settings.FoundableMarkerScale)},
                {g_settings.ShowPOILayer, static_cast<float>(g_settings.POIMarkerScale)}};
    }
    FogOfWarSettings GetFogOfWarSettings()
    {
        std::lock_guard lock(g_settingsMutex);
        return {g_settings.DisableFogOfWar, g_settings.VisibilityFeatherRange};
    }
    MouseWheelSettings GetMouseWheelSettings()
    {
        std::lock_guard lock(g_settingsMutex);
        return {g_settings.ZoomWithMouseWheel, g_settings.WheelModifier, g_settings.WheelRevision};
    }
    bool Initialize()
    {
        if (g_listening)
            return true;
        const auto* runtime = S::TryRuntime();
        auto* config = runtime ? runtime->config : nullptr;
        if (!config)
        {
            LOG_ERROR("MiniMap: configuration interface unavailable");
            return false;
        }
        // InitializeFromSchema replaces explicit empty required bindings with
        // defaults. Preserve validation evidence through public API reads first;
        // otherwise an invalid empty value would silently register F4/LeftControl.
        // Missing keys still return their normal schema defaults. Nothing is
        // published and no consumers are registered by this preflight.
        auto toggleBeforeSchema = config->ReadRaw({"General", "ToggleMiniMap"}, kEntries[Toggle].defaultText);
        auto wheelBeforeSchema = config->ReadRaw({"Zoom", "MouseWheelKey"}, kEntries[WheelKey].defaultText);
        const auto schema = config->PublishSchema(kEntries);
        if (schema.error != S::Error::None)
        {
            if (schema.error == S::Error::Unavailable && !schema.detail.empty())
                LOG_ERROR("%s", schema.detail.c_str());
            return false;
        }
        if (config->ValidateStoredSchema().error != S::Error::None)
            return false;
        Settings initial;
        for (int field = 0; field < FieldCount; ++field)
        {
            const auto& entry = kEntries[field];
            // The adapter retains the former raw-read capacity; overlong values
            // cannot become accepted truncated prefixes at the service boundary.
            const auto result = field == Toggle ? std::move(toggleBeforeSchema) :
                field == WheelKey ? std::move(wheelBeforeSchema) :
                config->ReadRaw({entry.section, entry.key}, entry.defaultText);
            const char* raw = result.value ? result.value->c_str() : "";
            Keybind owned{};
            Parsed parsed{};
            if (result.value && Copy(raw, owned) && Parse(field, owned, parsed))
                Apply(initial, field, parsed);
            else
            {
                LOG_WARN(
                    "MiniMap: invalid startup config [%s].%s='%.127s'; expected %s (maximum 127 bytes); using scalar "
                    "default or leaving action/activation unbound",
                    entry.section.data(), entry.key.data(), raw, ValidationRule(field));
                if (field == Toggle)
                    initial.ToggleMiniMap = {};
                if (field == WheelKey)
                {
                    initial.MouseWheelKey = {};
                    initial.WheelModifier = 0;
                }
            }
        }
        {
            std::lock_guard lock(g_settingsMutex);
            // Keep identities monotonic across an initialize/shutdown cycle too.
            initial.WheelRevision = g_settings.WheelRevision + 1;
            initial.TrackingRevision = g_settings.TrackingRevision + 1;
            g_settings = initial;
            g_acceptChanges = true;
        }
#if defined(MODLOADER_CLIENT_BUILD)
        const auto receipt = config->Observe(&OnChanged, nullptr);
        g_observation = receipt.token;
        g_listening = g_observation.id != 0;
        if (receipt.acceptance == S::Acceptance::Rejected || receipt.acceptance == S::Acceptance::BackendUnknown)
        {
            Shutdown();
            return false;
        }
#endif
        return true;
    }
    void Shutdown()
    {
        {
            std::lock_guard lock(g_settingsMutex);
            g_acceptChanges = false;
        }
        if (!g_listening)
            return;
        const auto* runtime = S::TryRuntime();
        if (runtime && runtime->config)
            runtime->config->Remove(g_observation);
        g_observation = {};
        g_listening = false;
    }
} // namespace MiniMapConfig
