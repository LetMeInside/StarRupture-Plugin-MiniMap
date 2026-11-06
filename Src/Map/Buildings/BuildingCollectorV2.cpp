// Independently acquires StarRupture native building facts. All ABI-specific access
// stays here; no rendering, legacy collector, service interface or runtime caller is used.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollectorV2.h"
#include "BuildingDefinitionAcquisition.h"
#include "BuildingSplineSource.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/Chimera_classes.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
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
            std::uint64_t HostGeneration;
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
        std::uint64_t WorldGeneration = 0;
        std::uint64_t ScanEpoch = 0;
        Clock::time_point NextMembership{};
        bool MembershipComplete = false;
        bool Dirty = true;
        bool RemoteTurn = false;
        bool HasRegion = false;
        Facts::Region RequestedRegion;
        std::shared_ptr<const Facts> Published;

        /** Packs the full session-private Index/Serial pair; never a content fingerprint. */
        static std::uint64_t Key(EntityId id) noexcept
        {
            return std::uint64_t(std::uint32_t(id.Index)) |
                (std::uint64_t(std::uint32_t(id.SerialNumber)) << 32);
        }

        /** Captures logical source lifetime before native operations. */
        Context CaptureContext() const noexcept
        {
            return {World, Manager, Generation, WorldGeneration};
        }

        /** Marks exposed bytes historical immediately; their retained CPU lifetime is unaffected. */
        void Invalidate() noexcept
        {
            Dirty = true;
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
                Generation != context.Session || WorldGeneration != context.HostGeneration)
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
            WorldGeneration = 0;
            Pending.clear();
            Entities.clear();
            Definitions.clear();
            ScanEpoch = 0;
            NextMembership = {};
            MembershipComplete = false;
            RemoteTurn = false;
            Published.reset();
            if (clearRequest)
            {
                HasRegion = false;
                RequestedRegion = {};
            }
        }

        /** Establishes a query for the supplied live world, preserving exact request across automatic rebinding. */
        bool EnsureSession(const Native& native, SDK::UWorld* world, std::uint64_t hostGeneration)
        {
            const auto* storage = ManagerStorage(native, world);
            const auto* manager = ReadManager(storage);
            if (World && (World != world || WorldGeneration != hostGeneration || Manager != manager))
                ClearSession(false);
            if (!world || !manager || !Types)
                return false;
            if (QueryConstructed)
                return true;
            World = world;
            Manager = manager;
            WorldGeneration = hostGeneration;
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

        /** Attempts shared definition conversion once per callback, without entry references across reflection. */
        void CopyDefinition(const Native& native, const Context& context,
            const std::shared_ptr<DefinitionOwner>& definition,
            std::unordered_set<const DefinitionOwner*>& attempted)
        {
            if (definition->Facts && BuildingDefinitionAcquisition::IsComplete(*definition->Facts))
                return;
            if (!attempted.insert(definition.get()).second)
                return;
            auto facts = DefinitionAcquisition.Copy(definition);
            if (IsContextCurrent(native, context))
                definition->Facts = std::move(facts);
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
                    CopyDefinition(native, context, definition, attempted);
                if (!IsContextCurrent(native, context))
                    return;
                result.DefinitionFacts = definition ? definition->Facts : nullptr;
                result.DefinitionStatus = DefinitionStatus(result.DefinitionFacts);
                result.Complete = result.DefinitionStatus == Status::Ready &&
                    result.TransformStatus == Status::Ready && result.SplineStatus == Status::Ready;
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

        /** Freezes all occurrences only when relevant acquisition/request evidence changed. */
        void Publish()
        {
            if (!Dirty)
                return;
            auto facts = std::make_shared<Facts>();
            facts->SessionGeneration = Generation;
            facts->WorldGeneration = WorldGeneration;
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
            Published = std::move(facts);
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

    /** Creates inert CPU state; no native resource is acquired here. */
    BuildingCollectorV2::BuildingCollectorV2() : state_(std::make_unique<State>()) {}

    /** Explicit lifecycle is mandatory; native retirement is never initiated from this destructor. */
    BuildingCollectorV2::~BuildingCollectorV2()
    {
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

    /** Progresses the sole native query and pending work without registering any caller. */
    void BuildingCollectorV2::AdvanceAcquisition(SDK::UWorld* world, std::uint64_t worldGeneration, bool gameplayReady)
    {
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.isGameThread())
            return;
        if (!gameplayReady || !world || !worldGeneration)
        {
            if (state_->World)
                state_->ClearSession(false);
            return;
        }
        try
        {
            if (!state_->Types || !state_->EnsureSession(*native, world, worldGeneration))
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
