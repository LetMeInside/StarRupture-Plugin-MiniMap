// Acquires StarRupture building facts and distributes retained neutral values to requested Sections.
// All ABI-specific access stays here; Section publications contain no native ownership.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingMapLoadingTrace.h"
#include "DiagnosticExports.h"
#include "plugin_helpers.h"
#include "BuildingCollectorV2.h"
#include "Native/MassLifecycle.h"
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
#include <SDK/MassEntity_structs.hpp>
#include "Native/ReadbackUEConfiguration.h"
#include <MassExecutionContext.h>
#include <MassArchetypeTypes.h>
#include <algorithm>
#include <cassert>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
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

    static_assert(sizeof(SDK::FMassEntityQuery) == 0x350,
        "BuildingCollectorV2 fixed query storage must match the generated Client SDK query size");
    static_assert(alignof(SDK::FMassEntityQuery) == 0x10,
        "BuildingCollectorV2 fixed query storage must match the generated Client SDK query alignment");
    static_assert(sizeof(FMassExecutionContext) == 0x4E0);
    static_assert(alignof(FMassExecutionContext) == 8);
    static_assert(sizeof(FMassExecuteFunction) == 0x30);
    static_assert(sizeof(::FMassEntityHandle) == sizeof(SDK::FMassEntityHandle));
    static_assert(alignof(::FMassEntityHandle) == alignof(SDK::FMassEntityHandle));
    static_assert(sizeof(SDK::FTransformFragment) == 0x60);
    static_assert(sizeof(SDK::FAuSplineConnectionFragment) == 0xE0);
    static_assert(sizeof(SDK::FAuAPMassFragment) == 0x50);
    static_assert(sizeof(SDK::FCrBuildingParameters) == 0x18);
    static_assert(sizeof(SDK::FAuAPPayloadDataHandle) == 0x20);

    /** Sole mutable acquisition owner; publications are frozen CPU projections only. */
    struct BuildingCollectorV2::State
    {
        using Clock = std::chrono::steady_clock;
        static constexpr auto MutationQuietPeriod = std::chrono::milliseconds(100);
        static constexpr auto PeriodicFallbackInterval = std::chrono::seconds(10);

        /** Classifies a collector scan for bounded success/failure diagnostics. */
        enum class AcquisitionReason
        {
            Immediate,
            Mutation,
            Fallback
        };

        /** Owns one native-constructed Shipping context for one synchronous traversal. */
        struct MassExecutionContextOwner
        {
            const Native& Api;
            alignas(FMassExecutionContext) std::byte Storage[sizeof(FMassExecutionContext)];
            bool Constructed = false;

            /** Native-constructs a non-flushing context for the current query manager. */
            MassExecutionContextOwner(const Native& api, const void* manager)
                : Api(api)
            {
                Api.mass.executionContextConstruct(
                    reinterpret_cast<FMassExecutionContext*>(Storage),
                    *reinterpret_cast<FMassEntityManager*>(const_cast<void*>(manager)),
                    0.0f,
                    false);
                Constructed = true;
            }

            MassExecutionContextOwner(const MassExecutionContextOwner&) = delete;
            MassExecutionContextOwner& operator=(const MassExecutionContextOwner&) = delete;

            /** Native-destroys the context once after callback and view lifetimes end. */
            ~MassExecutionContextOwner()
            {
                if (Constructed)
                    Api.mass.executionContextDestruct(Get());
            }

            /** Returns the genuine context pointer after native construction. */
            FMassExecutionContext* Get() noexcept
            {
                return std::launder(reinterpret_cast<FMassExecutionContext*>(Storage));
            }
        };

        /** Holds a native-owned payload copy at a stable address until native destruction. */
        struct PayloadHandleOwner
        {
            const Native& Api;
            alignas(SDK::FAuAPPayloadDataHandle)
                std::byte Storage[sizeof(SDK::FAuAPPayloadDataHandle)];

            /** Native-copy-constructs an owning handle from one borrowed fragment value. */
            PayloadHandleOwner(const Native& api, const SDK::FAuAPPayloadDataHandle& source)
                : Api(api)
            {
                Api.splinePayload.payloadHandleCopyConstruct(
                    reinterpret_cast<SDK::FAuAPPayloadDataHandle*>(Storage),
                    &source);
            }

            PayloadHandleOwner(const PayloadHandleOwner&) = delete;
            PayloadHandleOwner& operator=(const PayloadHandleOwner&) = delete;

            /** Releases the native shared ownership exactly once. */
            ~PayloadHandleOwner()
            {
                Api.splinePayload.payloadHandleDestruct(Get());
            }

            /** Returns the payload handle after native copy construction. */
            SDK::FAuAPPayloadDataHandle* Get() noexcept
            {
                return std::launder(reinterpret_cast<SDK::FAuAPPayloadDataHandle*>(Storage));
            }
        };

        /** Borrows chunk data only for the duration of its native execution callback. */
        struct MassChunkViews
        {
            TConstArrayView<::FMassEntityHandle> Entities;
            TConstArrayView<SDK::FTransformFragment> Transforms;
            TConstArrayView<SDK::FAuSplineConnectionFragment> SplineConnections;
            TConstArrayView<SDK::FAuAPMassFragment> ActorPlacement;
            const SDK::FCrBuildingParameters* BuildingParameters = nullptr;
            bool HasTransforms = false;
            bool HasSplineConnections = false;
            bool HasActorPlacement = false;

            /** Copies one current chunk identity into the SDK's established handle type. */
            SDK::FMassEntityHandle EntityAt(int32 index) const noexcept
            {
                SDK::FMassEntityHandle entity{};
                entity.Index = Entities[index].Index;
                entity.SerialNumber = Entities[index].SerialNumber;
                return entity;
            }
        };

        /** Owns admitted numerical facts and native references copied during one scan callback. */
        struct OwnedScanOccurrence
        {
            Facts::Building Data;
            std::shared_ptr<DefinitionOwner> Placement;
            std::unique_ptr<PayloadHandleOwner> Payload;
            std::uint64_t PreliminaryHashContribution = 0;
        };

        /** Holds the order-independent multiset aggregate for one in-progress regional scan. */
        struct AcquisitionHashAggregate
        {
            std::uint64_t Sum = 0;
            std::uint64_t Xor = 0;
            std::uint64_t Count = 0;

            /** Adds one mixed occurrence contribution while preserving duplicate multiplicity. */
            void Add(std::uint64_t contribution) noexcept
            {
                Sum += contribution;
                Xor ^= contribution;
                ++Count;
            }

            /** Replaces preliminary content without changing the occurrence count. */
            void Replace(std::uint64_t previous, std::uint64_t completed) noexcept
            {
                Sum -= previous;
                Sum += completed;
                Xor ^= previous;
                Xor ^= completed;
            }
        };

        /** Appends a fixed-width byte through the established deterministic byte step. */
        static void AppendAcquisitionByte(std::uint64_t& hash, std::uint8_t value) noexcept
        {
            BuildingCollectorV2::AppendHashByte(hash, value);
        }

        /** Appends a fixed-width integer in the established little-endian order. */
        static void AppendAcquisitionInteger(std::uint64_t& hash, std::uint64_t value) noexcept
        {
            BuildingCollectorV2::AppendHashInteger(hash, value);
        }

        /** Canonicalizes signed zero and NaN payloads before hashing one floating value. */
        static void AppendAcquisitionDouble(std::uint64_t& hash, double value) noexcept
        {
            if (value == 0.0)
                value = 0.0;
            if (std::isnan(value))
            {
                AppendAcquisitionInteger(hash, 0x7ff8000000000000ULL);
                return;
            }
            AppendAcquisitionInteger(hash, std::bit_cast<std::uint64_t>(value));
        }

        /** Appends owned text with a length prefix and its original byte sequence. */
        static void AppendAcquisitionString(std::uint64_t& hash, const std::string& value) noexcept
        {
            AppendAcquisitionInteger(hash, value.size());
            for (unsigned char byte : value)
                AppendAcquisitionByte(hash, byte);
        }

        /** Appends an ordered fixed-size array of acquired floating values. */
        template<std::size_t Size>
        static void AppendAcquisitionArray(std::uint64_t& hash, const std::array<double, Size>& values) noexcept
        {
            for (double value : values)
                AppendAcquisitionDouble(hash, value);
        }

        /** Appends one enum value without hashing its object representation. */
        template<class Enum>
        static void AppendAcquisitionEnum(std::uint64_t& hash, Enum value) noexcept
        {
            AppendAcquisitionByte(hash, static_cast<std::uint8_t>(value));
        }

        /** Appends a copied spline value whether it is scalar, point, or quaternion data. */
        static void AppendSplineValue(std::uint64_t& hash, double value) noexcept
        {
            AppendAcquisitionDouble(hash, value);
        }

        /** Appends a copied point or quaternion in its declared component order. */
        template<std::size_t Size>
        static void AppendSplineValue(std::uint64_t& hash, const std::array<double, Size>& value) noexcept
        {
            AppendAcquisitionArray(hash, value);
        }

        /** Appends one pointer-free authored spline channel, preserving point order and loop metadata. */
        template<class Value>
        static void AppendAcquisitionCurve(std::uint64_t& hash, const Facts::Curve<Value>& curve) noexcept
        {
            AppendAcquisitionInteger(hash, curve.Points.size());
            for (const auto& point : curve.Points)
            {
                AppendAcquisitionDouble(hash, point.Key);
                AppendSplineValue(hash, point.ValueAtKey);
                AppendSplineValue(hash, point.ArriveTangent);
                AppendSplineValue(hash, point.LeaveTangent);
                AppendAcquisitionByte(hash, point.Mode);
            }
            AppendAcquisitionByte(hash, curve.IsLooped ? 1 : 0);
            AppendAcquisitionDouble(hash, curve.LoopKeyOffset);
        }

        /** Appends one copied fragment or payload spline source and its ordered points. */
        static void AppendAcquisitionSplineData(std::uint64_t& hash, const Facts::SplineData& data) noexcept
        {
            AppendAcquisitionInteger(hash, data.Points.size());
            for (const auto& point : data.Points)
            {
                AppendAcquisitionByte(hash, point.Index);
                AppendAcquisitionArray(hash, point.Position);
                AppendAcquisitionArray(hash, point.Tangent);
            }
            AppendAcquisitionInteger(hash, static_cast<std::uint32_t>(data.NativePointCount));
            AppendAcquisitionEnum(hash, data.ValueStatus);
            AppendAcquisitionArray(hash, data.LocationWS);
            AppendAcquisitionArray(hash, data.RotationDegreesWS);
            AppendAcquisitionDouble(hash, data.RecordedLength);
        }

        /** Appends immutable authored profile alternatives and preserves absence and order. */
        static void AppendAcquisitionProfiles(std::uint64_t& hash,
            const std::shared_ptr<const ::Services::Map::Buildings::SplineProfileSet>& profiles) noexcept
        {
            AppendAcquisitionByte(hash, profiles ? 1 : 0);
            if (!profiles)
                return;
            AppendAcquisitionInteger(hash, profiles->Alternatives.size());
            for (const auto& alternative : profiles->Alternatives)
            {
                AppendAcquisitionEnum(hash, alternative.Status);
                AppendAcquisitionByte(hash, alternative.HasBounds ? 1 : 0);
                if (alternative.HasBounds)
                {
                    AppendAcquisitionArray(hash, alternative.Bounds.Origin);
                    AppendAcquisitionArray(hash, alternative.Bounds.Extent);
                    AppendAcquisitionDouble(hash, alternative.Bounds.SphereRadius);
                }
                AppendAcquisitionArray(hash, alternative.Transform.Translation);
                AppendAcquisitionArray(hash, alternative.Transform.RotationDegrees);
                AppendAcquisitionArray(hash, alternative.Transform.Rotation);
                AppendAcquisitionArray(hash, alternative.Transform.Scale);
                AppendAcquisitionArray(hash, alternative.CrossSection.StartScale);
                AppendAcquisitionArray(hash, alternative.CrossSection.EndScale);
                AppendAcquisitionArray(hash, alternative.CrossSection.StartOffset);
                AppendAcquisitionArray(hash, alternative.CrossSection.EndOffset);
                AppendAcquisitionDouble(hash, alternative.CrossSection.StartRoll);
                AppendAcquisitionDouble(hash, alternative.CrossSection.EndRoll);
                AppendAcquisitionArray(hash, alternative.CrossSection.Up);
                AppendAcquisitionByte(hash, alternative.ForwardAxis);
            }
        }

        /** Appends core definition facts and spline-only profile evidence when applicable. */
        static void AppendAcquisitionDefinition(std::uint64_t& hash, const Facts::Building& building) noexcept
        {
            const auto& definition = building.DefinitionFacts;
            AppendAcquisitionByte(hash, definition ? 1 : 0);
            if (!definition)
                return;
            AppendAcquisitionInteger(hash, static_cast<std::uint32_t>(definition->ObjectIndex));
            AppendAcquisitionInteger(hash, definition->BuildingId);
            AppendAcquisitionString(hash, definition->CanonicalClassPath);
            AppendAcquisitionString(hash, definition->EntityConfigPath);
            AppendAcquisitionEnum(hash, definition->ClassStatus);
            AppendAcquisitionEnum(hash, definition->ConfigReferenceStatus);
            AppendAcquisitionEnum(hash, definition->InteriorStatus);
            AppendAcquisitionByte(hash, definition->InteriorOnly ? 1 : 0);
            if (building.SplineClassificationStatus == Status::Ready && building.HasSpline)
            {
                AppendAcquisitionEnum(hash, definition->SplineProfileStatus);
                AppendAcquisitionProfiles(hash, definition->SplineProfiles);
            }
        }

        /** Appends immutable callback-copied spline values without pointer identity. */
        static void AppendCallbackSplineFacts(std::uint64_t& hash,
            const std::shared_ptr<const Facts::Spline>& spline) noexcept
        {
            AppendAcquisitionByte(hash, spline ? 1 : 0);
            if (!spline)
                return;
            AppendAcquisitionSplineData(hash, spline->FragmentData);
            AppendAcquisitionCurve(hash, spline->Position);
            AppendAcquisitionCurve(hash, spline->Rotation);
            AppendAcquisitionCurve(hash, spline->Scale);
            AppendAcquisitionCurve(hash, spline->Reparameterization);
            AppendAcquisitionInteger(hash, spline->CurveVersion);
            AppendAcquisitionByte(hash, spline->CurvesInitialized ? 1 : 0);
        }

        /** Appends deferred spline fallback values and final source classification. */
        static void AppendDeferredSplineFacts(std::uint64_t& hash,
            const std::shared_ptr<const Facts::Spline>& spline) noexcept
        {
            AppendAcquisitionByte(hash, spline ? 1 : 0);
            if (!spline)
                return;
            AppendAcquisitionByte(hash, spline->PayloadData ? 1 : 0);
            if (spline->PayloadData)
                AppendAcquisitionSplineData(hash, *spline->PayloadData);
            AppendAcquisitionEnum(hash, spline->Provenance);
            AppendAcquisitionEnum(hash, spline->SourceStatus);
        }

        /** Hashes callback-owned identity, transform, reach and raw spline values. */
        static std::uint64_t ComputePreliminaryAcquisitionContribution(const Facts::Building& building) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            AppendAcquisitionByte(hash, 0xC1);
            AppendAcquisitionInteger(hash, static_cast<std::uint32_t>(building.Index));
            AppendAcquisitionInteger(hash, static_cast<std::uint32_t>(building.Serial));
            AppendAcquisitionEnum(hash, building.EntityStatus);
            AppendAcquisitionEnum(hash, building.TransformStatus);
            AppendAcquisitionEnum(hash, building.SplineClassificationStatus);
            AppendAcquisitionByte(hash, building.HasSpline ? 1 : 0);
            AppendAcquisitionDouble(hash, building.ResolvedReach);
            AppendAcquisitionArray(hash, building.Position);
            AppendAcquisitionArray(hash, building.Rotation);
            AppendAcquisitionArray(hash, building.SignedScale);
            AppendCallbackSplineFacts(hash, building.SplineFacts);
            return SplitMix64(hash);
        }

        /** Combines the retained callback contribution with finalized deferred occurrence facts. */
        static std::uint64_t CompleteAcquisitionContribution(
            std::uint64_t preliminary, const Facts::Building& building) noexcept
        {
            std::uint64_t deferredHash = 14695981039346656037ULL;
            AppendAcquisitionByte(deferredHash, 0xD1);
            AppendAcquisitionEnum(deferredHash, building.DefinitionStatus);
            AppendAcquisitionEnum(deferredHash, building.SplineStatus);
            AppendAcquisitionByte(deferredHash, building.Complete ? 1 : 0);
            AppendAcquisitionDefinition(deferredHash, building);
            AppendDeferredSplineFacts(deferredHash, building.SplineFacts);
            return SplitMix64(preliminary ^ SplitMix64(deferredHash));
        }

        /** Finalizes a multiset aggregate after every preliminary contribution is replaced. */
        static std::uint64_t FinalizeAcquisitionHash(const AcquisitionHashAggregate& aggregate) noexcept
        {
            const std::uint64_t combined = aggregate.Sum ^ std::rotl(aggregate.Xor, 23) ^ aggregate.Count;
            return SplitMix64(combined);
        }

        /** Applies the standard SplitMix64 avalanche to one deterministic value. */
        static std::uint64_t SplitMix64(std::uint64_t value) noexcept
        {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Aggregates one repeated CPU phase without retaining per-operation history. */
        struct Metric
        {
            std::uint64_t Samples = 0;
            double TotalMs = 0.0;
            double MaxMs = 0.0;

            void Add(double elapsedMs) noexcept
            {
                ++Samples;
                TotalMs += elapsedMs;
                MaxMs = (std::max)(MaxMs, elapsedMs);
            }

            double MeanMs() const noexcept
            {
                return Samples == 0 ? 0.0 : TotalMs / static_cast<double>(Samples);
            }
        };

        /** Numeric evidence for the slowest regional Mass traversal in the active interval. */
        struct WorstMembershipScan
        {
            double TotalMs = 0.0;
            double ChunkExecutionMs = 0.0;
            double OwnedFactCopyingMs = 0.0;
            double DeferredAcquisitionMs = 0.0;
            double ReconciliationMs = 0.0;
            std::uint64_t Chunks = 0;
            std::uint64_t Entities = 0;
            std::uint64_t Admitted = 0;
            std::uint64_t Pending = 0;
            std::uint64_t Added = 0;
            std::uint64_t Removed = 0;
        };

        /** Owns bounded interval counters and phase aggregates for this collector session. */
        struct Measurements
        {
            Clock::time_point IntervalStart{};
            Metric Update;
            Metric UpdateMembership;
            Metric Membership;
            Metric ChunkExecution;
            Metric RegionalBookkeeping;
            Metric OwnedFactCopying;
            Metric Acquisition;
            std::uint64_t MembershipScans = 0;
            std::uint64_t Chunks = 0;
            std::uint64_t Examined = 0;
            std::uint64_t Admitted = 0;
            std::uint64_t MissingOrigin = 0;
            std::uint64_t Added = 0;
            std::uint64_t Removed = 0;
            std::uint64_t FailedScans = 0;
            std::uint64_t EqualRegionRequests = 0;
            std::uint64_t ChangedRegionRequests = 0;
            std::uint64_t ContainedRegionRequests = 0;
            std::uint64_t ImmediateRegionRequests = 0;
            std::uint64_t SuccessfulCoverageReuse = 0;
            std::uint64_t RejectedCoverageReuse = 0;
            std::uint64_t PeriodicScans = 0;
            std::uint64_t ImmediateScans = 0;
            std::uint64_t MutationScans = 0;
            std::uint64_t FailedImmediateScans = 0;
            std::uint64_t FailedMutationScans = 0;
            std::uint64_t FailedFallbackScans = 0;
            WorstMembershipScan WorstScan;

            void Reset() noexcept
            {
                *this = {};
                IntervalStart = Clock::now();
            }
        };

        /** Captures one regional traversal and folds its numeric result into the interval. */
        struct MembershipTiming
        {
            State& Owner;
            Clock::time_point Started = Clock::now();
            double ChunkExecutionMs = 0.0;
            double OwnedFactCopyingMs = 0.0;
            double DeferredAcquisitionMs = 0.0;
            double ReconciliationMs = 0.0;
            std::uint64_t Chunks = 0;
            std::uint64_t Examined = 0;
            std::uint64_t Admitted = 0;
            std::uint64_t MissingOrigin = 0;
            std::uint64_t Added = 0;
            std::uint64_t Removed = 0;
            std::uint64_t Generation = 0;
            AcquisitionReason Reason = AcquisitionReason::Immediate;
            bool Complete = false;

            explicit MembershipTiming(State& owner) noexcept : Owner(owner), Generation(owner.Generation) {}

            ~MembershipTiming() noexcept
            {
                if (Owner.Generation != Generation)
                {
                    return;
                }
                const double totalMs = ElapsedMs(Started);
                auto& measurements = Owner.Diagnostics;
                ++measurements.MembershipScans;
                measurements.Membership.Add(totalMs);
                measurements.ChunkExecution.Add(ChunkExecutionMs);
                measurements.OwnedFactCopying.Add(OwnedFactCopyingMs);
                measurements.Acquisition.Add(DeferredAcquisitionMs);
                measurements.RegionalBookkeeping.Add(ReconciliationMs);
                measurements.Chunks += Chunks;
                measurements.Examined += Examined;
                measurements.Admitted += Admitted;
                measurements.MissingOrigin += MissingOrigin;
                measurements.Added += Added;
                measurements.Removed += Removed;
                measurements.FailedScans += Complete ? 0 : 1;
                if (Complete)
                {
                    if (Reason == AcquisitionReason::Immediate)
                        ++measurements.ImmediateScans;
                    else if (Reason == AcquisitionReason::Mutation)
                        ++measurements.MutationScans;
                    else
                        ++measurements.PeriodicScans;
                }
                else if (Reason == AcquisitionReason::Immediate)
                {
                    ++measurements.FailedImmediateScans;
                }
                else if (Reason == AcquisitionReason::Mutation)
                {
                    ++measurements.FailedMutationScans;
                }
                else
                {
                    ++measurements.FailedFallbackScans;
                }
                if (totalMs >= measurements.WorstScan.TotalMs)
                {
                    measurements.WorstScan = {totalMs, ChunkExecutionMs, OwnedFactCopyingMs, DeferredAcquisitionMs,
                        ReconciliationMs, Chunks, Examined, Admitted, Owner.Pending.size(), Added, Removed};
                }
            }

            static double ElapsedMs(Clock::time_point started) noexcept
            {
                return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            }
        };

        /** Adds one elapsed phase sample when an existing operation scope exits. */
        struct PhaseTiming
        {
            Metric& Aggregate;
            Clock::time_point Started = Clock::now();

            explicit PhaseTiming(Metric& aggregate) noexcept : Aggregate(aggregate) {}

            ~PhaseTiming() noexcept
            {
                Aggregate.Add(std::chrono::duration<double, std::milli>(Clock::now() - Started).count());
            }
        };

        Measurements Diagnostics;
#endif

        const std::chrono::milliseconds RetryInterval;

        /** Stores the immutable scan interval and rejects nonpositive values once. */
        explicit State(std::chrono::milliseconds retryInterval)
            : RetryInterval(retryInterval)
        {
            if (RetryInterval <= std::chrono::milliseconds::zero())
            {
                throw std::invalid_argument("BuildingCollectorV2 retry interval must be positive.");
            }
        }

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

        /** Returns a typed SDK view over the genuine borrowed Mass fragment array. */
        template<typename FragmentType>
        static TConstArrayView<FragmentType> GetOptionalFragmentView(
            const FMassExecutionContext& context,
            const SDK::UScriptStruct* sdkType)
        {
            const auto* type = reinterpret_cast<const ::UScriptStruct*>(sdkType);
            const auto nativeView = context.GetFragmentView(type);
            return TConstArrayView<FragmentType>(
                reinterpret_cast<const FragmentType*>(nativeView.GetData()),
                nativeView.Num());
        }

        /** Builds chunk-local views and rejects any broken entity/fragment correspondence. */
        bool CreateMassChunkViews(
            const FMassExecutionContext& context,
            MassChunkViews& views) const
        {
            views.Entities = context.GetEntities();
            views.Transforms = GetOptionalFragmentView<SDK::FTransformFragment>(context, Types->Transform);
            views.SplineConnections = GetOptionalFragmentView<SDK::FAuSplineConnectionFragment>(context, Types->Spline);
            views.ActorPlacement = GetOptionalFragmentView<SDK::FAuAPMassFragment>(context, Types->Placement);
            views.HasTransforms = !views.Transforms.IsEmpty();
            views.HasSplineConnections = !views.SplineConnections.IsEmpty();
            views.HasActorPlacement = !views.ActorPlacement.IsEmpty();
            const auto* parametersType = reinterpret_cast<const ::UScriptStruct*>(Types->Parameters);
            // The context accessor returns the shared fragment's payload bytes, or null when absent.
            views.BuildingParameters = static_cast<const SDK::FCrBuildingParameters*>(
                context.GetConstSharedFragmentPtr(*parametersType));

            const int32 entityCount = views.Entities.Num();
            const auto hasValidCorrespondence = [entityCount](const auto& fragmentView)
            {
                return fragmentView.IsEmpty() ||
                    (fragmentView.Num() == entityCount && fragmentView.GetData() != nullptr);
            };
            return hasValidCorrespondence(views.Transforms) &&
                hasValidCorrespondence(views.SplineConnections) &&
                hasValidCorrespondence(views.ActorPlacement);
        }

        /** Runs one serial chunk traversal and converts callback exceptions to incomplete status. */
        bool ForEachMassChunk(
            const Native& native,
            TFunction<void(const MassChunkViews&)> visitor) noexcept
        {
            try
            {
                MassExecutionContextOwner executionContext(native, Manager);
                bool callbackFailed = false;
                FMassExecuteFunction callback = [this, &visitor, &callbackFailed](
                    FMassExecutionContext& context)
                {
                    if (callbackFailed)
                        return;
                    try
                    {
                        MassChunkViews views;
                        if (!CreateMassChunkViews(context, views))
                        {
                            callbackFailed = true;
                            return;
                        }
                        visitor(views);
                    }
                    catch (...)
                    {
                        callbackFailed = true;
                    }
                };
                native.mass.forEachEntityChunk(
                    reinterpret_cast<FMassEntityQuery*>(Query.data()),
                    *executionContext.Get(),
                    callback);
                return !callbackFailed;
            }
            catch (...)
            {
                return false;
            }
        }

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
            std::uint64_t Request;
            bool HasRegion;
            Facts::Region Region;
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
        std::uint64_t RequestGeneration = 0;
        /** Content hash for the currently committed regional dataset; zero is a valid value. */
        std::uint64_t AcquisitionContentHash = 0;
        Clock::time_point NextFallback{};
        Clock::time_point RetryNotBefore{};
        std::uint64_t ObservedMutationRevision = 0;
        std::uint64_t AcknowledgedMutationRevision = 0;
        Clock::time_point LatestMutationObservedAt{};
        bool HasMutationBaseline = false;
        bool MembershipComplete = false;
        bool HasRegion = false;
        bool IncludeDroneRails = true;
        Facts::Region RequestedRegion;
        Facts::Region CommittedCoverage;
        SDK::UWorld* CoverageWorld = nullptr;
        const void* CoverageManager = nullptr;
        std::uint64_t CoverageSession = 0;
        std::uint64_t CoverageRequest = 0;
        std::uint64_t CoverageContentHash = 0;
        bool CommittedCoverageValid = false;
        bool RegionChangePendingValidation = false;
        bool PendingSnapshotEligible = false;
        bool PendingReuseEligible = false;
        bool ImmediateScanRequired = true;

        /** Packs the full session-private Index/Serial pair; never a content fingerprint. */
        static std::uint64_t Key(EntityId id) noexcept
        {
            return std::uint64_t(std::uint32_t(id.Index)) |
                (std::uint64_t(std::uint32_t(id.SerialNumber)) << 32);
        }

        /** Captures logical source lifetime before native operations. */
        Context CaptureContext() const noexcept
        {
            return {World, Manager, Generation, RequestGeneration, HasRegion, RequestedRegion};
        }

        /** Checks exact inclusive-edge containment between already validated positive rectangles. */
        static bool IsRegionContained(const Facts::Region& requested, const Facts::Region& coverage) noexcept
        {
            return requested.X >= coverage.X && requested.Y >= coverage.Y &&
                requested.X + requested.Width <= coverage.X + coverage.Width &&
                requested.Y + requested.Height <= coverage.Y + coverage.Height;
        }

        /** Confirms that retained membership still belongs to this world's current native query source. */
        bool IsNativeSourceCurrent(const Native& native, SDK::UWorld* world) const
        {
            return world && QueryConstructed && Types && World == world && Manager &&
                ReadManager(ManagerStorage(native, world)) == Manager;
        }

        /** Establishes a new collector's revision baseline without importing prior-session dirtiness. */
        void EstablishMutationBaseline() noexcept
        {
            std::uint64_t revision = 0;
            MiniMapMassLifecycle::MassLifecycle::TryGetAcceptedMutationRevision(revision);
            ObservedMutationRevision = revision;
            AcknowledgedMutationRevision = revision;
            LatestMutationObservedAt = {};
            HasMutationBaseline = true;
        }

        /** Starts a fresh quiet period only when a new accepted revision is observed. */
        void ObserveMutationRevision(std::uint64_t revision) noexcept
        {
            if (!HasMutationBaseline)
            {
                EstablishMutationBaseline();
            }
            if (revision > ObservedMutationRevision)
            {
                ObservedMutationRevision = revision;
                LatestMutationObservedAt = Clock::now();
            }
        }

        /** Returns the local 100 ms trailing-edge deadline for the latest observed revision. */
        Clock::time_point MutationQuietDeadline() const noexcept
        {
            return LatestMutationObservedAt + MutationQuietPeriod;
        }

        /** Withdraws retained membership when the world or native query source is unavailable. */
        void InvalidateCoverageForUnavailableSource() noexcept
        {
            const bool hadCoverageCandidate = CommittedCoverageValid ||
                PendingSnapshotEligible || PendingReuseEligible;
            MembershipComplete = false;
            CommittedCoverageValid = false;
            NextFallback = {};
            RetryNotBefore = {};
            ImmediateScanRequired = true;
            if (RegionChangePendingValidation)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                if (hadCoverageCandidate)
                {
                    ++Diagnostics.RejectedCoverageReuse;
                }
                ++Diagnostics.ImmediateRegionRequests;
#endif
                RegionChangePendingValidation = false;
                PendingSnapshotEligible = false;
                PendingReuseEligible = false;
            }
        }

        /** Confirms the source before reuse and completes any deferred contained-region decision. */
        bool PrepareCurrentUpdate(const Native& native, SDK::UWorld* world)
        {
            if (!IsNativeSourceCurrent(native, world))
            {
                InvalidateCoverageForUnavailableSource();
                return false;
            }
            ValidatePendingRegionReuse(world);
            return true;
        }

        /** Restores eligibility only after game-thread source checks and exact coverage validation. */
        void ValidatePendingRegionReuse(SDK::UWorld* world)
        {
            if (!RegionChangePendingValidation)
                return;
            const bool hadCoverageCandidate = CommittedCoverageValid ||
                PendingSnapshotEligible || PendingReuseEligible;
            const auto now = Clock::now();
            const bool snapshotCurrent = CommittedCoverageValid &&
                CoverageWorld == World && CoverageWorld == world && CoverageManager == Manager &&
                CoverageSession == Generation && CoverageRequest != 0 &&
                CoverageContentHash == AcquisitionContentHash;
            const bool beforeDeadline = NextFallback != Clock::time_point{} && now < NextFallback;
            const bool contained = snapshotCurrent && IsRegionContained(RequestedRegion, CommittedCoverage);
            if (PendingReuseEligible && contained && beforeDeadline)
            {
                MembershipComplete = true;
                ImmediateScanRequired = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++Diagnostics.SuccessfulCoverageReuse;
#endif
            }
            else if (snapshotCurrent && contained && PendingReuseEligible)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++Diagnostics.RejectedCoverageReuse;
#endif
            }
            else if (!snapshotCurrent)
            {
                ImmediateScanRequired = true;
                NextFallback = {};
                CommittedCoverageValid = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                if (hadCoverageCandidate)
                {
                    ++Diagnostics.RejectedCoverageReuse;
                }
#endif
            }
            else
            {
                ImmediateScanRequired = !PendingSnapshotEligible || !contained;
                if (ImmediateScanRequired)
                {
                    NextFallback = {};
                }
            }
            RegionChangePendingValidation = false;
            PendingSnapshotEligible = false;
            PendingReuseEligible = false;
        }

        /** Accepts exact geometry and schedules validation or an immediate uncovered scan. */
        bool AcceptRequestedRegion(const Facts::Region& region) noexcept
        {
            const double right = region.X + region.Width;
            const double bottom = region.Y + region.Height;
            if (!std::isfinite(region.X) || !std::isfinite(region.Y) || !std::isfinite(region.Width) ||
                !std::isfinite(region.Height) || region.Width <= 0 || region.Height <= 0 ||
                !std::isfinite(right) || !std::isfinite(bottom) || right <= region.X || bottom <= region.Y ||
                !std::isfinite(std::hypot(region.Width, region.Height)))
            {
                return false;
            }
            if (HasRegion && RequestedRegion == region)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++Diagnostics.EqualRegionRequests;
#endif
                return true;
            }
            const bool sameCommittedSnapshot = CommittedCoverageValid && MembershipComplete &&
                CoverageWorld == World && CoverageManager == Manager && CoverageSession == Generation &&
                CoverageRequest != 0 && CoverageContentHash == AcquisitionContentHash;
            const bool pendingSnapshotEligible = RegionChangePendingValidation
                ? PendingSnapshotEligible
                : sameCommittedSnapshot;
            const bool containedByCoverage = CommittedCoverageValid && IsRegionContained(region, CommittedCoverage);
            const bool contained = pendingSnapshotEligible && containedByCoverage;
            RequestedRegion = region;
            HasRegion = true;
            ++RequestGeneration;
            if (RequestGeneration == 0)
                ++RequestGeneration;
            MembershipComplete = false;
            RegionChangePendingValidation = true;
            PendingSnapshotEligible = pendingSnapshotEligible;
            PendingReuseEligible = contained;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ++Diagnostics.ChangedRegionRequests;
            if (containedByCoverage)
            {
                ++Diagnostics.ContainedRegionRequests;
            }
            if (!contained)
            {
                ++Diagnostics.ImmediateRegionRequests;
            }
#endif
            return true;
        }

        /** Obtains existing native shared-pointer storage; never fabricates shared ownership. */
        static const std::byte* ManagerStorage(const Native& native, SDK::UWorld* world)
        {
            static_assert(sizeof(void*) == 8);
            auto* subsystem = world ? native.mass.getMassEntitySubsystem(world) : nullptr;
            if (!subsystem || native.mass.entityManagerFieldOffset < 0)
                return nullptr;
            return reinterpret_cast<const std::byte*>(subsystem) +
                native.mass.entityManagerFieldOffset;
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
                Generation != context.Session || RequestGeneration != context.Request ||
                HasRegion != context.HasRegion || (HasRegion && !(RequestedRegion == context.Region)))
                return false;
            return ReadManager(ManagerStorage(native, context.World)) == context.Manager;
        }

        /** Destroys query before releasing roots; caller keeps native services/world valid. */
        void ClearSession(bool clearRequest)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            Diagnostics.Reset();
#endif
            ++Generation;
            if (QueryConstructed)
                MiniMapNative::Get()->mass.queryDestruct(Query.data());
            QueryConstructed = false;
            World = nullptr;
            Manager = nullptr;
            Pending.clear();
            Entities.clear();
            AcquisitionContentHash = 0;
            CommittedCoverage = {};
            CoverageWorld = nullptr;
            CoverageManager = nullptr;
            CoverageSession = 0;
            CoverageRequest = 0;
            CoverageContentHash = 0;
            CommittedCoverageValid = false;
            RegionChangePendingValidation = false;
            PendingSnapshotEligible = false;
            PendingReuseEligible = false;
            ImmediateScanRequired = true;
            Definitions.clear();
            NextFallback = {};
            RetryNotBefore = {};
            ObservedMutationRevision = 0;
            AcknowledgedMutationRevision = 0;
            LatestMutationObservedAt = {};
            HasMutationBaseline = false;
            MembershipComplete = false;
            if (clearRequest)
            {
                HasRegion = false;
                RequestedRegion = {};
                ++RequestGeneration;
                if (RequestGeneration == 0)
                    ++RequestGeneration;
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
            native.mass.addOptionalSplineRequirement(Query.data(),
                std::uint8_t(SDK::EMassFragmentAccess::ReadOnly),
                std::uint8_t(SDK::EMassFragmentPresence::Optional));
            native.mass.addOptionalAPRequirement(Query.data(),
                std::uint8_t(SDK::EMassFragmentAccess::ReadOnly),
                std::uint8_t(SDK::EMassFragmentPresence::Optional));
            native.mass.addOptionalBuildingParametersRequirement(
                Query.data(), std::uint8_t(SDK::EMassFragmentPresence::Optional));
            EstablishMutationBaseline();
            return true;
        }

        /** Resolves the one reach used by regional admission and later Section selection. */
        static inline double ResolveBuildingReach(const SDK::FCrBuildingParameters* parameters) noexcept
        {
            (void)parameters;
            return 5000.0;
        }

        /** Applies the approved shortest XY distance from one finite origin to the exact request. */
        static bool IsRegionallyAdmitted(
            const Facts::Region& region,
            double px,
            double py,
            double resolvedReach) noexcept
        {
            const double right = region.X + region.Width;
            const double bottom = region.Y + region.Height;
            if (px >= region.X && px < right && py >= region.Y && py < bottom)
                return true;
            const double dx = (std::max)((std::max)(region.X - px, 0.0), px - right);
            const double dy = (std::max)((std::max)(region.Y - py, 0.0), py - bottom);
            return std::hypot(dx, dy) <= resolvedReach;
        }

        /** Copies transform values and preserves invalid rotation/scale status independently of XY admission. */
        static void CopyTransform(const SDK::FTransform& transform, Facts::Building& result)
        {
            result.Position = {transform.Translation.X, transform.Translation.Y, transform.Translation.Z};
            result.SignedScale = {transform.Scale3D.X, transform.Scale3D.Y, transform.Scale3D.Z};
            result.Rotation = {transform.Rotation.X, transform.Rotation.Y, transform.Rotation.Z, transform.Rotation.W};
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

        /** Copies an admitted entity while its transform, spline and AP views remain borrowed. */
        std::optional<OwnedScanOccurrence> CaptureMassOccurrence(
            const Native& native,
            const Context& context,
            const MassChunkViews& views,
            int32 index,
            EntityId id,
            bool& missingOrigin)
        {
            missingOrigin = false;
            if (!views.HasTransforms)
            {
                missingOrigin = true;
                LOG_WARN("MiniMap: BuildingMap skipped entity Index=%d Serial=%d because its optional transform origin is missing.",
                    id.Index, id.SerialNumber);
                return std::nullopt;
            }
            const SDK::FTransform& transform = views.Transforms[index].Transform;
            const double x = transform.Translation.X;
            const double y = transform.Translation.Y;
            if (!std::isfinite(x) || !std::isfinite(y))
            {
                missingOrigin = true;
                LOG_WARN("MiniMap: BuildingMap skipped entity Index=%d Serial=%d because its XY origin is nonfinite (x=%.9g y=%.9g).",
                    id.Index, id.SerialNumber, x, y);
                return std::nullopt;
            }
            const SDK::FCrBuildingParameters* parameters = views.BuildingParameters;
            const double resolvedReach = ResolveBuildingReach(parameters);
            if (!context.HasRegion || !IsRegionallyAdmitted(context.Region, x, y, resolvedReach))
                return std::nullopt;
            OwnedScanOccurrence occurrence;
            occurrence.Data.Index = id.Index;
            occurrence.Data.Serial = id.SerialNumber;
            occurrence.Data.EntityStatus = Status::Ready;
            occurrence.Data.ResolvedReach = resolvedReach;
            CopyTransform(transform, occurrence.Data);
            occurrence.Data.SplineClassificationStatus = Status::Ready;
            occurrence.Data.HasSpline = views.HasSplineConnections;
            if (occurrence.Data.HasSpline)
            {
                Facts::Spline spline = BuildingSplineSource::CopyFragment(views.SplineConnections[index]);
                occurrence.Data.SplineStatus = spline.SourceStatus;
                occurrence.Data.SplineFacts = std::make_shared<const Facts::Spline>(std::move(spline));
                if (BuildingSplineSource::NeedsFallback(*occurrence.Data.SplineFacts) && views.HasActorPlacement)
                {
                    occurrence.Payload = std::make_unique<PayloadHandleOwner>(
                        native, views.ActorPlacement[index].Payload);
                }
            }
            else
            {
                occurrence.Data.SplineStatus = Status::Ready;
            }
            if (views.BuildingParameters && views.BuildingParameters->PlacementData)
            {
                occurrence.Placement = DefinitionAcquisition.Retain(
                    views.BuildingParameters->PlacementData);
            }
            return std::move(occurrence);
        }

        /** Copies admitted chunk occurrences into owned storage and rejects duplicate identities. */
        void CaptureMassChunk(
            const Native& native,
            const Context& context,
            const MassChunkViews& views,
            std::vector<OwnedScanOccurrence>& occurrences,
            std::unordered_set<std::uint64_t>& seen,
            AcquisitionHashAggregate& acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
#endif
        )
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto copyingStarted = Clock::now();
#endif
            for (int32 index = 0; index < views.Entities.Num(); ++index)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++timing->Examined;
#endif
                const SDK::FMassEntityHandle sdkId = views.EntityAt(index);
                const EntityId id{sdkId.Index, sdkId.SerialNumber};
                if (id.Index < 0 || id.SerialNumber == 0)
                    throw std::runtime_error("Mass chunk contained an invalid building identity.");
                bool missingOrigin = false;
                auto occurrence = CaptureMassOccurrence(native, context, views, index, id, missingOrigin);
                if (!occurrence)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    if (missingOrigin)
                        ++timing->MissingOrigin;
#endif
                    continue;
                }
                if (!seen.insert(Key(id)).second)
                    throw std::runtime_error("Mass chunk traversal repeated a building identity.");
                occurrences.push_back(std::move(*occurrence));
                occurrences.back().PreliminaryHashContribution =
                    ComputePreliminaryAcquisitionContribution(occurrences.back().Data);
                acquisitionHash.Add(occurrences.back().PreliminaryHashContribution);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++timing->Admitted;
#endif
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing->OwnedFactCopyingMs += std::chrono::duration<double, std::milli>(Clock::now() - copyingStarted).count();
#endif
        }

        /** Completes one owned occurrence after every borrowed Mass view has expired. */
        bool AcquireOwnedOccurrence(
            const Native& native,
            const Context& context,
            OwnedScanOccurrence& occurrence,
            std::unordered_set<const DefinitionOwner*>& attemptedCoreDefinitions,
            std::unordered_set<const DefinitionOwner*>& attemptedSplineProfiles,
            Facts::Building& result)
        {
            result = std::move(occurrence.Data);
            if (result.SplineFacts && occurrence.Payload &&
                BuildingSplineSource::NeedsFallback(*result.SplineFacts))
            {
                Facts::Spline spline = *result.SplineFacts;
                BuildingSplineSource::CopyFallback(*occurrence.Payload->Get(), spline);
                result.SplineStatus = spline.SourceStatus;
                result.SplineFacts = std::make_shared<const Facts::Spline>(std::move(spline));
                if (!IsContextCurrent(native, context))
                    return false;
            }
            if (occurrence.Placement)
            {
                auto* placement = occurrence.Placement->Root;
                auto found = Definitions.find(placement);
                if (found == Definitions.end())
                {
                    found = Definitions.emplace(placement, occurrence.Placement).first;
                }
                const auto definition = found->second;
                CopyDefinition(native, context, definition, attemptedCoreDefinitions,
                    attemptedSplineProfiles,
                    result.SplineClassificationStatus == Status::Ready && result.HasSpline);
                if (!IsContextCurrent(native, context))
                    return false;
                result.DefinitionFacts = definition->Facts;
            }
            result.DefinitionStatus = DefinitionStatus(result.DefinitionFacts);
            result.Complete = result.DefinitionStatus == Status::Ready &&
                result.TransformStatus == Status::Ready && result.SplineStatus == Status::Ready &&
                AreRequiredSplineProfilesResolved(result.DefinitionFacts, result.HasSpline);
            return IsContextCurrent(native, context);
        }

        /** Compares acquired values deeply enough to preserve unchanged immutable spline owners. */
        static bool SameAcquiredFacts(const Facts::Building& first, const Facts::Building& second)
        {
            const bool definitionsMatch = first.DefinitionFacts == second.DefinitionFacts ||
                (first.DefinitionFacts && second.DefinitionFacts &&
                    *first.DefinitionFacts == *second.DefinitionFacts);
            const bool splinesMatch = first.SplineFacts == second.SplineFacts ||
                (first.SplineFacts && second.SplineFacts && *first.SplineFacts == *second.SplineFacts);
            return first.Index == second.Index && first.Serial == second.Serial &&
                first.EntityStatus == second.EntityStatus && first.DefinitionStatus == second.DefinitionStatus &&
                first.TransformStatus == second.TransformStatus &&
                first.SplineClassificationStatus == second.SplineClassificationStatus &&
                first.SplineStatus == second.SplineStatus && first.HasSpline == second.HasSpline &&
                first.Complete == second.Complete && first.Position == second.Position &&
                first.ResolvedReach == second.ResolvedReach &&
                first.Rotation == second.Rotation && first.SignedScale == second.SignedScale &&
                definitionsMatch && splinesMatch;
        }

        /** Captures one serial traversal into callback-owned snapshots. */
        bool CaptureMassSnapshot(
            const Native& native,
            const Context& context,
            std::vector<OwnedScanOccurrence>& occurrences,
            AcquisitionHashAggregate& acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
#endif
        )
        {
            std::unordered_set<std::uint64_t> seen;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto chunkStarted = Clock::now();
#endif
            const bool traversed = ForEachMassChunk(native, TFunction<void(const MassChunkViews&)>(
                [this, &native, &context, &occurrences, &seen, &acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , timing
#endif
                ](const MassChunkViews& views)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing->Chunks;
#endif
                    CaptureMassChunk(native, context, views, occurrences, seen, acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        , timing
#endif
                    );
                }));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing->ChunkExecutionMs = std::chrono::duration<double, std::milli>(Clock::now() - chunkStarted).count();
#endif
            return traversed && IsContextCurrent(native, context);
        }

        /** Adds one completed occurrence to the temporary reconciled regional state. */
        void ReconcileAcquiredOccurrence(
            Facts::Building& data,
            const OwnedScanOccurrence& occurrence,
            std::unordered_map<std::uint64_t, Entry>& reconciled,
            std::unordered_set<std::uint64_t>& pending)
        {
            const auto key = Key({data.Index, data.Serial});
            auto previous = Entities.find(key);
            Entry entry;
            if (previous != Entities.end() && SameAcquiredFacts(previous->second.Data, data))
            {
                entry.Data = previous->second.Data;
                entry.Definition = previous->second.Definition;
            }
            else
            {
                entry.Data = std::move(data);
            }
            if (!entry.Data.Complete)
            {
                const auto definition = Definitions.find(
                    occurrence.Placement ? occurrence.Placement->Root : nullptr);
                if (definition != Definitions.end())
                    entry.Definition = definition->second;
                pending.insert(key);
            }
            reconciled.emplace(key, std::move(entry));
        }

        /** Defers payload and authored-definition work until all borrowed Mass views have expired. */
        bool BuildRegionalState(
            const Native& native,
            const Context& context,
            std::vector<OwnedScanOccurrence>& occurrences,
            std::unordered_map<std::uint64_t, Entry>& reconciled,
            std::unordered_set<std::uint64_t>& pending,
            AcquisitionHashAggregate& acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
#endif
        )
        {
            reconciled.reserve(occurrences.size());
            pending.reserve(occurrences.size());
            std::unordered_set<const DefinitionOwner*> attemptedCoreDefinitions;
            std::unordered_set<const DefinitionOwner*> attemptedSplineProfiles;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto acquisitionStarted = Clock::now();
#endif
            for (auto& occurrence : occurrences)
            {
                Facts::Building data;
                if (!AcquireOwnedOccurrence(native, context, occurrence,
                    attemptedCoreDefinitions, attemptedSplineProfiles, data))
                    return false;
                acquisitionHash.Replace(occurrence.PreliminaryHashContribution,
                    CompleteAcquisitionContribution(occurrence.PreliminaryHashContribution, data));
                ReconcileAcquiredOccurrence(data, occurrence, reconciled, pending);
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing->DeferredAcquisitionMs = std::chrono::duration<double, std::milli>(Clock::now() - acquisitionStarted).count();
#endif
            return IsContextCurrent(native, context);
        }

        /** Commits the fully acquired regional set only while its session and request remain current. */
        bool CommitRegionalState(
            const Native& native,
            const Context& context,
            std::unordered_map<std::uint64_t, Entry>& reconciled,
            std::unordered_set<std::uint64_t>& pending,
            std::uint64_t acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
#endif
        )
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto reconciliationStarted = Clock::now();
#endif
            if (!IsContextCurrent(native, context))
                return false;
            for (const auto& previous : Entities)
            {
                if (!reconciled.contains(previous.first))
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing->Removed;
#endif
                }
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            for (const auto& item : reconciled)
            {
                if (!Entities.contains(item.first))
                    ++timing->Added;
            }
#endif
            Entities.swap(reconciled);
            Pending.swap(pending);
            AcquisitionContentHash = acquisitionHash;
            CommittedCoverage = context.Region;
            CoverageWorld = context.World;
            CoverageManager = context.Manager;
            CoverageSession = context.Session;
            CoverageRequest = context.Request;
            CoverageContentHash = acquisitionHash;
            CommittedCoverageValid = true;
            MembershipComplete = true;
            RegionChangePendingValidation = false;
            PendingSnapshotEligible = false;
            PendingReuseEligible = false;
            ImmediateScanRequired = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing->ReconciliationMs = std::chrono::duration<double, std::milli>(Clock::now() - reconciliationStarted).count();
#endif
            return true;
        }

        /** Completes one regional scan without exposing partial traversal or acquisition results. */
        bool Reconcile(
            const Native& native,
            const Context& context
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
#endif
        )
        {
            if (!context.HasRegion || !IsContextCurrent(native, context))
                return false;
            std::vector<OwnedScanOccurrence> occurrences;
            AcquisitionHashAggregate acquisitionHash{};
            if (!CaptureMassSnapshot(native, context, occurrences, acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
#endif
            ))
                return false;
            std::unordered_map<std::uint64_t, Entry> reconciled;
            std::unordered_set<std::uint64_t> pending;
            if (!BuildRegionalState(native, context, occurrences, reconciled, pending, acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
#endif
            ))
                return false;
            return CommitRegionalState(native, context, reconciled, pending,
                FinalizeAcquisitionHash(acquisitionHash)
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
#endif
            );
        }

        /** Chooses immediate coverage, expired mutation, or ordinary fallback work in priority order. */
        bool TrySelectAcquisitionReason(
            Clock::time_point now,
            std::uint64_t& startRevision,
            AcquisitionReason& reason)
        {
            if (!MiniMapMassLifecycle::MassLifecycle::TryGetAcceptedMutationRevision(startRevision))
                return false;
            ObserveMutationRevision(startRevision);
            const bool mutationPending = ObservedMutationRevision > AcknowledgedMutationRevision;
            if (ImmediateScanRequired)
            {
                reason = AcquisitionReason::Immediate;
            }
            else if (mutationPending && now >= MutationQuietDeadline())
            {
                reason = AcquisitionReason::Mutation;
            }
            else if (mutationPending)
            {
                return false;
            }
            else if (NextFallback == Clock::time_point{} || now >= NextFallback)
            {
                reason = AcquisitionReason::Fallback;
            }
            else
            {
                return false;
            }
            return true;
        }

        /** Executes one selected scan and acknowledges only its captured revision after a current commit. */
        void ExecuteAcquisition(
            const Native& native,
            const Context& context,
            std::uint64_t startRevision,
            AcquisitionReason reason)
        {
            bool complete = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MembershipTiming timing{*this};
            timing.Reason = reason;
#endif
            try
            {
                complete = Reconcile(native, context
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , &timing
#endif
                );
            }
            catch (...)
            {
                complete = false;
            }
            if (!IsContextCurrent(native, context))
                return;
            const auto completedAt = Clock::now();
            if (!complete)
            {
                MembershipComplete = false;
                CommittedCoverageValid = false;
                RetryNotBefore = completedAt + RetryInterval;
            }
            else
            {
                MembershipComplete = true;
                AcknowledgedMutationRevision = (std::max)(
                    AcknowledgedMutationRevision, startRevision);
                NextFallback = completedAt + PeriodicFallbackInterval;
                RetryNotBefore = {};
                ImmediateScanRequired = false;
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing.Complete = complete;
#endif
        }

        /** Runs the due request scan and keeps failed or stale results incomplete. */
        void ReconcileIfDue(const Native& native)
        {
            const auto context = CaptureContext();
            const auto now = Clock::now();
            if (!context.HasRegion || (RetryNotBefore != Clock::time_point{} && now < RetryNotBefore))
                return;
            std::uint64_t startRevision = 0;
            AcquisitionReason reason;
            if (!TrySelectAcquisitionReason(now, startRevision, reason))
                return;
            ExecuteAcquisition(native, context, startRevision, reason);
        }

        /** Validates a copied fixed-size numerical value, never native memory. */
        template<std::size_t Size> static bool Finite(const std::array<double, Size>& values)
        {
            for (double value : values)
                if (!std::isfinite(value))
                    return false;
            return true;
        }


        /** Acquires shared core facts and spline profiles independently once per definition in the deferred pass. */
        void CopyDefinition(const Native& native, const Context& context,
            const std::shared_ptr<DefinitionOwner>& definition,
            std::unordered_set<const DefinitionOwner*>& attemptedCoreDefinitions,
            std::unordered_set<const DefinitionOwner*>& attemptedSplineProfiles,
            bool hasSpline)
        {
            const bool coreComplete = definition->Facts && BuildingDefinitionAcquisition::IsComplete(*definition->Facts);
            if (!coreComplete && attemptedCoreDefinitions.insert(definition.get()).second)
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
                if (!attemptedSplineProfiles.insert(definition.get()).second)
                    return;
                auto facts = std::make_shared<Facts::Definition>(*definition->Facts);
                facts->SplineProfileStatus = ::Services::Map::Buildings::SplineProfileAcquisitionStatus::NotApplicable;
                definition->Facts = std::move(facts);
                return;
            }
            if (IsSplineProfileTerminal(definition->Facts->SplineProfileStatus))
                return;
            if (!attemptedSplineProfiles.insert(definition.get()).second)
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
                if (building.DefinitionFacts &&
                    building.DefinitionFacts->ClassStatus == Status::Ready &&
                    IsDroneRailDefinitionId(building.DefinitionFacts->BuildingId))
                {
                    source.splineGeometryMode =
                        ::Services::Map::Buildings::BuildingCandidateSplineGeometryMode::SyntheticThinRibbon;
                    source.syntheticThinRibbonHalfWidthMapUnits = 5.0;
                }
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

        using CandidateOccurrence = ::Services::Map::Buildings::BuildingCandidateOccurrence;
        using CandidateSourceRecord = ::Services::Map::Buildings::BuildingCandidateSourceRecord;
        using CandidatePublication = ::Services::Map::Buildings::BuildingCandidatePublication;
        using CollectionRegion = ::Services::Map::Buildings::BuildingCollectionRegion;

        /** Indexes one exact Section cell in the service's zero-origin grid. */
        struct SectionCellCoordinate
        {
            std::int64_t X = 0;
            std::int64_t Y = 0;
        };

        /** Stores requested Sections by row so sparse batches avoid scanning absent cells. */
        struct RequestedSectionLookup
        {
            std::map<std::int64_t, std::map<std::int64_t, std::vector<std::size_t>>> SectionsByRow;
            double SectionSize = 0.0;
            double MinimumX = 0.0;
            double MinimumY = 0.0;
            double MaximumRight = 0.0;
            double MaximumBottom = 0.0;
            bool HasBounds = false;
            std::int64_t MinimumGridX = 0;
            std::int64_t MinimumGridY = 0;
            std::int64_t MaximumGridX = 0;
            std::int64_t MaximumGridY = 0;
        };

        /** Keeps each selected candidate/source pair together until deterministic ordering. */
        struct SectionContributor
        {
            std::uint64_t Identity = 0;
            CandidateOccurrence Occurrence;
            CandidateSourceRecord Source;
        };

        /** Validates a finite, positive rectangle with finite, distinct edges. */
        static bool IsValidSectionRegion(const CollectionRegion& region) noexcept
        {
            const double right = region.xMapUnits + region.widthMapUnits;
            const double bottom = region.yMapUnits + region.heightMapUnits;
            return std::isfinite(region.xMapUnits) && std::isfinite(region.yMapUnits) &&
                std::isfinite(region.widthMapUnits) && std::isfinite(region.heightMapUnits) &&
                region.widthMapUnits > 0.0 && region.heightMapUnits > 0.0 &&
                std::isfinite(right) && std::isfinite(bottom) &&
                right > region.xMapUnits && bottom > region.yMapUnits;
        }

        /** Converts a service rectangle origin to its exact zero-origin grid index. */
        static bool TryGetSectionGridIndex(
            double coordinate,
            double sectionSize,
            std::int64_t& gridIndex) noexcept
        {
            const double ratio = coordinate / sectionSize;
            if (!std::isfinite(ratio))
            {
                return false;
            }
            const double rounded = std::round(ratio);
            constexpr double maximumExactInteger = 9007199254740991.0;
            if (rounded < -maximumExactInteger || rounded > maximumExactInteger)
            {
                return false;
            }
            const auto candidateIndex = static_cast<std::int64_t>(rounded);
            if (static_cast<double>(candidateIndex) * sectionSize != coordinate)
            {
                return false;
            }
            gridIndex = candidateIndex;
            return true;
        }

        /** Checks that a requested Section lies within the acquired region's exact rectangle. */
        bool IsSectionInsideAcquisition(const CollectionRegion& sectionRegion) const noexcept
        {
            const double acquisitionRight = RequestedRegion.X + RequestedRegion.Width;
            const double acquisitionBottom = RequestedRegion.Y + RequestedRegion.Height;
            const double sectionRight = sectionRegion.xMapUnits + sectionRegion.widthMapUnits;
            const double sectionBottom = sectionRegion.yMapUnits + sectionRegion.heightMapUnits;
            return CommittedCoverageValid &&
                CoverageWorld == World && CoverageManager == Manager && CoverageSession == Generation &&
                CoverageRequest != 0 && CoverageContentHash == AcquisitionContentHash &&
                sectionRegion.xMapUnits >= RequestedRegion.X &&
                sectionRegion.yMapUnits >= RequestedRegion.Y &&
                sectionRight <= acquisitionRight && sectionBottom <= acquisitionBottom &&
                sectionRegion.xMapUnits >= CommittedCoverage.X &&
                sectionRegion.yMapUnits >= CommittedCoverage.Y &&
                sectionRight <= CommittedCoverage.X + CommittedCoverage.Width &&
                sectionBottom <= CommittedCoverage.Y + CommittedCoverage.Height;
        }

        /** Validates common square size, acquisition containment and exact grid alignment. */
        void BuildRequestedSectionLookup(
            std::span<const CollectionRegion> sectionRegions,
            RequestedSectionLookup& lookup) const
        {
            if (sectionRegions.empty())
            {
                return;
            }
            if (!IsValidSectionRegion(sectionRegions.front()))
            {
                throw std::invalid_argument("Section request contains an invalid rectangle.");
            }
            lookup.SectionSize = sectionRegions.front().widthMapUnits;
            if (sectionRegions.front().heightMapUnits != lookup.SectionSize)
            {
                throw std::invalid_argument("Section requests must be square grid cells.");
            }
            for (std::size_t index = 0; index < sectionRegions.size(); ++index)
            {
                const CollectionRegion& region = sectionRegions[index];
                if (!IsValidSectionRegion(region) ||
                    region.widthMapUnits != lookup.SectionSize ||
                    region.heightMapUnits != lookup.SectionSize ||
                    !IsSectionInsideAcquisition(region))
                {
                    throw std::invalid_argument(
                        "Section batch has incompatible rectangles or exceeds acquired coverage.");
                }
                SectionCellCoordinate coordinate;
                if (!TryGetSectionGridIndex(region.xMapUnits, lookup.SectionSize, coordinate.X) ||
                    !TryGetSectionGridIndex(region.yMapUnits, lookup.SectionSize, coordinate.Y))
                {
                    throw std::invalid_argument("Section rectangle is not aligned to the service grid.");
                }
                lookup.SectionsByRow[coordinate.Y][coordinate.X].push_back(index);
                UpdateRequestedSectionBounds(region, coordinate, lookup);
            }
        }

        /** Expands the lookup's bounding envelope while preserving the sparse row index. */
        static void UpdateRequestedSectionBounds(
            const CollectionRegion& region,
            const SectionCellCoordinate& coordinate,
            RequestedSectionLookup& lookup) noexcept
        {
            const double right = region.xMapUnits + region.widthMapUnits;
            const double bottom = region.yMapUnits + region.heightMapUnits;
            if (!lookup.HasBounds)
            {
                lookup.MinimumX = region.xMapUnits;
                lookup.MinimumY = region.yMapUnits;
                lookup.MaximumRight = right;
                lookup.MaximumBottom = bottom;
                lookup.MinimumGridX = coordinate.X;
                lookup.MinimumGridY = coordinate.Y;
                lookup.MaximumGridX = coordinate.X;
                lookup.MaximumGridY = coordinate.Y;
                lookup.HasBounds = true;
                return;
            }
            lookup.MinimumX = (std::min)(lookup.MinimumX, region.xMapUnits);
            lookup.MinimumY = (std::min)(lookup.MinimumY, region.yMapUnits);
            lookup.MaximumRight = (std::max)(lookup.MaximumRight, right);
            lookup.MaximumBottom = (std::max)(lookup.MaximumBottom, bottom);
            lookup.MinimumGridX = (std::min)(lookup.MinimumGridX, coordinate.X);
            lookup.MinimumGridY = (std::min)(lookup.MinimumGridY, coordinate.Y);
            lookup.MaximumGridX = (std::max)(lookup.MaximumGridX, coordinate.X);
            lookup.MaximumGridY = (std::max)(lookup.MaximumGridY, coordinate.Y);
        }

        /** Bounds one building's reached cells to requested grid extents without overflow. */
        static bool TryGetReachedGridRange(
            double position,
            double reach,
            double sectionSize,
            double minimumWorld,
            double maximumWorld,
            std::int64_t minimumGrid,
            std::int64_t maximumGrid,
            std::int64_t& firstGrid,
            std::int64_t& lastGrid) noexcept
        {
            const double reachMinimum = position - reach;
            const double reachMaximum = position + reach;
            if (reachMaximum < minimumWorld || reachMinimum > maximumWorld)
            {
                return false;
            }
            const double clippedMinimum = (std::max)(reachMinimum, minimumWorld);
            const double clippedMaximum = (std::min)(reachMaximum, maximumWorld);
            const double minimumCell = std::floor(clippedMinimum / sectionSize) - 1.0;
            const double maximumCell = std::floor(clippedMaximum / sectionSize) + 1.0;
            firstGrid = (std::max)(minimumGrid, static_cast<std::int64_t>(minimumCell));
            lastGrid = (std::min)(maximumGrid, static_cast<std::int64_t>(maximumCell));
            return firstGrid <= lastGrid;
        }

        /** Applies exact half-open rectangle membership and the approved circular reach test. */
        static bool IsContributorForSection(
            const CollectionRegion& sectionRegion,
            double positionX,
            double positionY,
            double resolvedReach) noexcept
        {
            const double right = sectionRegion.xMapUnits + sectionRegion.widthMapUnits;
            const double bottom = sectionRegion.yMapUnits + sectionRegion.heightMapUnits;
            if (positionX >= sectionRegion.xMapUnits && positionX < right &&
                positionY >= sectionRegion.yMapUnits && positionY < bottom)
            {
                return true;
            }
            const double distanceX = (std::max)((std::max)(sectionRegion.xMapUnits - positionX, 0.0),
                positionX - right);
            const double distanceY = (std::max)((std::max)(sectionRegion.yMapUnits - positionY, 0.0),
                positionY - bottom);
            return std::hypot(distanceX, distanceY) <= resolvedReach;
        }

        /** Retains only contributors not excluded by established positive collector policy. */
        bool ShouldIncludeCandidate(const Facts::Building& building) const
        {
            const auto& definition = building.DefinitionFacts;
            if (definition && definition->InteriorStatus == Status::Ready && definition->InteriorOnly)
            {
                return false;
            }
            return IncludeDroneRails || !IsDroneRail(building);
        }

        /** Copies the neutral candidate fields from one completed retained occurrence. */
        static CandidateOccurrence MakeCandidateOccurrence(const Facts::Building& building)
        {
            CandidateOccurrence occurrence;
            occurrence.hasMapPosition = BuildingCollectorV2::HasValidMapPosition(building);
            if (!occurrence.hasMapPosition)
            {
                throw std::runtime_error("Committed building occurrence has no finite XY origin.");
            }
            occurrence.xMapUnits = building.Position[0];
            occurrence.yMapUnits = building.Position[1];
            occurrence.nativeFactsComplete = building.Complete;
            occurrence.isPendingOrUnresolved = !building.Complete;
            return occurrence;
        }

        /** Holds conservative grid bounds reached by one building's XY origin and radius. */
        struct SectionGridRange
        {
            std::int64_t FirstX = 0;
            std::int64_t LastX = 0;
            std::int64_t FirstY = 0;
            std::int64_t LastY = 0;
        };

        /** Derives bounded grid ranges after clipping the reach box to requested Sections. */
        static bool TryGetReachedSectionGridRange(
            const Facts::Building& building,
            const CandidateOccurrence& occurrence,
            const RequestedSectionLookup& lookup,
            SectionGridRange& range)
        {
            if (!std::isfinite(building.ResolvedReach) || building.ResolvedReach < 0.0)
            {
                throw std::runtime_error("Committed building has an invalid resolved reach.");
            }
            if (!TryGetReachedGridRange(occurrence.xMapUnits, building.ResolvedReach, lookup.SectionSize,
                    lookup.MinimumX, lookup.MaximumRight, lookup.MinimumGridX, lookup.MaximumGridX,
                    range.FirstX, range.LastX) ||
                !TryGetReachedGridRange(occurrence.yMapUnits, building.ResolvedReach, lookup.SectionSize,
                    lookup.MinimumY, lookup.MaximumBottom, lookup.MinimumGridY, lookup.MaximumGridY,
                    range.FirstY, range.LastY))
            {
                return false;
            }
            return true;
        }

        /** Collects exact requested Section indices reached by one finite origin and radius. */
        static void FindMatchingSections(
            const Facts::Building& building,
            const CandidateOccurrence& occurrence,
            const SectionGridRange& range,
            std::span<const CollectionRegion> sectionRegions,
            const RequestedSectionLookup& lookup,
            std::vector<std::size_t>& matchingSections)
        {
            matchingSections.clear();
            auto row = lookup.SectionsByRow.lower_bound(range.FirstY);
            while (row != lookup.SectionsByRow.end() && row->first <= range.LastY)
            {
                auto section = row->second.lower_bound(range.FirstX);
                while (section != row->second.end() && section->first <= range.LastX)
                {
                    for (std::size_t sectionIndex : section->second)
                    {
                        if (IsContributorForSection(sectionRegions[sectionIndex],
                            occurrence.xMapUnits, occurrence.yMapUnits, building.ResolvedReach))
                        {
                            matchingSections.push_back(sectionIndex);
                        }
                    }
                    ++section;
                }
                ++row;
            }
        }

        /** Appends one pre-converted occurrence and hash contribution to its exact Sections. */
        static void AppendContributorToSections(
            const Facts::Building& building,
            const CandidateOccurrence& occurrence,
            const CandidateSourceRecord& source,
            std::uint64_t contentHash,
            const std::vector<std::size_t>& matchingSections,
            std::vector<std::vector<SectionContributor>>& contributorsBySection,
            std::vector<AcquisitionHashAggregate>& hashesBySection)
        {
            const std::uint64_t identity = Key({building.Index, building.Serial});
            for (std::size_t sectionIndex : matchingSections)
            {
                contributorsBySection[sectionIndex].push_back({identity, occurrence, source});
                hashesBySection[sectionIndex].Add(contentHash);
            }
        }

        /** Freezes each Section after identity sorting and contiguous local ordinal assignment. */
        static std::vector<std::shared_ptr<const CandidatePublication>> FreezeSectionPublications(
            std::span<const CollectionRegion> sectionRegions,
            std::vector<std::vector<SectionContributor>>& contributorsBySection,
            const std::vector<AcquisitionHashAggregate>& hashesBySection)
        {
            std::vector<std::shared_ptr<const CandidatePublication>> publications;
            publications.reserve(sectionRegions.size());
            for (std::size_t sectionIndex = 0; sectionIndex < sectionRegions.size(); ++sectionIndex)
            {
                auto& contributors = contributorsBySection[sectionIndex];
                std::sort(contributors.begin(), contributors.end(), [](const auto& left, const auto& right)
                {
                    return left.Identity < right.Identity;
                });
                std::vector<CandidateOccurrence> occurrences;
                std::vector<CandidateSourceRecord> sourceRecords;
                occurrences.reserve(contributors.size());
                sourceRecords.reserve(contributors.size());
                for (const SectionContributor& contributor : contributors)
                {
                    CandidateOccurrence occurrence = contributor.Occurrence;
                    occurrence.publicationOrdinal = static_cast<std::uint64_t>(occurrences.size());
                    occurrences.push_back(std::move(occurrence));
                    sourceRecords.push_back(contributor.Source);
                }
                publications.push_back(std::make_shared<const CandidatePublication>(
                    sectionRegions[sectionIndex], std::move(occurrences), std::move(sourceRecords),
                    FinalizeAcquisitionHash(hashesBySection[sectionIndex])));
            }
            return publications;
        }

        /** Builds ordered immutable Section results from the currently committed regional dataset. */
        std::vector<std::shared_ptr<const CandidatePublication>> BuildSectionPublications(
            std::span<const CollectionRegion> sectionRegions) const
        {
            if (sectionRegions.empty())
            {
                return {};
            }
            if (!QueryConstructed || !HasRegion || !MembershipComplete || !CommittedCoverageValid ||
                CoverageWorld != World || CoverageManager != Manager || CoverageSession != Generation ||
                CoverageRequest == 0 || CoverageContentHash != AcquisitionContentHash ||
                !IsRegionContained(RequestedRegion, CommittedCoverage))
            {
                throw ::Services::Map::Buildings::SectionPublicationUnavailable(
                    "No successful committed regional acquisition is available.");
            }
            RequestedSectionLookup lookup;
            BuildRequestedSectionLookup(sectionRegions, lookup);
            std::vector<std::vector<SectionContributor>> contributorsBySection(sectionRegions.size());
            std::vector<AcquisitionHashAggregate> hashesBySection(sectionRegions.size());
            std::vector<std::size_t> matchingSections;
            matchingSections.reserve(8);
            for (const auto& entity : Entities)
            {
                const Facts::Building& building = entity.second.Data;
                if (!ShouldIncludeCandidate(building))
                {
                    continue;
                }
                CandidateOccurrence occurrence = MakeCandidateOccurrence(building);
                SectionGridRange reachedRange;
                if (!TryGetReachedSectionGridRange(building, occurrence, lookup, reachedRange))
                {
                    continue;
                }
                FindMatchingSections(building, occurrence, reachedRange, sectionRegions, lookup, matchingSections);
                if (matchingSections.empty())
                {
                    continue;
                }
                CandidateSourceRecord source = BuildCandidateSourceRecord(building);
                const std::uint64_t preliminary = ComputePreliminaryAcquisitionContribution(building);
                const std::uint64_t contentHash = CompleteAcquisitionContribution(preliminary, building);
                AppendContributorToSections(building, occurrence, source, contentHash, matchingSections,
                    contributorsBySection, hashesBySection);
            }
            return FreezeSectionPublications(sectionRegions, contributorsBySection, hashesBySection);
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Emits collector totals and acquisition phases for one completed interval. */
        void ReportCollectorMeasurements(double intervalMs) const
        {
            const auto& d = Diagnostics;
            LOG_INFO("MiniMap: BuildingMap collector intervalMs=%.1f updates={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f} membership={totalMs:%.3f meanMs:%.3f maxMs:%.3f} deferredAcquisition={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f pending:%llu}",
                intervalMs,
                static_cast<unsigned long long>(d.Update.Samples), d.Update.TotalMs, d.Update.MeanMs(), d.Update.MaxMs,
                d.UpdateMembership.TotalMs, d.UpdateMembership.MeanMs(), d.UpdateMembership.MaxMs,
                static_cast<unsigned long long>(d.Acquisition.Samples), d.Acquisition.TotalMs, d.Acquisition.MeanMs(), d.Acquisition.MaxMs,
                static_cast<unsigned long long>(Pending.size()));
            LOG_INFO("MiniMap: BuildingMap collector_phases intervalMs=%.1f chunkExecution={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f} ownedCopy={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f} reconciliation={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f}",
                intervalMs,
                static_cast<unsigned long long>(d.ChunkExecution.Samples), d.ChunkExecution.TotalMs, d.ChunkExecution.MeanMs(), d.ChunkExecution.MaxMs,
                static_cast<unsigned long long>(d.OwnedFactCopying.Samples), d.OwnedFactCopying.TotalMs, d.OwnedFactCopying.MeanMs(), d.OwnedFactCopying.MaxMs,
                static_cast<unsigned long long>(d.RegionalBookkeeping.Samples), d.RegionalBookkeeping.TotalMs, d.RegionalBookkeeping.MeanMs(), d.RegionalBookkeeping.MaxMs);
        }

        /** Emits membership phase and outcome totals for one completed interval. */
        void ReportMembershipMeasurements(double intervalMs) const
        {
            const auto& d = Diagnostics;
            LOG_INFO("MiniMap: BuildingMap regional_scan intervalMs=%.1f scans:%llu complete:%llu scan={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f} chunks:%llu examined:%llu admitted:%llu missingOrigin:%llu added:%llu evicted:%llu failed:%llu entities:%llu pending:%llu requests={equal:%llu changed:%llu contained:%llu immediate:%llu} reuse={success:%llu rejected:%llu} scanReason={immediate:%llu mutation:%llu fallback:%llu} failedByReason={immediate:%llu mutation:%llu fallback:%llu}",
                intervalMs, static_cast<unsigned long long>(d.MembershipScans),
                static_cast<unsigned long long>(d.MembershipScans - d.FailedScans),
                static_cast<unsigned long long>(d.Membership.Samples), d.Membership.TotalMs, d.Membership.MeanMs(), d.Membership.MaxMs,
                static_cast<unsigned long long>(d.Chunks), static_cast<unsigned long long>(d.Examined),
                static_cast<unsigned long long>(d.Admitted), static_cast<unsigned long long>(d.MissingOrigin),
                static_cast<unsigned long long>(d.Added), static_cast<unsigned long long>(d.Removed),
                static_cast<unsigned long long>(d.FailedScans), static_cast<unsigned long long>(Entities.size()),
                static_cast<unsigned long long>(Pending.size()),
                static_cast<unsigned long long>(d.EqualRegionRequests),
                static_cast<unsigned long long>(d.ChangedRegionRequests),
                static_cast<unsigned long long>(d.ContainedRegionRequests),
                static_cast<unsigned long long>(d.ImmediateRegionRequests),
                static_cast<unsigned long long>(d.SuccessfulCoverageReuse),
                static_cast<unsigned long long>(d.RejectedCoverageReuse),
                static_cast<unsigned long long>(d.ImmediateScans),
                static_cast<unsigned long long>(d.MutationScans),
                static_cast<unsigned long long>(d.PeriodicScans),
                static_cast<unsigned long long>(d.FailedImmediateScans),
                static_cast<unsigned long long>(d.FailedMutationScans),
                static_cast<unsigned long long>(d.FailedFallbackScans));
            const bool mutationPending = ObservedMutationRevision > AcknowledgedMutationRevision;
            const auto now = Clock::now();
            const auto quietDeadline = MutationQuietDeadline();
            const auto quietRemaining = mutationPending && quietDeadline > now
                ? std::chrono::duration_cast<std::chrono::milliseconds>(quietDeadline - now).count()
                : 0;
            const auto quietElapsed = mutationPending && now > quietDeadline
                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - quietDeadline).count()
                : 0;
            LOG_INFO("MiniMap: BuildingMap mutation_schedule latestRevision:%llu acknowledgedRevision:%llu pending:%u quietRemainingMs:%lld quietElapsedMs:%lld",
                static_cast<unsigned long long>(ObservedMutationRevision),
                static_cast<unsigned long long>(AcknowledgedMutationRevision),
                static_cast<unsigned>(mutationPending),
                static_cast<long long>(quietRemaining), static_cast<long long>(quietElapsed));
        }

        /** Emits the numeric-only worst scan retained for one completed interval. */
        void ReportWorstMembershipScan(double intervalMs) const
        {
            const auto& d = Diagnostics;
            if (d.WorstScan.TotalMs > 0.0)
            {
                const auto& worst = d.WorstScan;
                LOG_INFO("MiniMap: BuildingMap regional_scan_worst intervalMs=%.1f totalMs:%.3f chunkMs:%.3f ownedCopyMs:%.3f deferredMs:%.3f reconcileMs:%.3f chunks:%llu entities:%llu admitted:%llu pending:%llu added:%llu evicted:%llu",
                    intervalMs, worst.TotalMs, worst.ChunkExecutionMs, worst.OwnedFactCopyingMs,
                    worst.DeferredAcquisitionMs, worst.ReconciliationMs,
                    static_cast<unsigned long long>(worst.Chunks), static_cast<unsigned long long>(worst.Entities),
                    static_cast<unsigned long long>(worst.Admitted),
                    static_cast<unsigned long long>(worst.Pending), static_cast<unsigned long long>(worst.Added),
                    static_cast<unsigned long long>(worst.Removed));
            }
        }

        /** Emits and resets ten-second aggregates only from a permitted Update call. */
        void ReportMeasurements() noexcept
        {
            const auto now = Clock::now();
            if (Diagnostics.IntervalStart == Clock::time_point{})
            {
                Diagnostics.IntervalStart = now;
                return;
            }
            const double intervalMs = std::chrono::duration<double, std::milli>(now - Diagnostics.IntervalStart).count();
            if (intervalMs < 10000.0)
            {
                return;
            }
            try
            {
                ReportCollectorMeasurements(intervalMs);
                ReportMembershipMeasurements(intervalMs);
                ReportWorstMembershipScan(intervalMs);
            }
            catch (...)
            {
                Diagnostics.Reset();
                return;
            }
            Diagnostics.Reset();
        }
#endif
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

    /** Exposes a finite XY origin independently of rotation, scale or Z validity. */
    bool BuildingCollectorV2::HasValidMapPosition(const BuildingAcquiredFacts::Building& building) noexcept
    {
        return std::isfinite(building.Position[0]) && std::isfinite(building.Position[1]);
    }

    /** Creates inert CPU state and validates the immutable membership interval. */
    BuildingCollectorV2::BuildingCollectorV2(std::chrono::milliseconds retryInterval)
        : state_(std::make_unique<State>(retryInterval))
    {
    }

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

    /** Accepts the exact half-open rectangle used by this collector's regional admission request. */
    bool BuildingCollectorV2::SetAcquisitionRegion(const Facts::Region& region) noexcept
    {
        return state_->AcceptRequestedRegion(region);
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

    /** Sets whether newly requested Section publications include known drone rails. */
    void BuildingCollectorV2::SetRenderDroneRails(bool enabled) noexcept
    {
        state_->IncludeDroneRails = enabled;
    }

    /** Reads the stored hash for the currently committed regional dataset. */
    std::uint64_t BuildingCollectorV2::AcquisitionHash() const noexcept
    {
        return state_->AcquisitionContentHash;
    }

    /** Builds exact Section publications from the last successful committed regional scan. */
    std::vector<std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>>
    BuildingCollectorV2::AcquireSectionPublications(
        std::span<const ::Services::Map::Buildings::BuildingCollectionRegion> sectionRegions) const
    {
        return state_->BuildSectionPublications(sectionRegions);
    }

    /** Advances native acquisition using MiniMap's established gameplay world. */
    void BuildingCollectorV2::Update()
    {
        std::uint64_t mutationRevision = 0;
        if (!MiniMapMassLifecycle::MassLifecycle::TryGetAcceptedMutationRevision(mutationRevision))
            return;
        const auto* native = MiniMapNative::Get();
        if (!native)
        {
            state_->InvalidateCoverageForUnavailableSource();
            return;
        }
        if (!native->player.isGameThread())
            return;
        SDK::UWorld* world = MiniMapMap::GetWorld();
        if (!world)
        {
            state_->InvalidateCoverageForUnavailableSource();
            return;
        }
        try
        {
            if (!state_->Types || !state_->EnsureSession(*native, world))
                return;
            if (!state_->PrepareCurrentUpdate(*native, world))
                return;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto updateStarted = State::Clock::now();
            const auto membershipStarted = updateStarted;
#endif
            const auto context = state_->CaptureContext();
            BM124D_RUN("regional Mass scan", state_->ReconcileIfDue(*native));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            if (state_->Generation == context.Session)
            {
                state_->Diagnostics.UpdateMembership.Add(
                    std::chrono::duration<double, std::milli>(State::Clock::now() - membershipStarted).count());
            }
#endif
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            if (state_->Generation == context.Session)
            {
                state_->Diagnostics.Update.Add(
                    std::chrono::duration<double, std::milli>(State::Clock::now() - updateStarted).count());
                state_->ReportMeasurements();
            }
#endif
        }
        catch (const std::bad_alloc&)
        {
            // Retain the last committed dataset; failed scans remain unavailable through MembershipComplete.
            state_->MembershipComplete = false;
            state_->CommittedCoverageValid = false;
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

}
#endif
