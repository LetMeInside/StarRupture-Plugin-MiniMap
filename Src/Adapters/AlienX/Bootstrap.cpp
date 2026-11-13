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

#include <cstddef>
#include <cstdint>
#include <optional>

#if defined(MODLOADER_CLIENT_BUILD)
#include "SDK.hpp"
#include "SDK/AuItems_classes.hpp"
#include "SDK/AuItems_structs.hpp"
#include "SDK/Chimera_structs.hpp"
#include "SDK/MassEntity_structs.hpp"
#include "plugin_map.h"
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

#if defined(MODLOADER_CLIENT_BUILD)
    // Bridges the scanner-session field checks to PluginInit without retaining scanner metadata.
    bool g_buildingMassFieldsResolved = false;

    /** Resolves required Client Mass field layouts during the active scanner session. */
    class ClientBuildingMassFieldContract
    {
        /** Requires exact reflected offsets and extents for fields read by Foundables. */
        static bool ResolveFoundablesFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FCrInventoryFragment::InputInventoryContainer", "FCrInventoryFragment",
                    "InputInventoryContainer", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FCrItemsStorageContainer),
                    static_cast<int32_t>(offsetof(SDK::FCrInventoryFragment, InputInventoryContainer)) },
                { "FCrInventoryFragment::MainInventoryContainer", "FCrInventoryFragment",
                    "MainInventoryContainer", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FCrItemsStorageContainer),
                    static_cast<int32_t>(offsetof(SDK::FCrInventoryFragment, MainInventoryContainer)) },
                { "FCrItemsStorageContainer::Items", "FCrItemsStorageContainer", "Items",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FCrItemsStorageContainer::Items)),
                    static_cast<int32_t>(offsetof(SDK::FCrItemsStorageContainer, Items)) },
                { "FCrStorageItem::Item", "FCrStorageItem", "Item", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FAuSimpleItem),
                    static_cast<int32_t>(offsetof(SDK::FCrStorageItem, Item)) },
                { "FAuSimpleItem::ItemId", "FAuSimpleItem", "ItemId", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FAuItemId),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, ItemId)) },
                { "FAuSimpleItem::ItemDataBase", "FAuSimpleItem", "ItemDataBase", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::UAuItemDataBase*),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, ItemDataBase)) },
                { "FAuSimpleItem::Count", "FAuSimpleItem", "Count", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FAuSimpleItem::Count)),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, Count)) },
                { "FAuItemId::Handle", "FAuItemId", "Handle", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FGuid), static_cast<int32_t>(offsetof(SDK::FAuItemId, Handle)) },
                { "UAuItemDataBase::UIItemType", "UAuItemDataBase", "UIItemType", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::EUIItemType),
                    static_cast<int32_t>(offsetof(SDK::UAuItemDataBase, UIItemType)) },
                { "FCrMassFoundableParameters::FoundableType", "FCrMassFoundableParameters",
                    "FoundableType", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrMassFoundableParameters::FoundableType)),
                    static_cast<int32_t>(offsetof(SDK::FCrMassFoundableParameters, FoundableType)) }
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            return allResolved;
        }

    public:
        /** Requires exact compiled offsets and extents through the active loader scanner session. */
        static bool Resolve(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FMassEntityHandle::Index", "FMassEntityHandle", "Index", PLUGIN_FIELD_CHECK,
                    sizeof(int32_t), static_cast<int32_t>(offsetof(SDK::FMassEntityHandle, Index)) },
                { "FMassEntityHandle::SerialNumber", "FMassEntityHandle", "SerialNumber", PLUGIN_FIELD_CHECK,
                    sizeof(int32_t), static_cast<int32_t>(offsetof(SDK::FMassEntityHandle, SerialNumber)) },
                { "FTransformFragment::Transform", "FTransformFragment", "Transform", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FTransform), static_cast<int32_t>(offsetof(SDK::FTransformFragment, Transform)) },
                { "FTransform::Rotation", "FTransform", "Rotation", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FTransform::Rotation)),
                    static_cast<int32_t>(offsetof(SDK::FTransform, Rotation)) },
                { "FTransform::Translation", "FTransform", "Translation", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FTransform::Translation)),
                    static_cast<int32_t>(offsetof(SDK::FTransform, Translation)) },
                { "FTransform::Scale3D", "FTransform", "Scale3D", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FTransform::Scale3D)),
                    static_cast<int32_t>(offsetof(SDK::FTransform, Scale3D)) },
                { "FMassEnemyStateFragment::State", "FMassEnemyStateFragment", "State",
                    PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
                    sizeof(SDK::EMassEnemyInitializationState), 0x01 },
                { "FAuAPMassFragment::Payload", "FAuAPMassFragment", "Payload", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FAuAPPayloadDataHandle),
                    static_cast<int32_t>(offsetof(SDK::FAuAPMassFragment, Payload)) },
                { "FAuSplineConnectionFragment::Data", "FAuSplineConnectionFragment", "Data",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuSplineConnectionFragment::Data)),
                    static_cast<int32_t>(offsetof(SDK::FAuSplineConnectionFragment, Data)) },
                { "FAuSplineConnectionFragment::SplineCurves", "FAuSplineConnectionFragment", "SplineCurves",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuSplineConnectionFragment::SplineCurves)),
                    static_cast<int32_t>(offsetof(SDK::FAuSplineConnectionFragment, SplineCurves)) },
                { "FAuSplineConnectionFragment::bSplineCurvesInitialized", "FAuSplineConnectionFragment",
                    "bSplineCurvesInitialized", PLUGIN_FIELD_CHECK, sizeof(bool),
                    static_cast<int32_t>(offsetof(SDK::FAuSplineConnectionFragment, bSplineCurvesInitialized)) },
                { "FCrBuildingParameters::PlacementData", "FCrBuildingParameters", "PlacementData",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FCrBuildingParameters::PlacementData)),
                    static_cast<int32_t>(offsetof(SDK::FCrBuildingParameters, PlacementData)) }
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            const bool foundablesResolved = ResolveFoundablesFields(scanner, self);
            return allResolved && foundablesResolved;
        }
    };

    /** Compares resident Client script-struct size and alignment with the compiled Mass SDK layouts. */
    class ClientBuildingMassTypeContract
    {
        /** Requires a reflected script struct to match the compiled Client SDK size and alignment. */
        static bool CheckType(
            const IPluginSelf* self,
            const char* objectPath,
            const char* displayName,
            std::size_t expectedSize,
            std::size_t expectedAlignment)
        {
            const SDK::UScriptStruct* type = SDK::UObject::FindObject<SDK::UScriptStruct>(
                objectPath, SDK::EClassCastFlags::ScriptStruct);
            if (type == nullptr || type->Size != static_cast<std::int32_t>(expectedSize) ||
                type->MinAlignment != static_cast<std::int32_t>(expectedAlignment))
            {
                if (self && self->logger && self->logger->Error)
                {
                    self->logger->Error(self,
                        "MiniMap: Client admission refused: %s SDK size=0x%X align=0x%X, "
                        "running size=0x%X align=0x%X",
                        displayName, static_cast<unsigned>(expectedSize),
                        static_cast<unsigned>(expectedAlignment),
                        type ? static_cast<unsigned>(type->Size) : 0,
                        type ? static_cast<unsigned>(type->MinAlignment) : 0);
                }
                return false;
            }
            return true;
        }

    public:
        /** Requires each consumed Mass fragment and typed subobject to match resident Client metadata. */
        static bool IsCompatible(const IPluginSelf* self)
        {
            try
            {
                return CheckType(self, "ScriptStruct MassEntity.MassEntityHandle", "FMassEntityHandle",
                        sizeof(SDK::FMassEntityHandle), alignof(SDK::FMassEntityHandle)) &&
                    CheckType(self, "ScriptStruct MassCommon.TransformFragment", "FTransformFragment",
                        sizeof(SDK::FTransformFragment), alignof(SDK::FTransformFragment)) &&
                    CheckType(self, "ScriptStruct CoreUObject.Transform", "FTransform",
                        sizeof(SDK::FTransform), alignof(SDK::FTransform)) &&
                    CheckType(self,
                        "ScriptStruct MassAIPrototypeEnemyRuntime.MassEnemyStateFragment",
                        "FMassEnemyStateFragment", sizeof(SDK::FMassEnemyStateFragment),
                        alignof(SDK::FMassEnemyStateFragment)) &&
                    CheckType(self, "ScriptStruct AuActorPlacement.AuAPMassFragment", "FAuAPMassFragment",
                        sizeof(SDK::FAuAPMassFragment), alignof(SDK::FAuAPMassFragment)) &&
                    CheckType(self, "ScriptStruct AuActorPlacement.AuAPPayloadDataHandle",
                        "FAuAPPayloadDataHandle", sizeof(SDK::FAuAPPayloadDataHandle),
                        alignof(SDK::FAuAPPayloadDataHandle)) &&
                    CheckType(self, "ScriptStruct AuActorPlacement.AuSplineConnectionFragment",
                        "FAuSplineConnectionFragment", sizeof(SDK::FAuSplineConnectionFragment),
                        alignof(SDK::FAuSplineConnectionFragment)) &&
                    CheckType(self, "ScriptStruct Chimera.CrBuildingParameters", "FCrBuildingParameters",
                        sizeof(SDK::FCrBuildingParameters), alignof(SDK::FCrBuildingParameters)) &&
                    CheckType(self, "ScriptStruct Chimera.CrInventoryFragment", "FCrInventoryFragment",
                        sizeof(SDK::FCrInventoryFragment), alignof(SDK::FCrInventoryFragment)) &&
                    CheckType(self, "ScriptStruct Chimera.CrItemsStorageContainer", "FCrItemsStorageContainer",
                        sizeof(SDK::FCrItemsStorageContainer), alignof(SDK::FCrItemsStorageContainer)) &&
                    CheckType(self, "ScriptStruct Chimera.CrStorageItem", "FCrStorageItem",
                        sizeof(SDK::FCrStorageItem), alignof(SDK::FCrStorageItem)) &&
                    CheckType(self, "ScriptStruct AuItems.AuSimpleItem", "FAuSimpleItem",
                        sizeof(SDK::FAuSimpleItem), alignof(SDK::FAuSimpleItem)) &&
                    CheckType(self, "ScriptStruct AuItems.AuItemId", "FAuItemId",
                        sizeof(SDK::FAuItemId), alignof(SDK::FAuItemId)) &&
                    CheckType(self, "ScriptStruct CoreUObject.Guid", "FGuid",
                        sizeof(SDK::FGuid), alignof(SDK::FGuid)) &&
                    CheckType(self, "ScriptStruct Chimera.CrMassFoundableParameters",
                        "FCrMassFoundableParameters", sizeof(SDK::FCrMassFoundableParameters),
                        alignof(SDK::FCrMassFoundableParameters));
            }
            catch (...)
            {
                if (self && self->logger && self->logger->Error)
                    self->logger->Error(self,
                        "MiniMap: Client admission refused: required Mass type metadata lookup failed");
                return false;
            }
        }
    };

    /** Checks the reflected query layout before a Client subsystem can construct fixed native query storage. */
    class ClientMassQueryStorageContract
    {
    public:
        /** Requires the running MassEntity struct to match the SDK size and alignment used by all query owners. */
        static bool IsCompatible(const IPluginSelf* self)
        {
            try
            {
                const SDK::UScriptStruct* queryType = SDK::UObject::FindObject<SDK::UScriptStruct>(
                    "ScriptStruct MassEntity.MassEntityQuery", SDK::EClassCastFlags::ScriptStruct);
                if (queryType == nullptr)
                {
                    if (self && self->logger && self->logger->Error)
                        self->logger->Error(self,
                            "MiniMap: Client admission refused: MassEntity.MassEntityQuery is unavailable");
                    return false;
                }

                const unsigned sdkSize = static_cast<unsigned>(sizeof(SDK::FMassEntityQuery));
                const unsigned sdkAlignment = static_cast<unsigned>(alignof(SDK::FMassEntityQuery));
                if (queryType->Size != static_cast<std::int32_t>(sdkSize) ||
                    queryType->MinAlignment != static_cast<std::int32_t>(sdkAlignment))
                {
                    if (self && self->logger && self->logger->Error)
                        self->logger->Error(self,
                            "MiniMap: Client admission refused: FMassEntityQuery SDK size=0x%X align=0x%X, "
                            "running size=0x%X align=0x%X",
                            sdkSize, sdkAlignment, static_cast<unsigned>(queryType->Size),
                            static_cast<unsigned>(queryType->MinAlignment));
                    return false;
                }

                return true;
            }
            catch (...)
            {
                if (self && self->logger && self->logger->Error)
                    self->logger->Error(self,
                        "MiniMap: Client admission refused: FMassEntityQuery metadata lookup failed");
                return false;
            }
        }
    };
#endif

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
        MINIMAP_EXPORTS_TARGET,
        PLUGIN_GAME_BUILT_CL,
        PluginLoadSetting(PluginReload::MENU) |
            PluginUnloadSetting(PluginReload::MENU)
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
        g_buildingMassFieldsResolved = false;
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
#if defined(MODLOADER_CLIENT_BUILD)
    g_buildingMassFieldsResolved = false;
#endif
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
        if (requiredScanner.HasFailures())
        {
            MiniMapNative::Shutdown();
            return;
        }

        PluginMap::MassEntityManagerField.offset = -1;
        PluginMap::MassEntityManagerField.resolvedSize = 0;
        if (!PluginMap::ResolveMassEntityManager(scanner, self))
        {
            MiniMapNative::Shutdown();
            return;
        }

        const int32_t managerOffset = PluginMap::MassEntityManagerField.offset;
        const uint32_t managerSize = PluginMap::MassEntityManagerField.resolvedSize;
        if (managerOffset != 0x38 || managerSize != 0x10)
        {
            requiredScanner.Fail(
                "MiniMap::MassEntityManagerField",
                "resolved EntityManager field does not match CL-127004 offset 0x38 and size 0x10");
            MiniMapNative::Shutdown();
            return;
        }

        if (!MiniMapNative::PublishMassEntityManagerField(managerOffset, managerSize))
        {
            requiredScanner.Fail(
                "MiniMap::MassEntityManagerField",
                "validated EntityManager field could not be published by NativeApi");
            MiniMapNative::Shutdown();
            return;
        }

        if (!ClientBuildingMassFieldContract::Resolve(scanner, self))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_buildingMassFieldsResolved = true;
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
#if defined(MODLOADER_CLIENT_BUILD)
    if (!PluginSdkRuntime::Init(self ? self->hooks : nullptr))
    {
        MiniMapNative::Shutdown();
        if (self && self->logger && self->logger->Log)
            self->logger->Log(PluginLogLevel::Error, self,
                "MiniMap: SDK runtime initialization failed; loader game globals are unavailable");
        return false;
    }
    if (!ClientMassQueryStorageContract::IsCompatible(self))
    {
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_buildingMassFieldsResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: required Mass fields were not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (MiniMapNative::Get() == nullptr)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: native Mass EntityManager field was not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!ClientBuildingMassTypeContract::IsCompatible(self))
    {
        MiniMapNative::Shutdown();
        return false;
    }
#endif
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
