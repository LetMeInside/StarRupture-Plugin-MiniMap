#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingInventory.h"
#include "BuildingCollector.h"
#include "Map.h"
#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"
#include "SDK/Chimera_classes.hpp"
#include "SDK/MassRepresentation_structs.hpp"
#include "SDK/MassReplication_structs.hpp"
#include "SDK/MassSpawner_classes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    using Record = MiniMapBuildingInventory::Record;
    using Clock = std::chrono::steady_clock;
    using Inventory = std::unordered_map<uint64_t, Record>;
    constexpr auto kPollInterval = std::chrono::seconds(1);
    constexpr auto kDiagnosticInterval = std::chrono::seconds(5);
    constexpr std::size_t kQuerySize = 0x350; // Same native query ABI as Foundables.
    constexpr std::ptrdiff_t kManagerOffset = 0x38;
    constexpr int32_t kMaximumReasonableHandleCount = 1000000;
    constexpr std::size_t kEventSamples = 4;
    constexpr std::size_t kTypeSamples = 8;
    constexpr auto kGridValidationInterval = std::chrono::seconds(10);
    // Rotation-independent 400 x 400 m XY test region. This covers the default
    // viewport at every zoom with padding; it is not a rendering range limit.
    constexpr double kGridTestHalfExtent = 20000.0; // centimeters

    struct Types
    {
        SDK::UScriptStruct* BuildingTag = nullptr;
        SDK::UScriptStruct* Transform = nullptr;
        SDK::UScriptStruct* Parameters = nullptr;
        SDK::UScriptStruct* Representation = nullptr;
        SDK::UScriptStruct* NetworkId = nullptr;
        SDK::UScriptStruct* Placement = nullptr;
        SDK::UScriptStruct* Spline = nullptr;
        SDK::UScriptStruct* Tint = nullptr;
        SDK::UClass* PlacementClass = nullptr;
        SDK::UClass* BuildingClass = nullptr;
        SDK::UScriptStruct* GridTag = nullptr;
        SDK::UClass* GridClass = nullptr;
        SDK::UClass* ConfigClass = nullptr;
        SDK::UClass* GridTraitClass = nullptr;
        SDK::UEnum* BuildingIds = nullptr;
        SDK::UObject* PathLibrary = nullptr;
        SDK::UObject* NameLibrary = nullptr;
        void* PathFunction = nullptr;
        void* NameFunction = nullptr;
    };

    IPluginSelf* g_inventorySelf = nullptr;
    bool g_initialized = false;
    bool g_ready = false;
    bool g_constructed = false;
    bool g_typesResolved = false;
    bool g_typesAttempted = false;
    bool g_haveInventory = false;
    uint64_t g_generation = 0;
    SDK::UWorld* g_world = nullptr;
    SDK::UMassEntitySubsystem* g_subsystem = nullptr;
    const void* g_manager = nullptr;
    alignas(0x10) std::array<std::byte, kQuerySize> g_query = {};
    alignas(0x10) std::array<std::byte, kQuerySize> g_offGridQuery = {};
    bool g_offGridConstructed = false;
    // Copied diagnostic text only. Pages limit output to 32 definitions/10s.
    std::vector<std::string> g_censusRows;
    std::vector<std::string> g_geometryRows;
    std::size_t g_censusPage = 0;
    uint64_t g_censusEpoch = 0;
    Types g_types;
    std::mutex g_mutex;
    Inventory g_inventory;
    std::unordered_set<uint64_t> g_loggedTypes;
    std::unordered_set<uint64_t> g_loggedGaps;
    Clock::time_point g_nextPoll = {};
    Clock::time_point g_nextDiagnostic = {};
    Clock::time_point g_nextWarning = {};
    Clock::time_point g_nextGridValidation = {};
    double g_maxMilliseconds = 0.0;
    std::size_t g_intervalAdded = 0;
    std::size_t g_intervalRemoved = 0;

    uint64_t Key(int32_t index, int32_t serial)
    {
        return (uint64_t(static_cast<uint32_t>(serial)) << 32) |
            static_cast<uint32_t>(index);
    }

    const char* TypeName(uint8_t type)
    {
        static constexpr const char* names[] = { "Research", "Habitat", "Survival",
            "Player", "Power", "Extraction", "RawMaterialProcessing", "Crafting",
            "Transport", "Defensive", "CustomBuilding", "TemperatureManagement", "All", "Test" };
        return type < std::size(names) ? names[type] : "unresolved";
    }

    const char* RepresentationName(uint8_t type)
    {
        switch (static_cast<SDK::EMassRepresentationType>(type))
        {
        case SDK::EMassRepresentationType::HighResSpawnedActor: return "actor-high";
        case SDK::EMassRepresentationType::LowResSpawnedActor: return "actor-low";
        case SDK::EMassRepresentationType::StaticMeshInstance: return "ISM";
        case SDK::EMassRepresentationType::None: return "none";
        default: return "unresolved";
        }
    }

    void Warn(const char* reason)
    {
        const auto now = Clock::now();
        if (now >= g_nextWarning)
        {
            LOG_WARN("MiniMap: BuildingInventory: %s; collection not published", reason);
            g_nextWarning = now + kDiagnosticInterval;
        }
    }

    void DestroyQuery()
    {
        if (g_offGridConstructed)
        {
            if (const auto* native = MiniMapNative::Get()) native->mass.queryDestruct(g_offGridQuery.data());
        }
        g_offGridConstructed = false;
        g_offGridQuery.fill(std::byte{ 0 });
        if (g_constructed)
        {
            const auto* native = MiniMapNative::Get();
            if (native != nullptr) native->mass.queryDestruct(g_query.data());
            LOG_DEBUG("MiniMap: BuildingInventory: query destroyed");
        }
        g_constructed = false;
        g_query.fill(std::byte{ 0 });
        g_world = nullptr;
        g_subsystem = nullptr;
        g_manager = nullptr;
    }

    void ClearInventory(const char* reason)
    {
        std::size_t previous = 0;
        {
            std::scoped_lock lock(g_mutex);
            previous = g_inventory.size();
            g_inventory.clear();
            ++g_generation;
        }
        g_haveInventory = false;
        g_loggedTypes.clear();
        g_loggedGaps.clear();
        g_maxMilliseconds = 0.0;
        g_intervalAdded = g_intervalRemoved = 0;
        g_nextDiagnostic = {};
        g_nextWarning = {};
        g_nextGridValidation = {};
        g_censusRows.clear();
        g_geometryRows.clear();
        g_censusPage = 0;
        g_censusEpoch = 0;
        LOG_INFO("MiniMap: BuildingInventory: reset reason=%s generation=%llu cleared=%zu",
            reason, static_cast<unsigned long long>(g_generation), previous);
    }

    SDK::UObject* FindMetadata(const char* name, const char* className)
    {
        // Metadata lookup only, once per world. This never enumerates buildings
        // or uses GObjects as a population source. Reject ambiguous names.
        PluginObjectInfo info = {};
        const auto* walker = g_inventorySelf->hooks->ObjectWalker;
        if (walker->FindObjectsByNameInto(name, PluginObjectLookup_Both, &info, 1) != 1 ||
            std::strcmp(info.className, className) != 0)
        {
            LOG_WARN("MiniMap: BuildingInventory: metadata unavailable/ambiguous: %s", name);
            return nullptr;
        }
        return static_cast<SDK::UObject*>(info.object);
    }

    SDK::UScriptStruct* FindStruct(const char* name, std::size_t size)
    {
        auto* type = static_cast<SDK::UScriptStruct*>(FindMetadata(name, "ScriptStruct"));
        if (type != nullptr && type->Size != static_cast<int32_t>(size))
        {
            LOG_WARN("MiniMap: BuildingInventory: layout mismatch %s native=%d expected=%zu",
                name, type->Size, size);
            return nullptr;
        }
        return type;
    }

    SDK::UClass* FindClass(const char* name, std::size_t size)
    {
        auto* type = static_cast<SDK::UClass*>(FindMetadata(name, "Class"));
        if (type != nullptr && type->Size != static_cast<int32_t>(size))
        {
            LOG_WARN("MiniMap: BuildingInventory: class layout mismatch %s native=%d expected=%zu",
                name, type->Size, size);
            return nullptr;
        }
        return type;
    }

    bool IsDiagnosticStringFunction(const void* function)
    {
        if (!function) return false;
        // Matching PDB UFunction: NumParms+B4, ParmsSize+B6, ReturnValueOffset+B8.
        const auto* bytes = static_cast<const uint8_t*>(function);
        uint16_t size = 0, resultOffset = 0;
        std::memcpy(&size, bytes + 0xB6, sizeof(size));
        std::memcpy(&resultOffset, bytes + 0xB8, sizeof(resultOffset));
        return bytes[0xB4] == 2 && size == 0x18 && resultOffset == 8;
    }

    bool ResolveTypes()
    {
        if (g_typesAttempted) return g_typesResolved;
        if (!g_inventorySelf->hooks->ObjectWalker->IsReady()) return false;
        g_typesAttempted = true;
        // Optional metadata may be unavailable without preventing existence
        // collection. Missing fields remain explicitly unresolved in records.
        g_types.BuildingTag = FindStruct("CrMassBuildingTag", sizeof(SDK::FCrMassBuildingTag));
        g_types.Transform = FindStruct("TransformFragment", sizeof(SDK::FTransform));
        g_types.Parameters = FindStruct("CrBuildingParameters", sizeof(SDK::FCrBuildingParameters));
        g_types.Representation = FindStruct("MassRepresentationFragment", sizeof(SDK::FMassRepresentationFragment));
        g_types.NetworkId = FindStruct("MassNetworkIDFragment", sizeof(SDK::FMassNetworkIDFragment));
        g_types.Placement = FindStruct("AuAPMassFragment", sizeof(SDK::FAuAPMassFragment));
        g_types.Spline = FindStruct("AuSplineConnectionFragment", sizeof(SDK::FAuSplineConnectionFragment));
        g_types.Tint = FindStruct("CrBuildingVisualCustomizationFragment", sizeof(SDK::FCrBuildingVisualCustomizationFragment));
        g_types.PlacementClass = FindClass("AuActorPlacementData", sizeof(SDK::UAuActorPlacementData));
        g_types.BuildingClass = FindClass("CrBuildingData", sizeof(SDK::UCrBuildingData));
        g_types.GridTag = FindStruct("CrMassEntityGridTag", sizeof(SDK::FCrMassEntityGridTag));
        g_types.GridClass = FindClass("CrEntityGridSubsystem", sizeof(SDK::UCrEntityGridSubsystem));
        g_types.ConfigClass = FindClass("MassEntityConfigAsset", sizeof(SDK::UMassEntityConfigAsset));
        g_types.GridTraitClass = FindClass("CrMassEntityGridTrait", sizeof(SDK::UCrMassEntityGridTrait));
        g_types.BuildingIds = static_cast<SDK::UEnum*>(FindMetadata("ECrBuildingID", "Enum"));
        const auto* walker = g_inventorySelf->hooks->ObjectWalker;
        if (walker->ResolveUFunction && walker->InvokeResolvedUFunction)
        {
            g_types.PathLibrary = FindMetadata("Default__KismetSystemLibrary", "KismetSystemLibrary");
            g_types.NameLibrary = FindMetadata("Default__KismetStringLibrary", "KismetStringLibrary");
            g_types.PathFunction = walker->ResolveUFunction("KismetSystemLibrary", "GetPathName");
            g_types.NameFunction = walker->ResolveUFunction("KismetStringLibrary", "Conv_NameToString");
            // Both generated parameter blocks are input(8) + returned FString(16).
            if (!IsDiagnosticStringFunction(g_types.PathFunction)) g_types.PathFunction = nullptr;
            if (!IsDiagnosticStringFunction(g_types.NameFunction)) g_types.NameFunction = nullptr;
        }
        g_typesResolved = g_types.BuildingTag != nullptr && g_types.Transform != nullptr;
        return g_typesResolved;
    }

    bool IsClass(const SDK::UObject* object, const SDK::UClass* base)
    {
        if (object == nullptr || base == nullptr) return false;
        // No StaticClass/GetName calls or asset loading. Walk copied class links
        // only while on the game thread; no object pointer enters the snapshot.
        const SDK::UStruct* type = object->Class;
        for (unsigned depth = 0; type != nullptr && depth < 64; ++depth, type = type->SuperStruct)
            if (type == base) return true;
        return false;
    }

    bool EnsureQuery(const MiniMapNative::NativeApi& native)
    {
        auto* world = MiniMapMap::GetWorld();
        auto* subsystem = world != nullptr ? native.mass.getMassEntitySubsystem(world) : nullptr;
        const void* manager = nullptr;
        if (subsystem != nullptr)
            std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + kManagerOffset, sizeof(manager));
        if (g_constructed && world == g_world && subsystem == g_subsystem && manager == g_manager)
            return true;
        if (g_constructed)
        {
            DestroyQuery();
            ClearInventory("world/manager changed");
        }
        if (manager == nullptr || !ResolveTypes()) return false;
        native.mass.queryConstruct(g_query.data(), reinterpret_cast<const uint8_t*>(subsystem) + kManagerOffset);
        g_constructed = true;
        native.mass.addTagRequirement(g_query.data(), g_types.BuildingTag, SDK::EMassFragmentPresence::All);
        // Optional so missing transforms are diagnosed without dropping an
        // existing entity out of the binary existence inventory.
        native.mass.addTransformRequirement(g_query.data(),
            static_cast<uint8_t>(SDK::EMassFragmentAccess::ReadOnly),
            static_cast<uint8_t>(SDK::EMassFragmentPresence::Optional));
        g_world = world;
        g_subsystem = subsystem;
        g_manager = manager;
        LOG_INFO("MiniMap: BuildingInventory: query ready world=%p generation=%llu cadence=1s",
            world, static_cast<unsigned long long>(g_generation));
        return true;
    }

    const void* Fragment(const MiniMapNative::NativeApi& native,
        MiniMapNative::MassEntityHandle id, const SDK::UScriptStruct* type)
    {
        return type != nullptr ? native.mass.getFragmentDataPtr(g_manager, id, type) : nullptr;
    }

    const SDK::FCrBuildingParameters* Parameters(const MiniMapNative::NativeApi& native,
        MiniMapNative::MassEntityHandle id)
    {
        if (g_types.Parameters == nullptr) return nullptr;
        const auto* wrapper = native.mass.getConstSharedFragmentPtr(g_manager, id, g_types.Parameters);
        const uint8_t* memory = nullptr;
        if (wrapper != nullptr) std::memcpy(&memory, wrapper, sizeof(memory));
        const int16_t alignment = g_types.Parameters->MinAlignment;
        if (memory == nullptr || alignment <= 0 || (alignment & (alignment - 1)) != 0) return nullptr;
        // FConstSharedStruct layout/alignment is the same as Foundables.
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory + sizeof(void*));
        const uintptr_t aligned = (base + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
        return reinterpret_cast<const SDK::FCrBuildingParameters*>(aligned);
    }

    Record ReadRecord(const MiniMapNative::NativeApi& native, MiniMapNative::MassEntityHandle id)
    {
        Record record;
        record.WorldGeneration = g_generation;
        record.Index = id.Index;
        record.SerialNumber = id.SerialNumber;
        const auto* transform = static_cast<const SDK::FTransform*>(Fragment(native, id, g_types.Transform));
        if (transform != nullptr)
        {
            record.Position = { transform->Translation.X, transform->Translation.Y, transform->Translation.Z };
            record.Rotation = { transform->Rotation.X, transform->Rotation.Y, transform->Rotation.Z, transform->Rotation.W };
            record.Scale = { transform->Scale3D.X, transform->Scale3D.Y, transform->Scale3D.Z };
            const auto finite = [](double value) { return std::isfinite(value); };
            double norm = 0.0;
            for (double value : record.Rotation) norm += value * value;
            record.HasTransform = std::all_of(record.Position.begin(), record.Position.end(), finite) &&
                std::all_of(record.Rotation.begin(), record.Rotation.end(), finite) &&
                std::all_of(record.Scale.begin(), record.Scale.end(), finite) && norm > 0.0 && std::isfinite(norm);
            if (!record.HasTransform)
            {
                record.Position = {};
                record.Rotation = {};
                record.Scale = {};
            }
        }
        const auto* parameters = Parameters(native, id);
        const auto* placement = parameters != nullptr ? parameters->PlacementData : nullptr;
        // An unresolved TObjectPtr handle must not be treated as an address.
        if ((reinterpret_cast<uintptr_t>(placement) & 7) == 0 && IsClass(placement, g_types.PlacementClass))
        {
            record.HasPlacement = true;
            record.BuildingId = static_cast<uint32_t>(placement->BuildingID);
            record.PlacementNameIndex = placement->Name.ComparisonIndex;
            record.PlacementNameNumber = placement->Name.Number;
            if (IsClass(placement, g_types.BuildingClass))
            {
                const auto* building = static_cast<const SDK::UCrBuildingData*>(placement);
                const auto type = static_cast<uint8_t>(building->Type);
                if (type < static_cast<uint8_t>(SDK::ECrBuildingType::MAX)) record.BuildingType = type;
                record.UniqueNameIndex = building->BuldingUniqueName.ComparisonIndex;
                record.UniqueNameNumber = building->BuldingUniqueName.Number;
            }
        }
        const auto* representation = static_cast<const SDK::FMassRepresentationFragment*>(Fragment(native, id, g_types.Representation));
        if (representation != nullptr)
        {
            record.Representation = static_cast<uint8_t>(representation->CurrentRepresentation);
            record.PreviousRepresentation = static_cast<uint8_t>(representation->PrevRepresentation);
            record.HighResTemplate = representation->HighResTemplateActorIndex;
            record.LowResTemplate = representation->LowResTemplateActorIndex;
            std::memcpy(&record.MeshDescriptor, &representation->StaticMeshDescHandle, sizeof(record.MeshDescriptor));
        }
        const void* network = Fragment(native, id, g_types.NetworkId);
        record.HasNetworkIdentity = network != nullptr;
        if (network != nullptr) std::memcpy(&record.NetworkId, network, sizeof(record.NetworkId));
        record.HasPlacementFragment = Fragment(native, id, g_types.Placement) != nullptr;
        const auto* spline = static_cast<const SDK::FAuSplineConnectionFragment*>(Fragment(native, id, g_types.Spline));
        if (spline != nullptr)
        {
            record.HasSpline = true;
            record.SplineCurvesReady = spline->bSplineCurvesInitialized;
            record.SplinePointCount = spline->Data.SplineData.Num();
            record.SplineLength = std::isfinite(spline->Data.SplineRealLength) ? spline->Data.SplineRealLength : 0.0f;
        }
        const auto* tint = static_cast<const SDK::FCrBuildingVisualCustomizationFragment*>(Fragment(native, id, g_types.Tint));
        if (tint != nullptr)
        {
            const auto& color = tint->ColorTint;
            record.ColorTint = { color.R, color.G, color.B, color.A };
            record.HasColorTint = std::all_of(record.ColorTint.begin(), record.ColorTint.end(), [](float v) { return std::isfinite(v); });
            if (!record.HasColorTint) record.ColorTint = {};
        }
        return record;
    }

    void LogRecord(const char* event, const Record& record)
    {
        LOG_INFO("MiniMap: BuildingInventory: %s id=%d:%d type=%s buildingID=%u placement=%d placementFName=%d:%u rep=%s mesh=%u spline=%d netFragment=%d netID=%u transform=%d XYZ=(%.1f,%.1f,%.1f) quat=(%.4f,%.4f,%.4f,%.4f)",
            event, record.Index, record.SerialNumber, TypeName(record.BuildingType), record.BuildingId,
            record.HasPlacement ? 1 : 0, record.PlacementNameIndex, record.PlacementNameNumber, RepresentationName(record.Representation),
            static_cast<unsigned>(record.MeshDescriptor), record.HasSpline ? 1 : 0,
            record.HasNetworkIdentity ? 1 : 0, record.NetworkId, record.HasTransform ? 1 : 0,
            record.Position[0], record.Position[1], record.Position[2],
            record.Rotation[0], record.Rotation[1], record.Rotation[2], record.Rotation[3]);
    }

    // Stage A.3 metadata is resolved only for distinct missing definitions.
    // These reflected calls avoid the generated SDK's fixed FName addresses.
    std::string DiagnosticString(const MiniMapNative::NativeApi& native, const SDK::UObject* object,
        const SDK::FName* name = nullptr)
    {
        struct ParametersBlock
        {
            std::array<std::byte, 8> Input = {};
            wchar_t* Data = nullptr;
            int32_t Num = 0, Max = 0;
        } params;
        static_assert(sizeof(ParametersBlock) == 0x18);
        const auto* walker = g_inventorySelf->hooks->ObjectWalker;
        auto* library = name ? g_types.NameLibrary : g_types.PathLibrary;
        void* function = name ? g_types.NameFunction : g_types.PathFunction;
        if (!library || !function || (!object && !name)) return "unresolved";
        if (name) std::memcpy(params.Input.data(), name, sizeof(*name));
        else std::memcpy(params.Input.data(), &object, sizeof(object));
        const bool invoked = walker->InvokeResolvedUFunction(library, function, &params);
        // FString returned by ProcessEvent owns FMemory storage. The POD block
        // has no SDK destructor; release exactly once through existing native free.
        struct Owner { wchar_t* Data; MiniMapNative::TextureApi::MemoryFreeFn Free;
            ~Owner() { if (Data) Free(Data); } } owner{ params.Data, native.texture.memoryFree };
        if (!invoked || !params.Data || params.Num < 1 || params.Num > 2048 || params.Max < params.Num ||
            params.Data[params.Num - 1] != L'\0') return "unresolved";
        std::string result;
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, params.Data, params.Num - 1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return "unresolved";
        result.resize(bytes);
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, params.Data, params.Num - 1, result.data(), bytes, nullptr, nullptr);
        for (char& ch : result) if (static_cast<unsigned char>(ch) < 32 || ch == '"') ch = '_';
        return result;
    }

    std::string BuildingIdentifier(const MiniMapNative::NativeApi& native, uint32_t id)
    {
        if (g_types.BuildingIds)
        {
            const auto& names = g_types.BuildingIds->Names;
            if (names.Num() > 0 && names.Num() <= 4096 && names.Max() >= names.Num() && names.GetDataPtr())
                for (const auto& entry : names)
                    if (entry.Second == id) return DiagnosticString(native, nullptr, &entry.First);
        }
        return "unresolved";
    }

    struct DefinitionCensus
    {
        std::size_t Count = 0;
        uint32_t Id = 0;
        uint8_t Type = 0xFF;
        bool Spline = false;
        int32_t NameIndex = 0;
        uint32_t NameNumber = 0;
        std::string Identifier, Placement, Config, GridTrait = "unresolved";

        void Resolve(const MiniMapNative::NativeApi& native, const SDK::UAuActorPlacementData* placement)
        {
            Identifier = BuildingIdentifier(native, Id);
            Placement = DiagnosticString(native, placement);
            Config = "unresolved";
            if (!placement || !g_types.ConfigClass || !g_types.GridTraitClass) return;
            // Inspect loaded definitions only. Never load a soft asset merely for diagnostics.
            const auto* config = placement->EntityType.EntityConfigPtr;
            if ((reinterpret_cast<uintptr_t>(config) & 7) != 0 || !IsClass(config, g_types.ConfigClass)) return;
            Config = DiagnosticString(native, config);
            std::array<const SDK::UMassEntityConfigAsset*, 32> visited = {};
            for (std::size_t depth = 0; depth < visited.size(); ++depth)
            {
                if (!config) { GridTrait = "absent-in-loaded-chain"; return; }
                if ((reinterpret_cast<uintptr_t>(config) & 7) != 0 || !IsClass(config, g_types.ConfigClass) ||
                    std::find(visited.begin(), visited.begin() + depth, config) != visited.begin() + depth) return;
                visited[depth] = config;
                const auto& traits = config->Config.Traits;
                if (traits.Num() < 0 || traits.Num() > 256 || traits.Max() < traits.Num() ||
                    (traits.Num() && !traits.GetDataPtr())) return;
                for (const auto* trait : traits)
                {
                    if (trait && (reinterpret_cast<uintptr_t>(trait) & 7) != 0) return;
                    if (IsClass(trait, g_types.GridTraitClass))
                    {
                        GridTrait = depth ? "present-inherited" : "present-direct";
                        return;
                    }
                }
                config = config->Config.Parent;
            }
        }
    };

    // Pure geometry on copied scalar values: cubic Hermite -> Bezier hull.
    // Matching CL-127004 GetLocationAtSplineInputKey returns stored curve values
    // for World(1); Local(0) inverse-transforms them using SplineLocationWS/RotationWS.
    // Do NOT transform these already-world-space points a second time.
    struct XY { double X, Y; };
    using Cubic = std::array<XY, 4>;
    XY Mix(XY a, XY b) { return { (a.X + b.X) * 0.5, (a.Y + b.Y) * 0.5 }; }
    bool Finite(XY p) { return std::isfinite(p.X) && std::isfinite(p.Y); }
    enum class Crossing { Confirmed, BoundsOnly, NonIntersection, Unresolved };
    bool Inside(XY p, const SDK::FBox& box)
    {
        // A witness strictly inside avoids claiming a crossing from rounding at an edge.
        constexpr double epsilon = 0.0001;
        return p.X > box.Min.X + epsilon && p.X < box.Max.X - epsilon &&
            p.Y > box.Min.Y + epsilon && p.Y < box.Max.Y - epsilon;
    }
    Crossing CheckHull(const Cubic& p, const SDK::FBox& box, unsigned depth, unsigned& budget)
    {
        double minX = p[0].X, maxX = p[0].X, minY = p[0].Y, maxY = p[0].Y;
        for (const auto point : p)
        {
            minX = (std::min)(minX, point.X); maxX = (std::max)(maxX, point.X);
            minY = (std::min)(minY, point.Y); maxY = (std::max)(maxY, point.Y);
        }
        constexpr double epsilon = 0.0001;
        if (maxX < box.Min.X - epsilon || minX > box.Max.X + epsilon ||
            maxY < box.Min.Y - epsilon || minY > box.Max.Y + epsilon) return Crossing::NonIntersection;
        if (Inside(p[0], box) || Inside(p[3], box)) return Crossing::Confirmed;
        if (depth == 16 || budget == 0) return Crossing::BoundsOnly;
        --budget;
        const XY a = Mix(p[0], p[1]), b = Mix(p[1], p[2]), c = Mix(p[2], p[3]);
        const XY d = Mix(a, b), e = Mix(b, c), midpoint = Mix(d, e);
        if (Inside(midpoint, box)) return Crossing::Confirmed; // Actual curve point, not hull point.
        const auto left = CheckHull({ p[0], a, d, midpoint }, box, depth + 1, budget);
        if (left == Crossing::Confirmed) return left;
        const auto right = CheckHull({ midpoint, e, c, p[3] }, box, depth + 1, budget);
        if (right == Crossing::Confirmed) return right;
        return left == Crossing::BoundsOnly || right == Crossing::BoundsOnly ? Crossing::BoundsOnly : Crossing::NonIntersection;
    }

    Crossing ValidateSpline(const SDK::FAuSplineConnectionFragment* spline, const SDK::FBox& box)
    {
        if (!spline) return Crossing::Unresolved;
        static_assert(sizeof(SDK::FAuSplineConnectionFragment) == 0xE0);
        static_assert(offsetof(SDK::FAuSplineConnectionFragment, SplineCurves) == 0x70);
        static_assert(sizeof(SDK::FInterpCurvePointVector) == 0x58);
        static_assert(offsetof(SDK::FInterpCurvePointVector, LeaveTangent) == 0x38);
        static_assert(sizeof(SDK::FAuAPSplinePointData) == 0x40);
        static_assert(sizeof(SDK::FAuAPSplineData) == 0x50);
        static_assert(sizeof(SDK::FSplineCurves) == 0x68);
        const auto& curve = spline->SplineCurves.Position;
        std::array<SDK::FInterpCurvePointVector, 2> raw = {};
        const SDK::FInterpCurvePointVector* points = nullptr;
        int count = 0;
        if (spline->bSplineCurvesInitialized)
        {
            count = curve.Points.Num();
            if (curve.bIsLooped || count < 2 || count > 256 || curve.Points.Max() < count || !curve.Points.GetDataPtr())
                return Crossing::Unresolved;
            points = curve.Points.GetDataPtr();
        }
        else
        {
            // Native InitSplineCurves accepts exactly two source points and
            // creates keys 0/1, CurveUser, with both tangents copied from Data.
            // Reconstruct that positional curve locally; never mutate Mass state.
            const auto& data = spline->Data.SplineData;
            if (data.Num() != 2 || data.Max() < 2 || !data.GetDataPtr()) return Crossing::Unresolved;
            for (int i = 0; i < 2; ++i)
            {
                raw[i].InVal = static_cast<float>(i);
                std::memcpy(&raw[i].OutVal, &data[i].Position, sizeof(SDK::FVector));
                std::memcpy(&raw[i].ArriveTangent, &data[i].Tangent, sizeof(SDK::FVector));
                raw[i].LeaveTangent = raw[i].ArriveTangent;
                raw[i].InterpMode = SDK::EInterpCurveMode::CIM_CurveUser;
            }
            points = raw.data(); count = 2;
        }
        Crossing result = Crossing::NonIntersection;
        unsigned budget = 4096; // Shared by every segment of this candidate.
        for (int i = 0; i + 1 < count; ++i)
        {
            const auto& a = points[i]; const auto& b = points[i + 1];
            const double delta = double(b.InVal) - a.InVal;
            if (!std::isfinite(a.InVal) || !std::isfinite(b.InVal) || delta <= 0) return Crossing::Unresolved;
            Cubic p = { XY{a.OutVal.X, a.OutVal.Y}, {}, {}, XY{b.OutVal.X, b.OutVal.Y} };
            if (a.InterpMode == SDK::EInterpCurveMode::CIM_Constant)
            {
                if (!Finite(p[0]) || !Finite(p[3])) return Crossing::Unresolved;
                // Constant segments jump; there is no connecting line. Boundary
                // contacts remain bounds-only rather than false non-intersections.
                const auto first = CheckHull({ p[0], p[0], p[0], p[0] }, box, 16, budget);
                const auto last = CheckHull({ p[3], p[3], p[3], p[3] }, box, 16, budget);
                if (first == Crossing::Confirmed || last == Crossing::Confirmed) return Crossing::Confirmed;
                if (first == Crossing::BoundsOnly || last == Crossing::BoundsOnly) result = Crossing::BoundsOnly;
                continue;
            }
            if (a.InterpMode == SDK::EInterpCurveMode::CIM_Linear)
            {
                p[1] = { (2 * p[0].X + p[3].X) / 3, (2 * p[0].Y + p[3].Y) / 3 };
                p[2] = { (p[0].X + 2 * p[3].X) / 3, (p[0].Y + 2 * p[3].Y) / 3 };
            }
            else if (a.InterpMode == SDK::EInterpCurveMode::CIM_CurveAuto ||
                a.InterpMode == SDK::EInterpCurveMode::CIM_CurveUser || a.InterpMode == SDK::EInterpCurveMode::CIM_CurveBreak ||
                a.InterpMode == SDK::EInterpCurveMode::CIM_CurveAutoClamped)
            {
                p[1] = { p[0].X + a.LeaveTangent.X * delta / 3, p[0].Y + a.LeaveTangent.Y * delta / 3 };
                p[2] = { p[3].X - b.ArriveTangent.X * delta / 3, p[3].Y - b.ArriveTangent.Y * delta / 3 };
            }
            else return Crossing::Unresolved;
            if (!std::all_of(p.begin(), p.end(), Finite)) return Crossing::Unresolved;
            const auto segment = CheckHull(p, box, 0, budget);
            if (segment == Crossing::Confirmed) return segment;
            if (segment == Crossing::BoundsOnly) result = segment;
        }
        return result;
    }

    void OffGridDiagnostic(const MiniMapNative::NativeApi& native, const Inventory& baseline)
    {
        if (!g_types.GridTag)
        {
            LOG_WARN("MiniMap: OffGrid: disabled (grid tag unresolved); baseline unchanged");
            return;
        }
        if (!g_offGridConstructed)
        {
            native.mass.queryConstruct(g_offGridQuery.data(), reinterpret_cast<const uint8_t*>(g_subsystem) + kManagerOffset);
            g_offGridConstructed = true;
            native.mass.addTagRequirement(g_offGridQuery.data(), g_types.BuildingTag, SDK::EMassFragmentPresence::All);
            native.mass.addTagRequirement(g_offGridQuery.data(), g_types.GridTag, SDK::EMassFragmentPresence::None);
        }
        const auto started = Clock::now();
        MiniMapNative::MassEntityHandleArray handles = {};
        native.mass.getMatchingEntityHandles(g_offGridQuery.data(), &handles);
        const auto extracted = Clock::now();
        struct Owner { void* Data; MiniMapNative::TextureApi::MemoryFreeFn Free;
            ~Owner() { if (Data) Free(Data); } } owner{ handles.Data, native.texture.memoryFree };
        if (handles.Num < 0 || handles.Max < handles.Num || handles.Num > kMaximumReasonableHandleCount ||
            (handles.Num && !handles.Data))
        {
            LOG_WARN("MiniMap: OffGrid: invalid result array; diagnostic skipped");
            return;
        }
        std::size_t spline = 0, nonspline = 0, absent = 0, invalid = 0;
        std::array<std::size_t, 15> categories = {};
        for (int32_t i = 0; i < handles.Num; ++i)
        {
            const auto id = handles.Data[i];
            if (id.Index <= 0 || !id.SerialNumber) { ++invalid; continue; }
            const auto found = baseline.find(Key(id.Index, id.SerialNumber));
            if (found == baseline.end()) { ++absent; continue; }
            const auto& record = found->second;
            ++categories[record.BuildingType < 14 ? record.BuildingType : 14];
            if (record.HasSpline) ++spline; else ++nonspline;
        }
        const double queryMs = std::chrono::duration<double, std::milli>(extracted - started).count();
        const double extractionMs = std::chrono::duration<double, std::milli>(Clock::now() - extracted).count();
        // Extraction joins identities to the SAME poll's copied baseline; no
        // second worldwide record map or repeated fragment extraction is needed.
        LOG_INFO("MiniMap: OffGrid: matching=%d baseline=%zu inBaseline=%zu absentFromBaseline=%zu invalid=%zu spline=%zu nonSpline=%zu query=%.3fms extractionJoin=%.3fms classificationSource=same-poll-baseline",
            handles.Num, baseline.size(), spline + nonspline, absent, invalid, spline, nonspline, queryMs, extractionMs);
        for (std::size_t i = 0; i < categories.size(); ++i)
            if (categories[i]) LOG_INFO("MiniMap: OffGrid: category=%s count=%zu", TypeName(static_cast<uint8_t>(i)), categories[i]);
    }

    // Stage A.1 is diagnostic-only. Identity comparisons piggyback on ReadRecord
    // during an existing poll, never requiring another worldwide Mass scan.
    struct GridValidation
    {
        bool Active = false;
        bool Due = false;
        SDK::FBox Bounds = {};
        SDK::FVector Center = {};
        float Radius = 0.0f;
        std::unordered_set<uint64_t> Returned;
        std::size_t Candidates = 0, Duplicates = 0, InvalidResults = 0;
        std::size_t Baseline = 0, Matched = 0, Missing = 0;
        std::size_t ReturnedOutside = 0, UnresolvedTransforms = 0;
        std::size_t XYInsideSphereRejected = 0;
        std::size_t OutsideSplines = 0, MightCross = 0, UnknownSplineReach = 0;
        std::array<std::size_t, 15> MissingTypes = {};
        std::array<std::size_t, 5> MissingRepresentations = {};
        std::array<std::size_t, 3> MissingGridTags = {}; // absent/present/unresolved
        std::array<std::size_t, 2> MissingSplines = {};
        // Joint breakdown, bounded independently of population size.
        std::array<std::size_t, 15 * 5 * 3 * 2> Groups = {};
        std::vector<Record> MissingSamples, CrossingSamples;
        double QueryMilliseconds = 0.0, CleanupMilliseconds = 0.0;
        using DefinitionKey = std::tuple<uintptr_t, uint32_t, uint8_t, bool, int32_t, uint32_t>;
        std::map<DefinitionKey, DefinitionCensus> Definitions;
        std::size_t DefinitionOverflow = 0;
        std::array<std::size_t, 4> Crossings = {};
        std::size_t GeometryChecked = 0, GeometryDeferred = 0;
        double GeometryMilliseconds = 0.0;
        struct GeometryResult { int32_t Index, Serial; uint32_t BuildingId; Crossing Result; };
        std::array<GeometryResult, 256> GeometryResults = {};

        bool ContainsXY(const Record& record) const
        {
            return record.Position[0] > Bounds.Min.X && record.Position[0] < Bounds.Max.X &&
                record.Position[1] > Bounds.Min.Y && record.Position[1] < Bounds.Max.Y;
        }

        bool Contains(const Record& record) const
        {
            // Native first calls IsInRadius (3D distance, converted to float),
            // then applies strict XY containment. It does not test box Z limits.
            const double dx = record.Position[0] - Center.X;
            const double dy = record.Position[1] - Center.Y;
            const double dz = record.Position[2] - Center.Z;
            return ContainsXY(record) && static_cast<float>(dx * dx + dy * dy + dz * dz) <= Radius * Radius;
        }

        void Begin(const MiniMapNative::NativeApi& native)
        {
            const auto now = Clock::now();
            if (now < g_nextGridValidation) return;
            g_nextGridValidation = now + kGridValidationInterval;
            Due = true; // Off-grid diagnostics do not require a working native grid.
            if (!native.gridDiagnostic.IsAvailable() || g_types.GridClass == nullptr)
            {
                LOG_WARN("MiniMap: GridValidation: unavailable native API/class; Stage A baseline unchanged");
                return;
            }
            MiniMapMap::PlayerPose pose;
            if (!MiniMapMap::TryGetPlayerPose(pose)) return;
            auto* subsystem = static_cast<SDK::UObject*>(native.gridDiagnostic.getSubsystem(g_world));
            if (!IsClass(subsystem, g_types.GridClass))
            {
                LOG_WARN("MiniMap: GridValidation: world grid unavailable/wrong class; retry in 10s");
                return;
            }
            // PDB: UCrEntityGridSubsystem::EntityManager +0xF0. Do not query a
            // grid belonging to a different manager, and never cache its pointer.
            const void* gridManager = nullptr;
            std::memcpy(&gridManager, reinterpret_cast<const uint8_t*>(subsystem) + 0xF0, sizeof(gridManager));
            if (gridManager != g_manager)
            {
                LOG_WARN("MiniMap: GridValidation: manager mismatch; retry in 10s");
                return;
            }
            Center = { pose.WorldX, pose.WorldY, pose.WorldZ };
            Bounds.Min = { pose.WorldX - kGridTestHalfExtent, pose.WorldY - kGridTestHalfExtent, pose.WorldZ - kGridTestHalfExtent };
            Bounds.Max = { pose.WorldX + kGridTestHalfExtent, pose.WorldY + kGridTestHalfExtent, pose.WorldZ + kGridTestHalfExtent };
            Bounds.IsValid = 1;
            // Match native arithmetic from actual bounds (including float cast).
            const double width = Bounds.Max.X - Bounds.Min.X;
            const double height = Bounds.Max.Y - Bounds.Min.Y;
            const double depth = Bounds.Max.Z - Bounds.Min.Z;
            Radius = static_cast<float>(std::sqrt((height * height + width * width + depth * depth) * 0.25));
            static_assert(sizeof(SDK::FBox) == 0x38);
            static_assert(sizeof(SDK::UCrEntityGridSubsystem) == 0x150);

            // Zero is the native default TArray state. Its elements are
            // constructed by native emplacement, not by the generated SDK.
            MiniMapNative::GridResultArray results = {};
            struct ResultOwner
            {
                MiniMapNative::GridResultArray& Results;
                const MiniMapNative::NativeApi& Native;
                void Clear()
                {
                    if (Results.Data != nullptr)
                    {
                        // Must release each TSharedPtr before freeing storage.
                        if (Results.Num >= 0 && Results.Num <= Results.Max && Results.Num <= kMaximumReasonableHandleCount)
                            Native.gridDiagnostic.destructItems(Results.Data, Results.Num);
                        Native.texture.memoryFree(Results.Data);
                    }
                    Results = {};
                }
                ~ResultOwner() { Clear(); }
            } owner{ results, native };
            const auto queryStarted = Clock::now();
            native.gridDiagnostic.findInBox(subsystem, Bounds, results, nullptr, nullptr);
            QueryMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - queryStarted).count();
            if (results.Num < 0 || results.Max < results.Num || results.Num > kMaximumReasonableHandleCount ||
                (results.Num != 0 && results.Data == nullptr))
            {
                LOG_WARN("MiniMap: GridValidation: invalid result header; comparison skipped");
                return;
            }
            Candidates = static_cast<std::size_t>(results.Num);
            Returned.reserve(Candidates);
            for (int32_t i = 0; i < results.Num; ++i)
            {
                // Copy identity only; native shared-pointer members never escape.
                const auto id = results.Data[i].Entity;
                if (id.Index <= 0 || id.SerialNumber == 0) ++InvalidResults;
                else if (!Returned.insert(Key(id.Index, id.SerialNumber)).second) ++Duplicates;
            }
            const auto cleanupStarted = Clock::now();
            owner.Clear();
            CleanupMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - cleanupStarted).count();
            Active = true;
        }

        void Observe(const MiniMapNative::NativeApi& native, const Record& record)
        {
            if (!Active) return;
            const bool returned = Returned.erase(Key(record.Index, record.SerialNumber)) != 0;
            if (!record.HasTransform)
            {
                ++UnresolvedTransforms;
                if (returned) ++ReturnedOutside; // Region cannot be verified.
                return;
            }
            if (!Contains(record))
            {
                if (ContainsXY(record)) ++XYInsideSphereRejected;
                if (returned) ++ReturnedOutside;
                if (record.HasSpline)
                {
                    ++OutsideSplines;
                    // Length is only a reach heuristic, not geometry evidence.
                    // In particular, origin/curve pivot alignment is unverified.
                    const double dx = (std::max)({ Bounds.Min.X - record.Position[0], 0.0, record.Position[0] - Bounds.Max.X });
                    const double dy = (std::max)({ Bounds.Min.Y - record.Position[1], 0.0, record.Position[1] - Bounds.Max.Y });
                    if (record.SplineLength <= 0.0f) ++UnknownSplineReach;
                    else if (std::hypot(dx, dy) <= record.SplineLength)
                    {
                        ++MightCross;
                        // Validate the existing heuristic candidates only, including
                        // tagged and untagged entities. This is NOT a complete
                        // census of every possible crossing outside the rectangle.
                        if (GeometryChecked < 256 && GeometryMilliseconds < 8.0)
                        {
                            const auto started = Clock::now();
                            const auto* spline = static_cast<const SDK::FAuSplineConnectionFragment*>(
                                Fragment(native, { record.Index, record.SerialNumber }, g_types.Spline));
                            const auto classification = ValidateSpline(spline, Bounds);
                            ++Crossings[static_cast<std::size_t>(classification)];
                            GeometryResults[GeometryChecked] = { record.Index, record.SerialNumber, record.BuildingId, classification };
                            ++GeometryChecked;
                            GeometryMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                        }
                        else { ++GeometryDeferred; ++Crossings[static_cast<std::size_t>(Crossing::Unresolved)]; }
                        if (CrossingSamples.size() < 2) CrossingSamples.push_back(record);
                    }
                }
                return;
            }
            ++Baseline;
            if (returned) { ++Matched; return; }
            ++Missing;
            const auto* parameters = Parameters(native, { record.Index, record.SerialNumber });
            const auto* placement = parameters ? parameters->PlacementData : nullptr;
            if ((reinterpret_cast<uintptr_t>(placement) & 7) != 0 || !IsClass(placement, g_types.PlacementClass)) placement = nullptr;
            const DefinitionKey key{ reinterpret_cast<uintptr_t>(placement), record.BuildingId, record.BuildingType,
                record.HasSpline, record.PlacementNameIndex, record.PlacementNameNumber };
            auto definition = Definitions.find(key);
            if (definition == Definitions.end() && Definitions.size() < 4096)
            {
                auto& row = Definitions[key];
                row.Id = record.BuildingId; row.Type = record.BuildingType; row.Spline = record.HasSpline;
                row.NameIndex = record.PlacementNameIndex; row.NameNumber = record.PlacementNameNumber;
                row.Resolve(native, placement);
                definition = Definitions.find(key);
            }
            if (definition == Definitions.end()) ++DefinitionOverflow;
            else ++definition->second.Count;
            std::size_t tag = 2;
            if (g_types.GridTag != nullptr && record.Index > 0)
            {
                // PDB: FMassEntityView is 0x28 bytes, alignment 8, with no
                // owning members/destructor. Construct/use/discard in this tick.
                alignas(8) std::array<std::byte, 0x28> view = {};
                native.gridDiagnostic.viewConstruct(view.data(), g_manager, { record.Index, record.SerialNumber });
                tag = native.gridDiagnostic.viewHasTag(view.data(), g_types.GridTag) ? 1 : 0;
            }
            const auto type = record.BuildingType < 14 ? record.BuildingType : 14;
            const auto representation = record.Representation < 4 ? record.Representation : 4;
            ++MissingTypes[type];
            ++MissingRepresentations[representation];
            ++MissingGridTags[tag];
            ++MissingSplines[record.HasSpline ? 1 : 0];
            ++Groups[((type * 5 + representation) * 3 + tag) * 2 + (record.HasSpline ? 1 : 0)];
            if (MissingSamples.size() < 2) MissingSamples.push_back(record);
        }

        void Report() const
        {
            if (!Active) return;
            // Unmatched results may be non-buildings or stale grid identities;
            // never dereference them merely to classify an extra result.
            LOG_INFO("MiniMap: GridValidation: generation=%llu XY=(%.0f,%.0f)-(%.0f,%.0f) centerZ=%.0f sphereRadius=%.0fUU candidates=%zu unique=%zu baseline=%zu matchingBuildings=%zu missing=%zu extra=%zu nonBaseline=%zu outsideOrUnresolved=%zu duplicate=%zu invalid=%zu unresolvedTransforms=%zu xyInsideSphereRejected=%zu query=%.3fms cleanup=%.3fms collectIncludesComparison=1",
                static_cast<unsigned long long>(g_generation), Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y,
                Center.Z, Radius,
                Candidates, Candidates - Duplicates - InvalidResults, Baseline, Matched, Missing,
                Returned.size() + ReturnedOutside, Returned.size(), ReturnedOutside, Duplicates, InvalidResults,
                UnresolvedTransforms, XYInsideSphereRejected, QueryMilliseconds, CleanupMilliseconds);
            LOG_INFO("MiniMap: GridValidation: missing gridTag(absent/present/unknown)=%zu/%zu/%zu rep(high/low/ISM/none/unknown)=%zu/%zu/%zu/%zu/%zu spline(no/yes)=%zu/%zu outsideSplines=%zu mightCrossUnverified=%zu unknownReach=%zu (outside origins excluded from failures)",
                MissingGridTags[0], MissingGridTags[1], MissingGridTags[2], MissingRepresentations[0], MissingRepresentations[1],
                MissingRepresentations[2], MissingRepresentations[3], MissingRepresentations[4], MissingSplines[0], MissingSplines[1],
                OutsideSplines, MightCross, UnknownSplineReach);
            LOG_INFO("MiniMap: GridValidation: missing types Research=%zu Habitat=%zu Survival=%zu Player=%zu Power=%zu Extraction=%zu RawProcessing=%zu Crafting=%zu Transport=%zu Defensive=%zu Custom=%zu Temperature=%zu All=%zu Test=%zu unresolved=%zu",
                MissingTypes[0], MissingTypes[1], MissingTypes[2], MissingTypes[3], MissingTypes[4], MissingTypes[5], MissingTypes[6],
                MissingTypes[7], MissingTypes[8], MissingTypes[9], MissingTypes[10], MissingTypes[11], MissingTypes[12], MissingTypes[13], MissingTypes[14]);
            std::size_t logged = 0, omitted = 0;
            for (std::size_t i = 0; i < Groups.size(); ++i)
            {
                if (Groups[i] == 0) continue;
                if (logged++ >= 6) { omitted += Groups[i]; continue; }
                LOG_INFO("MiniMap: GridValidation: missing group type=%s rep=%s gridTag=%s spline=%zu count=%zu",
                    TypeName(static_cast<uint8_t>(i / 30)), RepresentationName(static_cast<uint8_t>((i / 6) % 5)),
                    ((i / 2) % 3) == 0 ? "absent" : ((i / 2) % 3) == 1 ? "present" : "unknown", i % 2, Groups[i]);
            }
            if (omitted) LOG_INFO("MiniMap: GridValidation: additional missing groups records=%zu (histograms above are complete)", omitted);
            ReportCensus();
            LOG_INFO("MiniMap: SplineCoverage: candidates=%zu checked=%zu confirmed=%zu boundsOnly=%zu nonIntersection=%zu unresolved=%zu deferredByBudget=%zu geometry=%.3fms scope=existing-length-heuristic centerlineXY=1",
                MightCross, GeometryChecked, Crossings[0], Crossings[1], Crossings[2], Crossings[3], GeometryDeferred, GeometryMilliseconds);
            std::vector<std::string> geometryRows;
            for (std::size_t i = 0; i < GeometryChecked; ++i)
            {
                if (i % 16 == 0) geometryRows.emplace_back();
                const auto& item = GeometryResults[i];
                static constexpr const char* labels[] = { "confirmed", "boundsOnly", "nonIntersection", "unresolved" };
                geometryRows.back() += std::to_string(item.Index) + ":" + std::to_string(item.Serial) +
                    "/buildingID=" + std::to_string(item.BuildingId) + "/" + labels[static_cast<std::size_t>(item.Result)] + " ";
            }
            if (geometryRows != g_geometryRows)
            {
                g_geometryRows = std::move(geometryRows);
                for (const auto& row : g_geometryRows) LOG_INFO("MiniMap: SplineCoverage: identities %s", row.c_str());
            }
            for (const auto& record : MissingSamples) LogRecord("grid missing origin-in-box", record);
            for (const auto& record : CrossingSamples) LogRecord("outside spline might-cross UNVERIFIED", record);
        }

        void ReportCensus() const
        {
            std::size_t sum = DefinitionOverflow;
            std::vector<std::string> rows;
            rows.reserve(Definitions.size());
            for (const auto& [key, row] : Definitions)
            {
                sum += row.Count;
                rows.push_back("buildingID=" + std::to_string(row.Id) + " enum=\"" + row.Identifier +
                    "\" category=" + TypeName(row.Type) + " placement=\"" + row.Placement +
                    "\" placementFName=" + std::to_string(row.NameIndex) + ":" + std::to_string(row.NameNumber) +
                    " config=\"" + row.Config + "\" gridTrait=" + row.GridTrait +
                    " spline=" + std::to_string(row.Spline) + " count=" + std::to_string(row.Count));
            }
            // Pointer order may change across worlds; text order stays deterministic.
            std::sort(rows.begin(), rows.end());
            if (rows != g_censusRows)
            {
                g_censusRows = std::move(rows);
                g_censusPage = 0;
                ++g_censusEpoch;
            }
            LOG_INFO("MiniMap: ExclusionCensus: generation=%llu epoch=%llu definitions=%zu definitionSum=%zu overflow=%zu missing=%zu sumMatches=%d complete=%d rowsPublished=%zu",
                static_cast<unsigned long long>(g_generation), static_cast<unsigned long long>(g_censusEpoch),
                Definitions.size(), sum - DefinitionOverflow, DefinitionOverflow, Missing, sum == Missing,
                DefinitionOverflow == 0 && sum == Missing, g_censusPage);
            const auto end = (std::min)(g_censusPage + 32, g_censusRows.size());
            for (; g_censusPage < end; ++g_censusPage)
                LOG_INFO("MiniMap: ExclusionCensus: epoch=%llu row=%zu/%zu %s", static_cast<unsigned long long>(g_censusEpoch),
                    g_censusPage + 1, g_censusRows.size(), g_censusRows[g_censusPage].c_str());
            if (DefinitionOverflow) LOG_WARN("MiniMap: ExclusionCensus: 4096-definition safety limit reached; census INCOMPLETE overflow=%zu", DefinitionOverflow);
        }
    };

    void Poll()
    {
        const auto started = Clock::now();
        const auto* native = MiniMapNative::Get();
        if (native == nullptr || !EnsureQuery(*native))
        {
            Warn("query unavailable");
            return;
        }
        MiniMapNative::MassEntityHandleArray handles = {};
        native->mass.getMatchingEntityHandles(g_query.data(), &handles);
        struct HandleOwner
        {
            void* Data;
            MiniMapNative::TextureApi::MemoryFreeFn Free;
            ~HandleOwner() { if (Data != nullptr) Free(Data); }
        } owner{ handles.Data, native->texture.memoryFree };
        if (handles.Num < 0 || handles.Max < handles.Num || handles.Num > kMaximumReasonableHandleCount ||
            (handles.Num != 0 && handles.Data == nullptr))
        {
            Warn("invalid native handle array");
            return; // Never turn a failed collection into removals.
        }
        Inventory next;
        next.reserve(handles.Num);
        GridValidation gridValidation;
        gridValidation.Begin(*native);
        std::size_t missingTransform = 0, missingPlacement = 0, missingType = 0, missingNetwork = 0, splines = 0;
        std::array<std::size_t, 5> representations = {};
        for (int32_t i = 0; i < handles.Num; ++i)
        {
            const auto id = handles.Data[i];
            if (id.Index < 0 || id.SerialNumber == 0)
            {
                Warn("invalid Mass identity");
                return;
            }
            Record record = ReadRecord(*native, id);
            gridValidation.Observe(*native, record);
            missingTransform += !record.HasTransform;
            missingPlacement += !record.HasPlacement;
            missingType += record.BuildingType == 0xFF;
            missingNetwork += !record.HasNetworkIdentity;
            splines += record.HasSpline;
            ++representations[record.Representation < 4 ? record.Representation : 4];
            if (!next.emplace(Key(id.Index, id.SerialNumber), record).second)
            {
                Warn("duplicate Mass identity");
                return;
            }
        }
        // No native handle/fragment pointers are needed after this point.
        if (owner.Data != nullptr)
        {
            owner.Free(owner.Data);
            owner.Data = nullptr;
        }
        std::size_t added = 0, removed = 0, updated = 0;
        std::vector<Record> additions, removals;
        const bool initial = !g_haveInventory;
        {
            std::scoped_lock lock(g_mutex);
            if (!initial)
            {
                for (const auto& [key, record] : next)
                {
                    const auto old = g_inventory.find(key);
                    if (old == g_inventory.end())
                    {
                        ++added;
                        if (additions.size() < kEventSamples) additions.push_back(record);
                    }
                    else updated += !(record == old->second);
                }
                for (const auto& [key, record] : g_inventory)
                    if (next.find(key) == next.end())
                    {
                        ++removed;
                        if (removals.size() < kEventSamples) removals.push_back(record);
                    }
            }
            g_inventory.swap(next);
        }
        next.clear(); // Include old-record destruction in collection timing.
        g_haveInventory = true;
        g_intervalAdded += added;
        g_intervalRemoved += removed;
        // This includes native query, copying, optional lookups, reconciliation
        // and publication. Diagnostic formatting/output follows this measurement.
        const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        g_maxMilliseconds = (std::max)(g_maxMilliseconds, milliseconds);
        gridValidation.Report();
        if (gridValidation.Due)
        {
            // Keep the separate query and its output outside the existing
            // collection measurement. TotalWithDiagnostics still includes it.
            std::scoped_lock lock(g_mutex);
            OffGridDiagnostic(*native, g_inventory);
        }
        if (initial) LOG_INFO("MiniMap: BuildingInventory: initial count=%d generation=%llu", handles.Num, static_cast<unsigned long long>(g_generation));
        if (added != 0 || removed != 0)
            LOG_INFO("MiniMap: BuildingInventory: delta added=%zu removed=%zu updated=%zu total=%d (samples <=4 each)", added, removed, updated, handles.Num);
        for (const auto& record : additions) LogRecord("added", record);
        for (const auto& record : removals) LogRecord("removed", record);
        const auto now = Clock::now();
        if (initial || now >= g_nextDiagnostic)
        {
            LOG_INFO("MiniMap: BuildingInventory: matching=%d retained=%d added=%zu removed=%zu updated=%zu missingTransform=%zu missingPlacement=%zu missingType=%zu missingNetwork=%zu rep(high/low/ISM/none/missing)=%zu/%zu/%zu/%zu/%zu splines=%zu collect=%.3fms max=%.3fms",
                handles.Num, handles.Num, g_intervalAdded, g_intervalRemoved, updated,
                missingTransform, missingPlacement, missingType, missingNetwork,
                representations[0], representations[1], representations[2], representations[3], representations[4],
                splines, milliseconds, g_maxMilliseconds);
            std::size_t samples = 0;
            // No render consumer exists in Stage A. Only copied records are
            // inspected here; classification examples are once per type/rep.
            std::scoped_lock lock(g_mutex);
            for (const auto& [key, record] : g_inventory)
            {
                const uint64_t typeKey = (uint64_t(record.BuildingId) << 24) |
                    (uint64_t(record.BuildingType) << 16) | (uint64_t(record.Representation) << 8) | record.HasSpline;
                if (g_loggedTypes.insert(typeKey).second)
                {
                    LogRecord("classification", record);
                    if (++samples == kTypeSamples) break;
                }
            }
            samples = 0;
            for (const auto& [key, record] : g_inventory)
            {
                if ((!record.HasTransform || !record.HasPlacement || record.BuildingType == 0xFF) &&
                    g_loggedGaps.insert(key).second)
                {
                    LogRecord("unresolved data", record);
                    if (++samples == kEventSamples) break;
                }
            }
            g_nextDiagnostic = now + kDiagnosticInterval;
            g_intervalAdded = g_intervalRemoved = 0;
            g_maxMilliseconds = 0.0;
        }
        if (MiniMapBuildingCollector::BeginComparison(g_world))
        {
            std::scoped_lock lock(g_mutex);
            for (const auto& [key, record] : g_inventory)
                MiniMapBuildingCollector::ObserveBaseline(record);
            MiniMapBuildingCollector::EndComparison();
        }
        const double totalMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (totalMilliseconds > 20.0 && Clock::now() >= g_nextWarning)
        {
            LOG_WARN("MiniMap: BuildingInventory: slow poll collect=%.3fms totalWithDiagnostics=%.3fms matching=%d",
                milliseconds, totalMilliseconds, handles.Num);
            g_nextWarning = Clock::now() + kDiagnosticInterval;
        }
    }

    void OnTick(float)
    {
        if (!g_ready || !MiniMapMap::HasWorld()) return;
        const auto now = Clock::now();
        if (now < g_nextPoll) return;
        g_nextPoll = now + kPollInterval; // No catch-up loops after a hitch.
        Poll();
    }
}

namespace MiniMapBuildingInventory
{
    bool Initialize(IPluginSelf* self)
    {
        if (g_initialized) return true;
        if (self == nullptr || self->hooks == nullptr || self->hooks->Engine == nullptr ||
            self->hooks->ObjectWalker == nullptr || self->hooks->ObjectWalker->IsReady == nullptr ||
            self->hooks->ObjectWalker->FindObjectsByNameInto == nullptr) return false;
        g_inventorySelf = self;
        Reset();
        self->hooks->Engine->RegisterOnTick(&OnTick);
        g_initialized = true;
        LOG_INFO("MiniMap: BuildingInventory: Stage A enabled (read-only; no rendering)");
        return true;
    }

    void Reset()
    {
        g_ready = false;
        DestroyQuery();
        ClearInventory("lifecycle reset");
        g_types = {};
        g_typesResolved = false;
        g_typesAttempted = false;
        g_nextPoll = {};
    }

    void OnExperienceLoadComplete()
    {
        if (g_initialized)
        {
            g_ready = true;
            g_nextPoll = {};
        }
    }

    std::vector<Record> CopySnapshot()
    {
        std::scoped_lock lock(g_mutex);
        std::vector<Record> result;
        result.reserve(g_inventory.size());
        for (const auto& [key, record] : g_inventory) result.push_back(record);
        return result;
    }

    void Shutdown()
    {
        if (!g_initialized) return;
        g_ready = false;
        g_inventorySelf->hooks->Engine->UnregisterOnTick(&OnTick);
        Reset(); // Query destruction must precede NativeApi shutdown.
        g_inventorySelf = nullptr;
        g_initialized = false;
    }
}

#endif
