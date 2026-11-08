// Independently acquires StarRupture native building facts. All ABI-specific access
// stays here; no rendering, legacy collector, service interface or runtime caller is used.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollectorV2.h"
#include "SplinePolicyId.h"
#include "../Map.h"
#include "Services/Map/Buildings/BuildingCandidatePublication.h"
#include "BuildingDefinitionAcquisition.h"
#include "BuildingSplineSource.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/AuActorPlacement_structs.hpp>
#include <SDK/Chimera_classes.hpp>
#include <SDK/CoreUObject_structs.hpp>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MiniMap::BuildingAcquisition
{
    using Facts = BuildingAcquiredFacts;
    using Status = Facts::Status;
    using Native = MiniMapNative::NativeApi;
    using EntityId = MiniMapNative::MassEntityHandle;
    using DefinitionOwner = BuildingDefinitionAcquisition::DefinitionOwner;

    /** Sole mutable acquisition owner; publications are frozen CPU projections only. */
    struct BuildingCollectorV2::State
    {
        using Clock = std::chrono::steady_clock;

        /** Metadata roots belong exclusively to this collector instance. */
        struct Metadata
        {
            std::vector<SDK::UObject*> Roots;
            SDK::UScriptStruct* Building = nullptr;
            SDK::UScriptStruct* Transform = nullptr;
            SDK::UScriptStruct* Parameters = nullptr;
            SDK::UScriptStruct* Spline = nullptr;
            SDK::UScriptStruct* Placement = nullptr;

            /** Balances roots after explicit shutdown or failed initialization. */
            ~Metadata()
            {
                const auto* native = MiniMapNative::Get();
                assert(Roots.empty() || native);
                if (native)
                    for (auto* root : Roots)
                        native->asset.releaseObject(root);
            }

            /** Resolves a resident script struct, recording ownership before retaining it. */
            SDK::UScriptStruct* Lookup(const wchar_t* path)
            {
                const auto result = Services::TryObjects()->FindResident(nullptr, nullptr, path, false);
                auto* value = result.value ? *result.value : nullptr;
                if (value)
                {
                    Roots.push_back(value);
                    MiniMapNative::Get()->asset.retainObject(value);
                }
                return static_cast<SDK::UScriptStruct*>(value);
            }

            /** Loads acquisition declarations only, with no visualization traits/classes. */
            bool Initialize()
            {
                Building = Lookup(L"/Script/Chimera.CrMassBuildingTag");
                Transform = Lookup(L"/Script/MassCommon.TransformFragment");
                Parameters = Lookup(L"/Script/Chimera.CrBuildingParameters");
                Spline = Lookup(L"/Script/AuActorPlacement.AuSplineConnectionFragment");
                Placement = Lookup(L"/Script/AuActorPlacement.AuAPMassFragment");
                return Building && Transform && Parameters && Spline && Placement;
            }
        };

        /** Native correspondence and scan mark for one authoritative building occurrence. */
        struct Entry
        {
            Facts::Building Data;
            std::shared_ptr<DefinitionOwner> Definition;
            std::uint64_t SeenScan = 0;
        };

        /** Captured context used after every potentially reentrant acquisition operation. */
        struct Context
        {
            SDK::UWorld* World;
            const void* Manager;
            std::uint64_t Session;
        };

        /** Owns only the native result allocation, not its entity handles' lifetimes. */
        struct HandleArrayOwner
        {
            const Native& Api;
            MiniMapNative::MassEntityHandleArray Handles{};
            /** Frees native array storage even on exceptions/context rejection. */
            ~HandleArrayOwner()
            {
                if (Handles.Data)
                    Api.texture.memoryFree(Handles.Data);
            }
        };

        /** Carries synchronous grid visitor context; grid visits only prioritize pending keys. */
        struct GridVisit
        {
            State& Owner;
            std::unordered_set<std::uint64_t>& Keys;
        };

        std::unordered_map<std::uint64_t, Entry> Entities;
        std::unordered_set<std::uint64_t> Pending;
        std::unordered_map<SDK::UAuActorPlacementData*, std::shared_ptr<DefinitionOwner>> Definitions;
        std::shared_ptr<Metadata> Types;
        BuildingDefinitionAcquisition DefinitionAcquisition;
        // StarRupture's established UE 5.6.1 query ABI, Section 22: size 0x350, align 0x10.
        alignas(16) std::array<std::byte, 0x350> Query{};
        bool QueryConstructed = false;
        SDK::UWorld* World = nullptr;
        const void* Manager = nullptr;
        std::uint64_t Generation = 0;
        std::uint64_t ScanEpoch = 0;
        Clock::time_point NextMembership{};
        bool MembershipComplete = false;
        bool Dirty = true;
        bool RemoteTurn = false;
        bool HasRegion = false;
        bool IncludeDroneRails = true;
        Facts::Region RequestedRegion;
        std::shared_ptr<const Facts> Published;
        std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication> PublishedCandidates;
        std::shared_ptr<std::atomic<bool>> PublicationEligibility;

        /** Packs the full session-private Index/Serial pair; never a content fingerprint. */
        static std::uint64_t Key(EntityId id) noexcept
        {
            return std::uint64_t(std::uint32_t(id.Index)) |
                (std::uint64_t(std::uint32_t(id.SerialNumber)) << 32);
        }

        /** Captures logical source lifetime before native operations. */
        Context CaptureContext() const noexcept
        {
            return {World, Manager, Generation};
        }

        /** Marks exposed bytes historical immediately; their retained CPU lifetime is unaffected. */
        void Invalidate() noexcept
        {
            Dirty = true;
            if (PublicationEligibility)
                PublicationEligibility->store(false, std::memory_order_release);
            PublicationEligibility.reset();
            PublishedCandidates.reset();
        }

        /** Obtains existing native shared-pointer storage; never fabricates shared ownership.
         * StarRupture/UE 5.6.1 ABI: subsystem +0x38 is a 16-byte TSharedPtr, first
         * word manager, second controller. This is version-specific, not an SDK property.
         */
        static const std::byte* ManagerStorage(const Native& native, SDK::UWorld* world)
        {
            static_assert(sizeof(void*) == 8);
            auto* subsystem = world ? native.mass.getMassEntitySubsystem(world) : nullptr;
            return subsystem ? reinterpret_cast<const std::byte*>(subsystem) + 0x38 : nullptr;
        }

        /** Borrows only the object word; query construction separately receives the full storage address. */
        static const void* ReadManager(const std::byte* storage) noexcept
        {
            const void* manager = nullptr;
            if (storage)
                std::memcpy(&manager, storage, sizeof(manager));
            return manager;
        }

        /** Rejects stale session/query context and changed native manager after reentry. */
        bool IsContextCurrent(const Native& native, const Context& context) const
        {
            if (!QueryConstructed || World != context.World || Manager != context.Manager ||
                Generation != context.Session)
                return false;
            return ReadManager(ManagerStorage(native, context.World)) == context.Manager;
        }

        /** Destroys query before releasing roots; caller keeps native services/world valid. */
        void ClearSession(bool clearRequest)
        {
            Invalidate();
            ++Generation;
            if (QueryConstructed)
                MiniMapNative::Get()->mass.queryDestruct(Query.data());
            QueryConstructed = false;
            World = nullptr;
            Manager = nullptr;
            Pending.clear();
            Entities.clear();
            Definitions.clear();
            ScanEpoch = 0;
            NextMembership = {};
            MembershipComplete = false;
            RemoteTurn = false;
            Published.reset();
            PublishedCandidates.reset();
            if (clearRequest)
            {
                HasRegion = false;
                RequestedRegion = {};
            }
        }

        /** Establishes the native query once for this collector's gameplay session. */
        bool EnsureSession(const Native& native, SDK::UWorld* world)
        {
            if (!world || !Types)
                return false;
            if (QueryConstructed)
                return true;
            const auto* storage = ManagerStorage(native, world);
            const auto* manager = ReadManager(storage);
            if (!manager)
                return false;
            World = world;
            Manager = manager;
            ++Generation;
            // Existing Win64 ctor: RCX=query, RDX=native shared-owner storage, RAX=this.
            native.mass.queryConstruct(Query.data(), storage);
            QueryConstructed = true;
            native.mass.addTagRequirement(Query.data(), Types->Building, SDK::EMassFragmentPresence::All);
            native.mass.addTransformRequirement(Query.data(),
                std::uint8_t(SDK::EMassFragmentAccess::ReadOnly), std::uint8_t(SDK::EMassFragmentPresence::Optional));
            Invalidate();
            return true;
        }

        /** Advances mark epoch, clearing old marks before reusing epoch one. */
        std::uint64_t NextScanEpoch()
        {
            if (ScanEpoch == (std::numeric_limits<std::uint64_t>::max)())
            {
                for (auto& item : Entities)
                    item.second.SeenScan = 0;
                ScanEpoch = 0;
            }
            return ++ScanEpoch;
        }

        /** Inserts one full handle with rollback if pending-index insertion cannot allocate. */
        void MarkHandle(EntityId id, std::uint64_t epoch)
        {
            const auto key = Key(id);
            auto [entry, added] = Entities.try_emplace(key);
            if (added)
            {
                try
                {
                    Pending.insert(key);
                }
                catch (...)
                {
                    Entities.erase(entry);
                    throw;
                }
                entry->second.Data.Index = id.Index;
                entry->second.Data.Serial = id.SerialNumber;
                Invalidate();
            }
            entry->second.SeenScan = epoch;
        }

        /** Removes absent entries only after the complete native scan/context was validated. */
        void SweepMembership(std::uint64_t epoch)
        {
            for (auto entry = Entities.begin(); entry != Entities.end();)
            {
                if (entry->second.SeenScan == epoch)
                    ++entry;
                else
                {
                    Pending.erase(entry->first);
                    entry = Entities.erase(entry);
                    Invalidate();
                }
            }
        }

        /** Reconciles authoritative membership; failed traversal cannot authorize a removal sweep. */
        bool Reconcile(const Native& native)
        {
            const auto context = CaptureContext();
            const auto epoch = NextScanEpoch();
            HandleArrayOwner owner{native};
            const auto* returned = native.mass.getMatchingEntityHandles(Query.data(), &owner.Handles);
            const auto& handles = owner.Handles;
            if (returned != &handles || handles.Num < 0 || handles.Max < handles.Num ||
                (handles.Num && !handles.Data) || !IsContextCurrent(native, context))
                return false;
            bool valid = true;
            for (int index = 0; index < handles.Num; ++index)
            {
                const auto id = handles.Data[index];
                if (id.Index < 0 || !id.SerialNumber)
                    valid = false;
                else
                    MarkHandle(id, epoch); // Duplicate handles merely repeat the same mark.
            }
            if (!valid || !IsContextCurrent(native, context))
                return false;
            SweepMembership(epoch);
            return true;
        }

        /** Revokes scan completeness before a due scan, including exceptions or reentrant reset. */
        void ReconcileIfDue(const Native& native)
        {
            if (Clock::now() < NextMembership)
                return;
            const auto context = CaptureContext();
            bool complete = false;
            try
            {
                complete = Reconcile(native);
            }
            catch (...)
            {
                if (Generation == context.Session)
                {
                    MembershipComplete = false;
                    Invalidate();
                }
                throw;
            }
            if (!IsContextCurrent(native, context))
                return;
            if (MembershipComplete != complete)
                Invalidate();
            MembershipComplete = complete;
            NextMembership = Clock::now() + std::chrono::seconds(1);
        }

        /** Borrows the aligned parameter value only for immediate placement acquisition.
         * Established UE 5.6.1 FConstSharedStruct ABI: first word is shared storage,
         * whose 8-byte script-struct header precedes AlignUp(payload, MinAlignment).
         * The returned wrapper/value are manager-owned and must never be freed/copied as owners.
         */
        const SDK::FCrBuildingParameters* Parameters(const Native& native, EntityId id) const
        {
            static_assert(sizeof(SDK::FCrBuildingParameters) == 0x18);
            const auto* wrapper = native.mass.getConstSharedFragmentPtr(Manager, id, Types->Parameters);
            const std::byte* storage = nullptr;
            if (wrapper)
                std::memcpy(&storage, wrapper, sizeof(storage));
            const auto alignment = std::uintptr_t(Types->Parameters->MinAlignment);
            if (!storage || !alignment || (alignment & (alignment - 1)))
                return nullptr;
            const auto address = (reinterpret_cast<std::uintptr_t>(storage) + sizeof(void*) + alignment - 1) &
                ~(alignment - 1);
            return reinterpret_cast<const SDK::FCrBuildingParameters*>(address);
        }

        /** Finds or roots one placement in the sole session definition cache. */
        std::shared_ptr<DefinitionOwner> FindDefinition(const Native& native, EntityId id)
        {
            SDK::UAuActorPlacementData* placement = nullptr;
            {
                const auto* parameters = Parameters(native, id);
                placement = parameters ? parameters->PlacementData : nullptr;
            }
            if (!placement)
                return {};
            auto found = Definitions.find(placement);
            if (found != Definitions.end())
                return found->second;
            auto definition = DefinitionAcquisition.Retain(placement);
            return Definitions.emplace(placement, std::move(definition)).first->second;
        }

        /** Validates a copied fixed-size numerical value, never native memory. */
        template<std::size_t Size> static bool Finite(const std::array<double, Size>& values)
        {
            for (double value : values)
                if (!std::isfinite(value))
                    return false;
            return true;
        }

        /** Copies optional native transform and normalizes orientation, retaining signed scale. */
        void CopyTransform(const Native& native, EntityId id, Facts::Building& result) const
        {
            if (result.TransformStatus == Status::Ready)
                return;
            const auto* transform = static_cast<const SDK::FTransform*>(
                native.mass.getFragmentDataPtr(Manager, id, Types->Transform));
            result.TransformStatus = Status::Missing;
            if (!transform)
                return;
            result.Position = {transform->Translation.X, transform->Translation.Y, transform->Translation.Z};
            result.SignedScale = {transform->Scale3D.X, transform->Scale3D.Y, transform->Scale3D.Z};
            result.Rotation = {transform->Rotation.X, transform->Rotation.Y, transform->Rotation.Z, transform->Rotation.W};
            double norm = 0;
            for (double value : result.Rotation)
                norm += value * value;
            result.TransformStatus = Status::Invalid;
            if (!Finite(result.Position) || !Finite(result.SignedScale) || !Finite(result.Rotation) ||
                !std::isfinite(norm) || norm <= 0)
                return;
            for (double& value : result.Rotation)
                value /= std::sqrt(norm);
            result.TransformStatus = Status::Ready;
        }

        /** Copies a scoped AP handle argument only; it is not an owning or retained handle. */
        bool CopyPayloadArgument(const Native& native, EntityId id, SDK::FAuAPPayloadDataHandle& handle) const
        {
            const auto* placement = static_cast<const SDK::FAuAPMassFragment*>(
                native.mass.getFragmentDataPtr(Manager, id, Types->Placement));
            if (!placement)
                return false;
            handle = placement->Payload;
            return true;
        }

        /** Ends every native fragment borrow before the potentially reentrant payload getter. */
        void CopySpline(const Native& native, EntityId id, Facts::Building& result) const
        {
            if (result.SplineStatus == Status::Ready)
                return;
            Facts::Spline source;
            {
                const auto* fragment = static_cast<const SDK::FAuSplineConnectionFragment*>(
                    native.mass.getFragmentDataPtr(Manager, id, Types->Spline));
                result.SplineClassificationStatus = Status::Ready;
                result.HasSpline = fragment != nullptr;
                if (!fragment)
                {
                    result.SplineStatus = Status::Ready;
                    result.SplineFacts.reset();
                    return;
                }
                source = BuildingSplineSource::CopyFragment(*fragment);
            }
            if (BuildingSplineSource::NeedsFallback(source))
            {
                SDK::FAuAPPayloadDataHandle handle{};
                if (CopyPayloadArgument(native, id, handle))
                    BuildingSplineSource::CopyFallback(handle, source);
            }
            result.SplineStatus = source.SourceStatus;
            if (!result.SplineFacts || !(*result.SplineFacts == source))
                result.SplineFacts = std::make_shared<const Facts::Spline>(std::move(source));
        }

        /** Acquires shared core/profile facts once per callback without retaining entry references across reentry. */
        void CopyDefinition(const Native& native, const Context& context,
            const std::shared_ptr<DefinitionOwner>& definition,
            std::unordered_set<const DefinitionOwner*>& attempted, bool hasSpline)
        {
            const bool coreComplete = definition->Facts && BuildingDefinitionAcquisition::IsComplete(*definition->Facts);
            const bool profilesTerminal = !hasSpline || (definition->Facts && IsSplineProfileTerminal(
                definition->Facts->SplineProfileStatus));
            if (coreComplete && profilesTerminal)
                return;
            if (!attempted.insert(definition.get()).second)
                return;
            if (!coreComplete)
            {
                auto facts = DefinitionAcquisition.Copy(definition);
                if (!IsContextCurrent(native, context))
                    return;
                definition->Facts = std::move(facts);
            }
            if (!hasSpline || !definition->Facts || !BuildingDefinitionAcquisition::IsComplete(*definition->Facts))
                return;
            if (IsDroneRailDefinitionId(definition->Facts->BuildingId))
            {
                auto facts = std::make_shared<Facts::Definition>(*definition->Facts);
                facts->SplineProfileStatus = ::Services::Map::Buildings::SplineProfileAcquisitionStatus::NotApplicable;
                definition->Facts = std::move(facts);
                return;
            }
            if (IsSplineProfileTerminal(definition->Facts->SplineProfileStatus))
                return;
            auto facts = DefinitionAcquisition.CopySplineProfiles(definition);
            if (facts && IsContextCurrent(native, context))
                definition->Facts = std::move(facts);
        }

        /** Identifies profile outcomes that need no further acquisition retry. */
        static bool IsSplineProfileTerminal(::Services::Map::Buildings::SplineProfileAcquisitionStatus status)
        {
            using ProfileStatus = ::Services::Map::Buildings::SplineProfileAcquisitionStatus;
            return status == ProfileStatus::Complete || status == ProfileStatus::NotApplicable ||
                status == ProfileStatus::Invalid || status == ProfileStatus::Unsupported;
        }

        /** Preserves the exact legacy drone-rail definition family without profile acquisition. */
        static bool IsDroneRailDefinitionId(std::uint32_t buildingId)
        {
            switch (static_cast<SDK::ECrBuildingID>(buildingId))
            {
            case SDK::ECrBuildingID::DroneRailT1:
            case SDK::ECrBuildingID::DroneRailT2:
            case SDK::ECrBuildingID::DroneRailT3:
            case SDK::ECrBuildingID::DroneRailT4:
            case SDK::ECrBuildingID::DroneRailT5:
                return true;
            default:
                return false;
            }
        }

        /** Requires applicable definition profiles before an occurrence leaves pending acquisition. */
        static bool AreRequiredSplineProfilesResolved(
            const std::shared_ptr<const Facts::Definition>& definition, bool hasSpline)
        {
            if (!hasSpline)
                return true;
            if (!definition || !BuildingDefinitionAcquisition::IsComplete(*definition))
                return false;
            if (IsDroneRailDefinitionId(definition->BuildingId))
                return definition->SplineProfileStatus ==
                    ::Services::Map::Buildings::SplineProfileAcquisitionStatus::NotApplicable;
            return IsSplineProfileTerminal(definition->SplineProfileStatus);
        }

        /** Summarizes missing/invalid definition evidence without relabelling pending data as invalid. */
        static Status DefinitionStatus(const std::shared_ptr<const Facts::Definition>& definition)
        {
            if (!definition)
                return Status::Missing;
            if (BuildingDefinitionAcquisition::IsComplete(*definition))
                return Status::Ready;
            for (const auto status : {Status::Invalid, Status::Unavailable, Status::Missing, Status::Unknown})
                if (definition->ClassStatus == status || definition->InteriorStatus == status ||
                    (status != Status::Missing && definition->ConfigReferenceStatus == status))
                    return status;
            return Status::Missing;
        }

        /** Commits copied facts only after session/manager and full entity correspondence revalidation. */
        void Commit(const Native& native, const Context& context, std::uint64_t key,
            Facts::Building&& result, const std::shared_ptr<DefinitionOwner>& definition)
        {
            if (!IsContextCurrent(native, context))
                return;
            auto found = Entities.find(key);
            if (found == Entities.end() || found->second.Data.Complete)
                return;
            const EntityId id{result.Index, result.Serial};
            if (result.EntityStatus == Status::Ready &&
                (!native.mass.isEntityValid(Manager, id) || !native.mass.isEntityBuilt(Manager, id)))
                return;
            if (!(found->second.Data == result))
            {
                found->second.Data = std::move(result);
                Invalidate();
            }
            found->second.Definition = definition;
            if (found->second.Data.Complete)
            {
                found->second.Definition.reset();
                Pending.erase(key);
            }
        }

        /** Acquires independent native facts with owned locals across payload/reflection reentry. */
        void Acquire(const Native& native, std::uint64_t key, std::unordered_set<const DefinitionOwner*>& attempted)
        {
            Facts::Building result;
            std::shared_ptr<DefinitionOwner> definition;
            {
                const auto found = Entities.find(key);
                if (found == Entities.end() || found->second.Data.Complete)
                    return;
                result = found->second.Data;
                definition = found->second.Definition;
            }
            const auto context = CaptureContext();
            const EntityId id{result.Index, result.Serial};
            result.EntityStatus = Status::Missing;
            if (native.mass.isEntityValid(Manager, id) && native.mass.isEntityBuilt(Manager, id))
            {
                result.EntityStatus = Status::Ready;
                if (!definition)
                    definition = FindDefinition(native, id);
                CopyTransform(native, id, result);
                CopySpline(native, id, result);
                if (!IsContextCurrent(native, context))
                    return;
                if (definition)
                    CopyDefinition(native, context, definition, attempted,
                        result.SplineClassificationStatus == Status::Ready && result.HasSpline);
                if (!IsContextCurrent(native, context))
                    return;
                result.DefinitionFacts = definition ? definition->Facts : nullptr;
                result.DefinitionStatus = DefinitionStatus(result.DefinitionFacts);
                result.Complete = result.DefinitionStatus == Status::Ready &&
                    result.TransformStatus == Status::Ready && result.SplineStatus == Status::Ready &&
                    AreRequiredSplineProfilesResolved(result.DefinitionFacts, result.HasSpline);
            }
            Commit(native, context, key, std::move(result), definition);
        }

        /** Records only pending keys; spatial visits are never authoritative membership. */
        static bool VisitLocal(void* context, const MiniMapNative::GridEntityHandle& handle)
        {
            auto& visit = *static_cast<GridVisit*>(context);
            const auto key = Key(handle.Entity);
            if (visit.Owner.Pending.contains(key))
                visit.Keys.insert(key);
            return true;
        }

        /** Enumerates local pending priority around the exact requested rectangle, across all Z. */
        void CollectLocalKeys(const Native& native, std::unordered_set<std::uint64_t>& keys)
        {
            if (!HasRegion)
                return;
            const double radius = std::hypot(RequestedRegion.Width, RequestedRegion.Height) * 0.5;
            const double x = RequestedRegion.X + RequestedRegion.Width * 0.5;
            const double y = RequestedRegion.Y + RequestedRegion.Height * 0.5;
            if (radius > (std::numeric_limits<float>::max)())
                return; // Remote pending pass still reaches every entity.
            auto* grid = native.gridDiagnostic.getSubsystem(World);
            if (!grid)
                return;
            GridVisit visit{*this, keys};
            const MiniMapNative::GridDiagnosticApi::VisitorRef visitor{&State::VisitLocal, &visit};
            native.gridDiagnostic.forEachCellInRadius(grid, SDK::FVector{x, y, 0},
                std::nextafter(static_cast<float>(radius), INFINITY), visitor, false);
        }

        /** Acquires a stable key sequence; no map iterators survive native calls. */
        template<class Keys> bool AcquireKeys(const Native& native, const Keys& keys,
            std::unordered_set<const DefinitionOwner*>& attempted, const Context& context)
        {
            for (const auto key : keys)
            {
                if (Pending.contains(key))
                    Acquire(native, key, attempted);
                if (!IsContextCurrent(native, context))
                    return false;
            }
            return true;
        }

        /** Maps copied native acquisition outcomes without exposing native status values. */
        static ::Services::Map::Buildings::BuildingCandidateSourceStatus MapSourceStatus(Status status)
        {
            using SourceStatus = ::Services::Map::Buildings::BuildingCandidateSourceStatus;
            switch (status)
            {
            case Status::Ready:
                return SourceStatus::Complete;
            case Status::Unavailable:
                return SourceStatus::Unavailable;
            case Status::Invalid:
                return SourceStatus::Invalid;
            case Status::Unknown:
            case Status::Missing:
                return SourceStatus::Incomplete;
            }
            return SourceStatus::Unknown;
        }

        /** Maps definition profile evidence independently from curve and render readiness. */
        static ::Services::Map::Buildings::BuildingCandidateSourceStatus MapSplineProfileStatus(
            ::Services::Map::Buildings::SplineProfileAcquisitionStatus status)
        {
            using ProfileStatus = ::Services::Map::Buildings::SplineProfileAcquisitionStatus;
            using SourceStatus = ::Services::Map::Buildings::BuildingCandidateSourceStatus;
            switch (status)
            {
            case ProfileStatus::Complete:
            case ProfileStatus::NotApplicable:
                return SourceStatus::Complete;
            case ProfileStatus::Unavailable:
                return SourceStatus::Unavailable;
            case ProfileStatus::Invalid:
                return SourceStatus::Invalid;
            case ProfileStatus::Unsupported:
                return SourceStatus::Unsupported;
            case ProfileStatus::Unknown:
                return SourceStatus::Incomplete;
            }
            return SourceStatus::Unknown;
        }

        /** Positively recognizes the legacy renderer's exact non-spline Base Core identity. */
        static ::Services::Map::Buildings::BuildingCandidateSpritePass ClassifySpritePass(
            const Facts::Building& building)
        {
            using SpritePass = ::Services::Map::Buildings::BuildingCandidateSpritePass;
            if (!building.DefinitionFacts || building.DefinitionFacts->ClassStatus != Status::Ready ||
                building.DefinitionFacts->CanonicalClassPath.empty() ||
                building.SplineClassificationStatus != Status::Ready || building.HasSpline)
            {
                return SpritePass::Unknown;
            }
            constexpr std::string_view baseCoreClassPath =
                "/Game/Chimera/Buildings/BaseCore/BP_BaseCore.BP_BaseCore_C";
            if (building.DefinitionFacts->CanonicalClassPath == baseCoreClassPath)
                return SpritePass::SecondPass;
            return SpritePass::FirstPass;
        }

        /** Recognizes only the established StarRupture drone-rail BuildingID values. */
        static bool IsDroneRail(const Facts::Building& building)
        {
            if (!building.DefinitionFacts)
                return false;
            switch (static_cast<SDK::ECrBuildingID>(building.DefinitionFacts->BuildingId))
            {
            case SDK::ECrBuildingID::DroneRailT1:
            case SDK::ECrBuildingID::DroneRailT2:
            case SDK::ECrBuildingID::DroneRailT3:
            case SDK::ECrBuildingID::DroneRailT4:
            case SDK::ECrBuildingID::DroneRailT5:
                return true;
            default:
                return false;
            }
        }

        /** Classifies only spline occurrences with a ready acquired definition ID. */
        static SplinePolicyId ClassifySplinePolicyId(const Facts::Building& building)
        {
            if (building.SplineClassificationStatus != Status::Ready || !building.HasSpline ||
                !building.DefinitionFacts ||
                building.DefinitionFacts->ClassStatus != Status::Ready)
            {
                return SplinePolicyId::Unknown;
            }

            switch (static_cast<SDK::ECrBuildingID>(building.DefinitionFacts->BuildingId))
            {
            case SDK::ECrBuildingID::DroneRailT1:
            case SDK::ECrBuildingID::DroneRailT2:
            case SDK::ECrBuildingID::DroneRailT3:
            case SDK::ECrBuildingID::DroneRailT4:
            case SDK::ECrBuildingID::DroneRailT5:
                return SplinePolicyId::DroneRail;
            case SDK::ECrBuildingID::Zipline:
            case SDK::ECrBuildingID::Zipline_250:
            case SDK::ECrBuildingID::Zipline_320:
            case SDK::ECrBuildingID::Zipline_390:
            case SDK::ECrBuildingID::Zipline_460:
            case SDK::ECrBuildingID::Zipline_530:
            case SDK::ECrBuildingID::Zipline_600:
            case SDK::ECrBuildingID::Zipline_2x2:
            case SDK::ECrBuildingID::Zipline_2x2_250:
            case SDK::ECrBuildingID::Zipline_2x2_320:
            case SDK::ECrBuildingID::Zipline_2x2_390:
            case SDK::ECrBuildingID::Zipline_2x2_460:
            case SDK::ECrBuildingID::Zipline_2x2_530:
            case SDK::ECrBuildingID::Zipline_2x2_600:
            case SDK::ECrBuildingID::ZipLine_OLD:
            case SDK::ECrBuildingID::ZiplineSupport:
            case SDK::ECrBuildingID::ZiplineSupport_2x2:
            case SDK::ECrBuildingID::ZipRail:
                return SplinePolicyId::Zipline;
            case SDK::ECrBuildingID::Walkway:
            case SDK::ECrBuildingID::RailingWalkway:
                return SplinePolicyId::Walkway;
            default:
                return SplinePolicyId::Unknown;
            }
        }

        /** Combines required source facts while retaining the strongest unresolved evidence. */
        static ::Services::Map::Buildings::BuildingCandidateSourceStatus CombineSourceStatus(
            ::Services::Map::Buildings::BuildingCandidateSourceStatus first,
            ::Services::Map::Buildings::BuildingCandidateSourceStatus second)
        {
            using SourceStatus = ::Services::Map::Buildings::BuildingCandidateSourceStatus;
            if (first == SourceStatus::Invalid || second == SourceStatus::Invalid)
                return SourceStatus::Invalid;
            if (first == SourceStatus::Unsupported || second == SourceStatus::Unsupported)
                return SourceStatus::Unsupported;
            if (first == SourceStatus::Unavailable || second == SourceStatus::Unavailable)
                return SourceStatus::Unavailable;
            if (first != SourceStatus::Complete || second != SourceStatus::Complete)
                return SourceStatus::Incomplete;
            return SourceStatus::Complete;
        }

        /** Maps established native position-curve modes and retains unknown modes explicitly. */
        static ::Services::Map::Buildings::Visuals::KeyedSplineInterpolation MapInterpolationMode(
            std::uint8_t mode)
        {
            using Interpolation = ::Services::Map::Buildings::Visuals::KeyedSplineInterpolation;
            switch (static_cast<SDK::EInterpCurveMode>(mode))
            {
            case SDK::EInterpCurveMode::CIM_Linear:
                return Interpolation::Linear;
            case SDK::EInterpCurveMode::CIM_Constant:
                return Interpolation::Constant;
            case SDK::EInterpCurveMode::CIM_CurveAuto:
            case SDK::EInterpCurveMode::CIM_CurveUser:
            case SDK::EInterpCurveMode::CIM_CurveBreak:
            case SDK::EInterpCurveMode::CIM_CurveAutoClamped:
                return Interpolation::Cubic;
            }
            return Interpolation::Unsupported;
        }

        /** Copies initialized position keys without applying placement to their world-space values. */
        static void CopyInitializedSplineKeys(
            const Facts::Spline& nativeSpline,
            ::Services::Map::Buildings::BuildingCandidateSplineSource& spline)
        {
            spline.isLooped = nativeSpline.Position.IsLooped;
            spline.points.reserve(nativeSpline.Position.Points.size());
            for (const Facts::CurvePoint<Facts::Point>& point : nativeSpline.Position.Points)
            {
                spline.points.push_back({
                    point.Key,
                    point.ValueAtKey,
                    point.ArriveTangent,
                    point.LeaveTangent,
                    MapInterpolationMode(point.Mode)});
            }
            spline.hasKeyedCurve = spline.points.size() >= 2;
        }

        /** Converts only the established two-point fallback to its implicit [0, 1] keyed form. */
        static ::Services::Map::Buildings::BuildingCandidateSourceStatus CopyFallbackSplineKeys(
            const Facts::SplineData& data,
            ::Services::Map::Buildings::BuildingCandidateSplineSource& spline)
        {
            using SourceStatus = ::Services::Map::Buildings::BuildingCandidateSourceStatus;
            if (data.Points.size() != 2)
            {
                spline.unkeyedPoints.reserve(data.Points.size());
                for (const Facts::SplinePoint& point : data.Points)
                    spline.unkeyedPoints.push_back({point.Position, point.Tangent});
                return data.Points.size() > 2 ? SourceStatus::Unsupported : SourceStatus::Incomplete;
            }

            const Facts::SplinePoint& first = data.Points[0];
            const Facts::SplinePoint& last = data.Points[1];
            spline.points.reserve(2);
            spline.points.push_back({0.0, first.Position, {}, first.Tangent,
                ::Services::Map::Buildings::Visuals::KeyedSplineInterpolation::Cubic});
            spline.points.push_back({1.0, last.Position, last.Tangent, {},
                ::Services::Map::Buildings::Visuals::KeyedSplineInterpolation::Cubic});
            spline.hasKeyedCurve = true;
            return SourceStatus::Complete;
        }

        /** Copies native spline values and provenance into the neutral publication record. */
        static ::Services::Map::Buildings::BuildingCandidateSourceStatus CopySplineSource(
            const Facts::Building& building,
            ::Services::Map::Buildings::BuildingCandidateSplineSource& spline)
        {
            using Provenance = ::Services::Map::Buildings::BuildingCandidateSplineProvenance;
            using CoordinateSpace = ::Services::Map::Buildings::BuildingCandidateSplineCoordinateSpace;
            using SourceStatus = ::Services::Map::Buildings::BuildingCandidateSourceStatus;
            if (!building.SplineFacts)
            {
                return building.SplineStatus == Status::Ready
                    ? SourceStatus::Incomplete
                    : MapSourceStatus(building.SplineStatus);
            }

            const Facts::Spline& nativeSpline = *building.SplineFacts;
            spline.coordinateSpace = CoordinateSpace::WorldSpace;
            // Provenance describes the value source selected below, not a fallback attempt.
            if (nativeSpline.CurvesInitialized)
            {
                spline.provenance = Provenance::InitializedCurve;
            }
            else if (nativeSpline.Provenance == Facts::SplineProvenance::Payload &&
                nativeSpline.PayloadData)
            {
                spline.provenance = Provenance::PayloadFallback;
            }
            else
            {
                spline.provenance = Provenance::FragmentPoints;
            }
            SourceStatus sourceStatus = MapSourceStatus(nativeSpline.SourceStatus);
            if (nativeSpline.CurvesInitialized)
            {
                CopyInitializedSplineKeys(nativeSpline, spline);
                if (!spline.hasKeyedCurve)
                    sourceStatus = CombineSourceStatus(sourceStatus, SourceStatus::Incomplete);
            }
            else if (nativeSpline.Provenance == Facts::SplineProvenance::Payload && nativeSpline.PayloadData)
            {
                sourceStatus = CombineSourceStatus(sourceStatus,
                    CopyFallbackSplineKeys(*nativeSpline.PayloadData, spline));
            }
            else
            {
                sourceStatus = CombineSourceStatus(sourceStatus,
                    CopyFallbackSplineKeys(nativeSpline.FragmentData, spline));
                if (nativeSpline.Provenance == Facts::SplineProvenance::Payload)
                    sourceStatus = CombineSourceStatus(sourceStatus, SourceStatus::Incomplete);
            }
            return sourceStatus;
        }

        /** Copies known neutral source facts while preserving classification and acquisition gaps. */
        static ::Services::Map::Buildings::BuildingCandidateSourceRecord BuildCandidateSourceRecord(
            const Facts::Building& building)
        {
            using SourceKind = ::Services::Map::Buildings::BuildingCandidateSourceKind;
            using SourceStatus = ::Services::Map::Buildings::BuildingCandidateSourceStatus;
            using CandidateSource = ::Services::Map::Buildings::BuildingCandidateSourceRecord;
            CandidateSource source;
            const auto definitionStatus = building.DefinitionFacts
                ? MapSourceStatus(building.DefinitionFacts->ClassStatus)
                : SourceStatus::Incomplete;
            const auto transformStatus = MapSourceStatus(building.TransformStatus);
            if (building.DefinitionFacts && building.DefinitionFacts->ClassStatus == Status::Ready)
                source.canonicalClassPath = building.DefinitionFacts->CanonicalClassPath;
            if (building.TransformStatus == Status::Ready)
            {
                source.hasPlacement = true;
                source.placement = {building.Position, building.SignedScale, building.Rotation};
            }
            if (building.SplineClassificationStatus != Status::Ready)
            {
                source.status = SourceStatus::Unknown;
                return source;
            }

            source.kind = building.HasSpline ? SourceKind::Spline : SourceKind::Sprite;
            if (source.kind == SourceKind::Sprite)
            {
                source.splineProfileStatus =
                    ::Services::Map::Buildings::SplineProfileAcquisitionStatus::NotApplicable;
                source.spritePass = ClassifySpritePass(building);
                source.status = CombineSourceStatus(definitionStatus, transformStatus);
            }
            else
            {
                source.splinePolicyIndex = static_cast<std::uint16_t>(ClassifySplinePolicyId(building));
                if (building.DefinitionFacts)
                {
                    source.splineProfiles = building.DefinitionFacts->SplineProfiles;
                    source.splineProfileStatus = building.DefinitionFacts->SplineProfileStatus;
                }
                source.status = CombineSourceStatus(
                    CombineSourceStatus(definitionStatus, transformStatus),
                    CombineSourceStatus(CopySplineSource(building, source.spline),
                        MapSplineProfileStatus(source.splineProfileStatus)));
            }
            return source;
        }

        /** Builds candidate and source vectors in one pass over the exact same frozen facts. */
        static void BuildCandidateRecords(
            const Facts& facts,
            bool includeDroneRails,
            std::vector<::Services::Map::Buildings::BuildingCandidateOccurrence>& occurrences,
            std::vector<::Services::Map::Buildings::BuildingCandidateSourceRecord>& sourceRecords,
            std::size_t& excludedInteriorOnlyCount,
            std::size_t& excludedDroneRailCount)
        {
            using CandidateOccurrence = ::Services::Map::Buildings::BuildingCandidateOccurrence;
            occurrences.reserve(facts.Buildings.size());
            sourceRecords.reserve(facts.Buildings.size());
            excludedInteriorOnlyCount = 0;
            excludedDroneRailCount = 0;
            for (const Facts::Building& building : facts.Buildings)
            {
                if (building.DefinitionFacts &&
                    building.DefinitionFacts->InteriorStatus == Status::Ready &&
                    building.DefinitionFacts->InteriorOnly)
                {
                    ++excludedInteriorOnlyCount;
                    continue;
                }
                if (!includeDroneRails && IsDroneRail(building))
                {
                    ++excludedDroneRailCount;
                    continue;
                }
                CandidateOccurrence occurrence;
                occurrence.hasMapPosition = BuildingCollectorV2::HasValidMapPosition(building);
                if (occurrence.hasMapPosition)
                {
                    occurrence.xMapUnits = building.Position[0];
                    occurrence.yMapUnits = building.Position[1];
                }
                occurrence.nativeFactsComplete = building.Complete;
                occurrence.isPendingOrUnresolved = !building.Complete;
                occurrence.contentSignature = BuildingCollectorV2::ComputeOccurrenceSignature(building);
                occurrences.push_back(std::move(occurrence));
                sourceRecords.push_back(BuildCandidateSourceRecord(building));
            }
        }

        /** Produces neutral candidate occurrences and explicit coverage evidence from one fact owner. */
        std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>
        CreateCandidatePublication(
            const std::shared_ptr<const Facts>& facts,
            std::shared_ptr<std::atomic<bool>>& eligibility)
        {
            using CandidateOccurrence = ::Services::Map::Buildings::BuildingCandidateOccurrence;
            using CandidateSourceRecord = ::Services::Map::Buildings::BuildingCandidateSourceRecord;
            using CandidateEvidence = ::Services::Map::Buildings::BuildingCandidateEvidence;
            using CandidateRegion = ::Services::Map::Buildings::BuildingCollectionRegion;
            std::vector<CandidateOccurrence> occurrences;
            std::vector<CandidateSourceRecord> sourceRecords;
            std::size_t excludedInteriorOnlyCount = 0;
            std::size_t excludedDroneRailCount = 0;
            BuildCandidateRecords(
                *facts,
                IncludeDroneRails,
                occurrences,
                sourceRecords,
                excludedInteriorOnlyCount,
                excludedDroneRailCount);
            CandidateEvidence evidence;
            for (const CandidateOccurrence& occurrence : occurrences)
            {
                evidence.hasPendingOrUnresolvedOccurrences |= occurrence.isPendingOrUnresolved;
            }
            evidence.membershipEnumerationComplete = facts->MembershipComplete;
            evidence.nativeAcquisitionComplete = facts->AcquisitionComplete &&
                !evidence.hasPendingOrUnresolvedOccurrences;
            const bool allAuthoritativeOccurrencesAccounted =
                occurrences.size() + excludedInteriorOnlyCount + excludedDroneRailCount ==
                facts->Buildings.size();
            // Only positively classified policy exclusions are omitted from candidates.
            evidence.candidateCoverageComplete = facts->HasRequestedRegion &&
                facts->MembershipComplete && allAuthoritativeOccurrencesAccounted;
            const CandidateRegion requestedCoverage{
                facts->RequestedRegion.X,
                facts->RequestedRegion.Y,
                facts->RequestedRegion.Width,
                facts->RequestedRegion.Height};
            eligibility = std::make_shared<std::atomic<bool>>(true);
            return std::make_shared<::Services::Map::Buildings::BuildingCandidatePublication>(
                std::move(occurrences),
                std::move(sourceRecords),
                evidence,
                true,
                requestedCoverage,
                evidence.candidateCoverageComplete,
                requestedCoverage,
                eligibility);
        }

        /** Freezes all occurrences only when relevant acquisition/request evidence changed. */
        void Publish()
        {
            if (!Dirty)
                return;
            auto facts = std::make_shared<Facts>();
            facts->MembershipComplete = MembershipComplete;
            facts->AcquisitionComplete = MembershipComplete && Pending.empty();
            facts->HasRequestedRegion = HasRegion;
            facts->RequestedRegion = RequestedRegion;
            facts->Buildings.reserve(Entities.size());
            for (const auto& item : Entities)
                facts->Buildings.push_back(item.second.Data);
            std::sort(facts->Buildings.begin(), facts->Buildings.end(), [](const auto& left, const auto& right)
            {
                return Key({left.Index, left.Serial}) < Key({right.Index, right.Serial});
            });
            std::shared_ptr<std::atomic<bool>> eligibility;
            std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication> candidates;
            if (facts->HasRequestedRegion)
            {
                candidates = CreateCandidatePublication(facts, eligibility);
            }
            Published = std::move(facts);
            PublishedCandidates = std::move(candidates);
            PublicationEligibility = std::move(eligibility);
            Dirty = false;
        }

        /** Alternates local-progress publication with unrestricted remote pending acquisition. */
        void AdvancePending(const Native& native)
        {
            if (Pending.empty())
            {
                RemoteTurn = false;
                Publish();
                return;
            }
            const auto context = CaptureContext();
            std::unordered_set<std::uint64_t> local;
            CollectLocalKeys(native, local);
            if (!IsContextCurrent(native, context))
                return;
            std::unordered_set<const DefinitionOwner*> attempted;
            if (!AcquireKeys(native, local, attempted, context))
                return;
            if (!Pending.empty() && Dirty && !RemoteTurn)
            {
                Publish();
                RemoteTurn = true;
                return;
            }
            RemoteTurn = false;
            const std::vector<std::uint64_t> remote(Pending.begin(), Pending.end());
            if (AcquireKeys(native, remote, attempted, context))
                Publish();
        }
    };

    /** Hashes the exact approved class-path and ready transform bytes for one occurrence. */
    std::uint64_t BuildingCollectorV2::ComputeOccurrenceSignature(
        const BuildingAcquiredFacts::Building& building) noexcept
    {
        std::uint64_t hash = 14695981039346656037ULL;
        std::string_view classPath;
        if (building.DefinitionFacts != nullptr)
            classPath = building.DefinitionFacts->CanonicalClassPath;
        AppendHashInteger(hash, static_cast<std::uint64_t>(classPath.size()));
        for (unsigned char value : classPath)
            AppendHashByte(hash, value);
        if (building.TransformStatus == BuildingAcquiredFacts::Status::Ready)
        {
            for (double value : building.Position)
                AppendHashDouble(hash, value);
            AppendCanonicalQuaternion(hash, building.Rotation);
        }
        return hash;
    }

    /** Canonicalizes quaternion sign in W/X/Y/Z order without renormalizing the stored value. */
    void BuildingCollectorV2::AppendCanonicalQuaternion(
        std::uint64_t& hash,
        const BuildingAcquiredFacts::Quaternion& rotation) noexcept
    {
        bool negate = false;
        const std::size_t signCheckOrder[] = {3, 0, 1, 2};
        for (std::size_t componentIndex : signCheckOrder)
        {
            const double value = rotation[componentIndex];
            if (value != 0.0)
            {
                negate = std::signbit(value);
                break;
            }
        }
        for (double value : rotation)
            AppendHashDouble(hash, negate ? -value : value);
    }

    /** Applies the fixed FNV-1a byte step used by the placement-content signature. */
    void BuildingCollectorV2::AppendHashByte(std::uint64_t& hash, std::uint8_t value) noexcept
    {
        hash = (hash ^ value) * 1099511628211ULL;
    }

    /** Serializes one integer using the established little-endian hash input. */
    void BuildingCollectorV2::AppendHashInteger(std::uint64_t& hash, std::uint64_t value) noexcept
    {
        for (std::size_t byteIndex = 0; byteIndex < sizeof(value); ++byteIndex)
        {
            AppendHashByte(hash, static_cast<std::uint8_t>(value & 0xff));
            value >>= 8;
        }
    }

    /** Hashes one floating value after canonicalizing either sign of zero. */
    void BuildingCollectorV2::AppendHashDouble(std::uint64_t& hash, double value) noexcept
    {
        if (value == 0.0)
            value = 0.0;
        AppendHashInteger(hash, std::bit_cast<std::uint64_t>(value));
    }

    /** Exposes map position only when native transform facts are ready and finite. */
    bool BuildingCollectorV2::HasValidMapPosition(const BuildingAcquiredFacts::Building& building) noexcept
    {
        return building.TransformStatus == BuildingAcquiredFacts::Status::Ready &&
            std::isfinite(building.Position[0]) && std::isfinite(building.Position[1]);
    }

    /** Creates inert CPU state; no native resource is acquired here. */
    BuildingCollectorV2::BuildingCollectorV2() : state_(std::make_unique<State>()) {}

    /** Explicit lifecycle is mandatory; native retirement is never initiated from this destructor. */
    BuildingCollectorV2::~BuildingCollectorV2()
    {
        state_->Invalidate();
        assert(!state_->QueryConstructed && !state_->Types && state_->Definitions.empty());
    }

    /** Initializes independent metadata roots, with allocation failure left retryable. */
    bool BuildingCollectorV2::InitializeResources()
    {
        const auto* native = MiniMapNative::Get();
        auto* objects = Services::TryObjects();
        if (!native || !native->player.isGameThread() || !objects || !objects->Ready())
            return false;
        if (state_->Types)
            return true;
        auto types = std::make_shared<State::Metadata>();
        if (!types->Initialize() || !state_->DefinitionAcquisition.Initialize())
            return false;
        state_->Types = std::move(types);
        return true;
    }

    /** Accepts exact representable half-open coverage; it controls priority, never exclusion. */
    bool BuildingCollectorV2::SetAcquisitionRegion(const Facts::Region& region) noexcept
    {
        const double right = region.X + region.Width;
        const double bottom = region.Y + region.Height;
        if (!std::isfinite(region.X) || !std::isfinite(region.Y) || !std::isfinite(region.Width) ||
            !std::isfinite(region.Height) || region.Width <= 0 || region.Height <= 0 ||
            !std::isfinite(right) || !std::isfinite(bottom) || right <= region.X || bottom <= region.Y ||
            !std::isfinite(std::hypot(region.Width, region.Height)))
            return false;
        if (!state_->HasRegion || !(state_->RequestedRegion == region))
        {
            state_->RequestedRegion = region;
            state_->HasRegion = true;
            state_->Invalidate();
        }
        return true;
    }

    /** Maps service units directly to native centimetres and reuses the accepted-request transaction. */
    bool BuildingCollectorV2::SetAcquisitionRegion(
        const ::Services::Map::Buildings::BuildingCollectionRegion& region) noexcept
    {
        const Facts::Region nativeRegion{
            region.xMapUnits,
            region.yMapUnits,
            region.widthMapUnits,
            region.heightMapUnits};
        return SetAcquisitionRegion(nativeRegion);
    }

    /** Invalidates only the candidate selection when the caller changes rail visibility policy. */
    void BuildingCollectorV2::SetRenderDroneRails(bool enabled) noexcept
    {
        if (state_->IncludeDroneRails == enabled)
            return;
        state_->IncludeDroneRails = enabled;
        state_->Invalidate();
    }

    /** Returns only the current candidate owner; stale publications never fall back to another observation path. */
    std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>
    BuildingCollectorV2::AcquireCandidatePublication() const
    {
        if (state_->Dirty || state_->PublishedCandidates == nullptr ||
            !state_->PublishedCandidates->IsCurrent())
        {
            return {};
        }
        return state_->PublishedCandidates;
    }

    /** Advances native acquisition using MiniMap's established gameplay world. */
    void BuildingCollectorV2::Update()
    {
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.isGameThread())
            return;
        SDK::UWorld* world = MiniMapMap::GetWorld();
        if (!world)
            return;
        try
        {
            if (!state_->Types || !state_->EnsureSession(*native, world))
                return;
            const auto context = state_->CaptureContext();
            state_->ReconcileIfDue(*native);
            if (state_->IsContextCurrent(*native, context))
                state_->AdvancePending(*native);
        }
        catch (const std::bad_alloc&)
        {
            // Keep owned entries/pending roots; never infer absence from failed bookkeeping.
            state_->Invalidate();
        }
    }

    /** Ends the session while native services are valid, withdrawing old eligibility. */
    void BuildingCollectorV2::Reset()
    {
        state_->ClearSession(true);
    }

    /** Releases query/definition roots before per-instance acquisition/conversion metadata. */
    void BuildingCollectorV2::ShutdownResources()
    {
        Reset();
        state_->DefinitionAcquisition.Shutdown();
        state_->Types.reset();
    }

    /** Returns retained CPU facts even if later acquisition has revoked their currentness. */
    std::shared_ptr<const Facts> BuildingCollectorV2::GetFacts() const noexcept
    {
        return state_->Published;
    }

    /** Requires the currently exposed immutable owner and unchanged acquisition evidence. */
    bool BuildingCollectorV2::IsCurrent(const std::shared_ptr<const Facts>& facts) const noexcept
    {
        return facts && facts == state_->Published && !state_->Dirty;
    }
}
#endif
