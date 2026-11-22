// Acquires StarRupture building facts, distributes neutral Section values, and retains immutable publications.
// All ABI-specific access stays here; Section publications contain no native ownership.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingMapLoadingTrace.h"
#include "DiagnosticExports.h"
#include "plugin_helpers.h"
#include "BuildingCollectorV2.h"
#include "BuildingSpatialAdmission.h"
#include "Native/MassLifecycle.h"
#include "SplinePolicyId.h"
#include "../Map.h"
#include "Services/Map/Buildings/BuildingCandidatePublication.h"
#include "BuildingDefinitionAcquisition.h"
#include "BuildingSplineSource.h"
#include "Map/RuntimeFieldAccess.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/AuActorPlacement_classes.hpp>
#include <SDK/AuActorPlacement_structs.hpp>
#include <SDK/Chimera_classes.hpp>
#include <SDK/CoreUObject_classes.hpp>
#include <SDK/CoreUObject_structs.hpp>
#include <SDK/MassEntity_structs.hpp>
#include "Native/ReadbackUEConfiguration.h"
#include <MassExecutionContext.h>
#include <MassArchetypeTypes.h>
#include <algorithm>
#include <array>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "Services/Map/Buildings/BuildingMapPerformanceDiagnostics.h"
#include <array>
#endif
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
#include <string>
#include <string_view>
#include <type_traits>
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
    using SplineEnumNameValue = SDK::TPair<SDK::FName, std::int64_t>;

    /** Mirrors the confirmed native three-field TArray header for compile-time layout checks. */
    struct SplineEnumArrayLayout
    {
        void* Data;
        std::int32_t Num;
        std::int32_t Max;
    };

    /** Owns one copied enum name/value pair without SDK container constructors. */
    struct SplineEnumNameValueCopy
    {
        SDK::FName Name;
        std::int64_t Value;
    };

    static_assert(sizeof(SDK::FMassEntityQuery) == 0x350,
        "BuildingCollectorV2 fixed query storage must match the generated Client SDK query size");
    static_assert(alignof(SDK::FMassEntityQuery) == 0x10,
        "BuildingCollectorV2 fixed query storage must match the generated Client SDK query alignment");
    static_assert(sizeof(FMassExecutionContext) == 0x4E0);
    static_assert(alignof(FMassExecutionContext) == 8);
    static_assert(sizeof(FMassExecuteFunction) == 0x30);
    static_assert(sizeof(::FMassEntityHandle) == sizeof(SDK::FMassEntityHandle));
    static_assert(alignof(::FMassEntityHandle) == alignof(SDK::FMassEntityHandle));
    static_assert(sizeof(SDK::FMassEntityHandle) == 0x08);
    static_assert(alignof(SDK::FMassEntityHandle) == 0x08);
    static_assert(offsetof(SDK::FMassEntityHandle, Index) == 0x00);
    static_assert(offsetof(SDK::FMassEntityHandle, SerialNumber) == 0x04);
    static_assert(sizeof(decltype(SDK::FMassEntityHandle::Index)) == 0x04);
    static_assert(sizeof(decltype(SDK::FMassEntityHandle::SerialNumber)) == 0x04);
    static_assert(std::is_signed_v<decltype(SDK::FMassEntityHandle::Index)>);
    static_assert(std::is_signed_v<decltype(SDK::FMassEntityHandle::SerialNumber)>);
    static_assert(sizeof(EntityId) == 0x08);
    static_assert(offsetof(EntityId, Index) == 0x00);
    static_assert(offsetof(EntityId, SerialNumber) == 0x04);
    static_assert(sizeof(decltype(EntityId::Index)) == 0x04);
    static_assert(sizeof(decltype(EntityId::SerialNumber)) == 0x04);
    static_assert(std::is_signed_v<decltype(EntityId::Index)>);
    static_assert(std::is_signed_v<decltype(EntityId::SerialNumber)>);
    static_assert(offsetof(::FMassEntityHandle, Index) == 0x00);
    static_assert(offsetof(::FMassEntityHandle, SerialNumber) == 0x04);
    static_assert(std::is_signed_v<decltype(::FMassEntityHandle::Index)>);
    static_assert(std::is_signed_v<decltype(::FMassEntityHandle::SerialNumber)>);
    static_assert(sizeof(SDK::FTransformFragment) == 0x60);
    static_assert(alignof(SDK::FTransformFragment) == 0x10);
    static_assert(offsetof(SDK::FTransformFragment, Transform) == 0x00);
    static_assert(sizeof(decltype(SDK::FTransformFragment::Transform)) == 0x60);
    static_assert(sizeof(SDK::FTransform) == 0x60);
    static_assert(alignof(SDK::FTransform) == 0x10);
    static_assert(offsetof(SDK::FTransform, Rotation) == 0x00);
    static_assert(sizeof(decltype(SDK::FTransform::Rotation)) == 0x20);
    static_assert(offsetof(SDK::FTransform, Translation) == 0x20);
    static_assert(sizeof(decltype(SDK::FTransform::Translation)) == 0x18);
    static_assert(offsetof(SDK::FTransform, Scale3D) == 0x40);
    static_assert(sizeof(decltype(SDK::FTransform::Scale3D)) == 0x18);
    static_assert(sizeof(SDK::FAuSplineConnectionFragment) == 0xE0);
    static_assert(alignof(SDK::FAuSplineConnectionFragment) == 0x08);
    static_assert(sizeof(decltype(SDK::FAuSplineConnectionFragment::Data)) == 0x50);
    static_assert(sizeof(decltype(SDK::FAuSplineConnectionFragment::SplineCurves)) == 0x68);
    static_assert(sizeof(decltype(SDK::FAuSplineConnectionFragment::bSplineCurvesInitialized)) == 0x01);
    static_assert(sizeof(SDK::FAuAPMassFragment) == 0x50);
    static_assert(alignof(SDK::FAuAPMassFragment) == 0x08);
    static_assert(sizeof(decltype(SDK::FAuAPMassFragment::Payload)) == 0x20);
    static_assert(sizeof(SDK::FCrBuildingParameters) == 0x18);
    static_assert(alignof(SDK::FCrBuildingParameters) == 0x08);
    static_assert(sizeof(decltype(SDK::FCrBuildingParameters::PlacementData)) == 0x08);
    static_assert(sizeof(SDK::FAuAPPayloadDataHandle) == 0x20);
    static_assert(sizeof(SDK::UObject) == 0x28);
    static_assert(offsetof(SDK::UObject, Class) == 0x10);
    static_assert(offsetof(SDK::UStruct, Size) == 0x58);
    static_assert(sizeof(SDK::UEnum) == 0x68);
    static_assert(offsetof(SDK::UEnum, Names) == 0x40);
    static_assert(sizeof(decltype(SDK::UEnum::Names)) == 0x10);
    static_assert(sizeof(SplineEnumArrayLayout) == sizeof(decltype(SDK::UEnum::Names)));
    static_assert(offsetof(SplineEnumArrayLayout, Data) == 0x00);
    static_assert(offsetof(SplineEnumArrayLayout, Num) == 0x08);
    static_assert(offsetof(SplineEnumArrayLayout, Max) == 0x0C);
    static_assert(sizeof(SplineEnumNameValue) == 0x10);
    static_assert(offsetof(SplineEnumNameValue, First) == 0x00);
    static_assert(offsetof(SplineEnumNameValue, Second) == 0x08);
    static_assert(sizeof(SplineEnumNameValueCopy) == sizeof(SplineEnumNameValue));
    static_assert(offsetof(SplineEnumNameValueCopy, Name) == 0x00);
    static_assert(offsetof(SplineEnumNameValueCopy, Value) == 0x08);
    static_assert(std::is_trivially_copyable_v<SDK::FName>);
    static_assert(std::is_trivially_copyable_v<SplineEnumNameValueCopy>);
    static_assert(sizeof(SDK::FName) == 0x08);
    static_assert(offsetof(SDK::FName, ComparisonIndex) == 0x00);
    static_assert(offsetof(SDK::FName, Number) == 0x04);
    static_assert(alignof(SDK::FAuAPPayloadDataHandle) == 0x08);
    static_assert(offsetof(SDK::FAuAPPayloadDataHandle, Type) == 0x10);
    static_assert(offsetof(SDK::FAuAPPayloadDataHandle, Revision) == 0x18);

    /** Sole mutable acquisition owner; publications are frozen CPU projections only. */
    struct BuildingCollectorV2::State
    {
        using Clock = std::chrono::steady_clock;
        static constexpr auto MutationQuietPeriod = std::chrono::milliseconds(100);

        /** Classifies a collector scan for bounded success/failure diagnostics. */
        enum class AcquisitionReason
        {
            Immediate,
            Mutation
        };

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Identifies source-fact groups that changed on a retained occurrence. */
        enum class AcquiredFactChange : std::uint32_t
        {
            Identity = 1U << 0,
            Spatial = 1U << 1,
            EntityStatus = 1U << 2,
            DefinitionAvailability = 1U << 3,
            DefinitionValues = 1U << 4,
            Interior = 1U << 5,
            SplineClassification = 1U << 6,
            SplineFragment = 1U << 7,
            SplineCurveVersion = 1U << 8,
            SplinePosition = 1U << 9,
            SplineRotation = 1U << 10,
            SplineScale = 1U << 11,
            SplineReparameterization = 1U << 12,
            SplineInitialization = 1U << 13,
            SplineProfiles = 1U << 14,
            SplinePayload = 1U << 15,
            Other = 1U << 16
        };

        /** Aggregates only changed retained occurrences for one completed acquisition. */
        struct AcquiredFactChangeSummary
        {
            struct Representative
            {
                std::int32_t Index = -1;
                std::int32_t Serial = 0;
                std::uint32_t Mask = 0;
            };

            std::uint64_t RetainedExamined = 0;
            std::uint64_t Changed = 0;
            std::uint64_t Identity = 0;
            std::uint64_t Spatial = 0;
            std::uint64_t EntityStatus = 0;
            std::uint64_t DefinitionAvailability = 0;
            std::uint64_t DefinitionValues = 0;
            std::uint64_t Interior = 0;
            std::uint64_t SplineClassification = 0;
            std::uint64_t SplineFragment = 0;
            std::uint64_t SplineCurveVersion = 0;
            std::uint64_t SplinePosition = 0;
            std::uint64_t SplineRotation = 0;
            std::uint64_t SplineScale = 0;
            std::uint64_t SplineReparameterization = 0;
            std::uint64_t SplineInitialization = 0;
            std::uint64_t SplineProfiles = 0;
            std::uint64_t SplinePayload = 0;
            std::uint64_t Other = 0;
            std::uint64_t CompleteTransitions = 0;
            std::uint64_t TransformChanges = 0;
            std::uint64_t BothUninitializedChannels = 0;
            std::uint64_t OldUninitializedChannels = 0;
            std::uint64_t BothInitializedChannels = 0;
            std::uint64_t OldInitializedChannels = 0;
            std::array<Representative, 4> Samples{};
            AcquisitionReason Reason = AcquisitionReason::Immediate;

            /** Counts one changed occurrence and keeps at most four identity/mask examples. */
            void Add(const Facts::Building& previous, const Facts::Building& current, std::uint32_t mask,
                bool splineChannelsChanged, bool transformChanged)
            {
                ++Changed;
                Identity += Has(mask, AcquiredFactChange::Identity);
                Spatial += Has(mask, AcquiredFactChange::Spatial);
                EntityStatus += Has(mask, AcquiredFactChange::EntityStatus);
                DefinitionAvailability += Has(mask, AcquiredFactChange::DefinitionAvailability);
                DefinitionValues += Has(mask, AcquiredFactChange::DefinitionValues);
                Interior += Has(mask, AcquiredFactChange::Interior);
                SplineClassification += Has(mask, AcquiredFactChange::SplineClassification);
                SplineFragment += Has(mask, AcquiredFactChange::SplineFragment);
                SplineCurveVersion += Has(mask, AcquiredFactChange::SplineCurveVersion);
                SplinePosition += Has(mask, AcquiredFactChange::SplinePosition);
                SplineRotation += Has(mask, AcquiredFactChange::SplineRotation);
                SplineScale += Has(mask, AcquiredFactChange::SplineScale);
                SplineReparameterization += Has(mask, AcquiredFactChange::SplineReparameterization);
                SplineInitialization += Has(mask, AcquiredFactChange::SplineInitialization);
                SplineProfiles += Has(mask, AcquiredFactChange::SplineProfiles);
                SplinePayload += Has(mask, AcquiredFactChange::SplinePayload);
                Other += Has(mask, AcquiredFactChange::Other);
                CompleteTransitions += !previous.Complete && current.Complete;
                TransformChanges += transformChanged;
                if (splineChannelsChanged)
                    CountSplineInitializationState(previous, current);
                if (Changed <= Samples.size())
                    Samples[Changed - 1] = {current.Index, current.Serial, mask};
            }

            /** Counts changed channels by the old and new native initialization flags. */
            void CountSplineInitializationState(const Facts::Building& previous, const Facts::Building& current)
            {
                const bool oldInitialized = previous.SplineFacts && previous.SplineFacts->CurvesInitialized;
                const bool newInitialized = current.SplineFacts && current.SplineFacts->CurvesInitialized;
                if (!oldInitialized && !newInitialized)
                    ++BothUninitializedChannels;
                else if (!oldInitialized && newInitialized)
                    ++OldUninitializedChannels;
                else if (oldInitialized && newInitialized)
                    ++BothInitializedChannels;
                else
                    ++OldInitializedChannels;
            }

            /** Reports whether a bit is present in one occurrence's change mask. */
            static bool Has(std::uint32_t mask, AcquiredFactChange change) noexcept
            {
                return (mask & static_cast<std::uint32_t>(change)) != 0;
            }
        };
#endif

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
            bool ReusedRetainedFacts = false;
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

        /** Hashes one source number using the shared six-decimal equivalence. */
        static void AppendAcquisitionDouble(std::uint64_t& hash, double value) noexcept
        {
            AppendAcquisitionInteger(hash, ::Services::Map::Buildings::SourceContentPrecision::CanonicalBits(value));
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

        /** Appends a normalized Mass quaternion with the established sign equivalence. */
        static void AppendAcquisitionQuaternion(
            std::uint64_t& hash, const Facts::Quaternion& rotation, bool signEquivalent) noexcept
        {
            const bool negate = signEquivalent &&
                ::Services::Map::Buildings::SourceContentPrecision::QuaternionNeedsNegation(rotation);
            for (double value : rotation)
            {
                AppendAcquisitionDouble(hash, negate ? -value : value);
            }
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

        /** Hashes callback-owned identity, transform, reach and spline values at source precision. */
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
            AppendAcquisitionQuaternion(hash, building.Rotation,
                building.TransformStatus == Facts::Status::Ready);
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

        /** Finalizes a multiset aggregate after every completed contribution is added. */
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
            std::uint64_t FastPathEligible = 0;
            std::uint64_t FullAcquisitionRequired = 0;
            std::uint64_t Pending = 0;
            std::uint64_t Added = 0;
            std::uint64_t Removed = 0;
        };

        /** Aggregates sampled wall time for one operation type without per-operation clocks. */
        struct SampledPublicationPhase
        {
            static constexpr std::uint64_t SampleInterval = 256;

            std::uint64_t Operations = 0;
            std::uint64_t Samples = 0;
            double SampledMilliseconds = 0.0;

            /** Counts one operation and samples its first and every 256th occurrence. */
            bool BeginOperation() noexcept
            {
                ++Operations;
                return Operations == 1 || Operations % SampleInterval == 0;
            }

            /** Adds one sampled duration; callers scale the mean by the exact operation count. */
            void AddSample(double milliseconds) noexcept
            {
                ++Samples;
                SampledMilliseconds += milliseconds;
            }

            /** Estimates total phase time from the sampled mean and exact operation count. */
            double EstimatedMilliseconds() const noexcept
            {
                return Samples == 0 ? 0.0 : SampledMilliseconds *
                    static_cast<double>(Operations) / static_cast<double>(Samples);
            }

            /** Merges bounded sample totals while retaining exact operation and sample counts. */
            void Accumulate(const SampledPublicationPhase& other) noexcept
            {
                Operations += other.Operations;
                Samples += other.Samples;
                SampledMilliseconds += other.SampledMilliseconds;
            }
        };

        /** Measures only sampled operations while keeping every operation count exact. */
        struct SampledPublicationPhaseTimer
        {
            SampledPublicationPhase& Phase;
            bool IsSampled = false;
            Clock::time_point Started{};

            explicit SampledPublicationPhaseTimer(SampledPublicationPhase& phase) noexcept
                : Phase(phase), IsSampled(phase.BeginOperation())
            {
                if (IsSampled)
                {
                    Started = Clock::now();
                }
            }

            ~SampledPublicationPhaseTimer() noexcept
            {
                if (IsSampled)
                {
                    Phase.AddSample(
                        std::chrono::duration<double, std::milli>(Clock::now() - Started).count());
                }
            }
        };

        /** Stores exact outer phase times, sampled nested estimates, and exact builder counts. */
        struct SectionPublicationBuildMetrics
        {
            double TotalMilliseconds = 0.0;
            double LookupMilliseconds = 0.0;
            double BoundsMilliseconds = 0.0;
            double EntityLoopMilliseconds = 0.0;
            double FreezeMilliseconds = 0.0;
            double IndexSortMilliseconds = 0.0;
            double MaterializationMilliseconds = 0.0;
            SampledPublicationPhase CandidatePolicy;
            SampledPublicationPhase CandidateOccurrence;
            SampledPublicationPhase ReachedRange;
            SampledPublicationPhase MatchingSections;
            SampledPublicationPhase SourceConversion;
            SampledPublicationPhase ContributorDistribution;
            std::uint64_t EntitiesVisited = 0;
            std::uint64_t PolicyExcluded = 0;
            std::uint64_t SpatialBoundsExcluded = 0;
            std::uint64_t NoMatchingSection = 0;
            std::uint64_t EntitiesConverted = 0;
            std::uint64_t DistinctContributors = 0;
            std::uint64_t ContributorCopies = 0;
            std::uint64_t SourceCopies = 0;
            std::uint64_t SourceMoves = 0;
            std::uint64_t RequestedSections = 0;
            std::uint64_t FinalOccurrences = 0;

            /** Adds one invocation to the interval's exact and sampled aggregate fields. */
            void Accumulate(const SectionPublicationBuildMetrics& other) noexcept
            {
                TotalMilliseconds += other.TotalMilliseconds;
                LookupMilliseconds += other.LookupMilliseconds;
                BoundsMilliseconds += other.BoundsMilliseconds;
                EntityLoopMilliseconds += other.EntityLoopMilliseconds;
                FreezeMilliseconds += other.FreezeMilliseconds;
                IndexSortMilliseconds += other.IndexSortMilliseconds;
                MaterializationMilliseconds += other.MaterializationMilliseconds;
                CandidatePolicy.Accumulate(other.CandidatePolicy);
                CandidateOccurrence.Accumulate(other.CandidateOccurrence);
                ReachedRange.Accumulate(other.ReachedRange);
                MatchingSections.Accumulate(other.MatchingSections);
                SourceConversion.Accumulate(other.SourceConversion);
                ContributorDistribution.Accumulate(other.ContributorDistribution);
                EntitiesVisited += other.EntitiesVisited;
                PolicyExcluded += other.PolicyExcluded;
                SpatialBoundsExcluded += other.SpatialBoundsExcluded;
                NoMatchingSection += other.NoMatchingSection;
                EntitiesConverted += other.EntitiesConverted;
                DistinctContributors += other.DistinctContributors;
                ContributorCopies += other.ContributorCopies;
                SourceCopies += other.SourceCopies;
                SourceMoves += other.SourceMoves;
                RequestedSections += other.RequestedSections;
                FinalOccurrences += other.FinalOccurrences;
            }

            /** Returns inclusive time outside the four exact, sequentially timed outer phases. */
            double ResidualMilliseconds() const noexcept
            {
                const double measured = LookupMilliseconds + BoundsMilliseconds +
                    EntityLoopMilliseconds + FreezeMilliseconds;
                return (std::max)(0.0, TotalMilliseconds - measured);
            }
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
            std::uint64_t FastPathEligible = 0;
            std::uint64_t FullAcquisitionRequired = 0;
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
            std::uint64_t ImmediateScans = 0;
            std::uint64_t MutationScans = 0;
            std::uint64_t FailedImmediateScans = 0;
            std::uint64_t FailedMutationScans = 0;
            std::uint64_t SectionOwnerReuses = 0;
            std::uint64_t SectionOwnersPreservedOnMovement = 0;
            std::uint64_t SectionPublicationsRebuilt = 0;
            std::uint64_t FullFootprintRebuilds = 0;
            std::uint64_t PublicationBuilderCalls = 0;
            std::uint64_t RepeatedQueriesWithoutRebuild = 0;
            double PublicationBuildMs = 0.0;
            SectionPublicationBuildMetrics PublicationBuildTotals;
            SectionPublicationBuildMetrics SlowestPublicationBuild;
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
            std::uint64_t FastPathEligible = 0;
            std::uint64_t FullAcquisitionRequired = 0;
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
                ::Services::Map::Buildings::BuildingMapPerformanceDiagnostics::Game().Scan.Add(totalMs);
                measurements.ChunkExecution.Add(ChunkExecutionMs);
                measurements.OwnedFactCopying.Add(OwnedFactCopyingMs);
                measurements.Acquisition.Add(DeferredAcquisitionMs);
                measurements.RegionalBookkeeping.Add(ReconciliationMs);
                measurements.Chunks += Chunks;
                measurements.Examined += Examined;
                measurements.Admitted += Admitted;
                measurements.FastPathEligible += FastPathEligible;
                measurements.FullAcquisitionRequired += FullAcquisitionRequired;
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
                }
                else if (Reason == AcquisitionReason::Immediate)
                {
                    ++measurements.FailedImmediateScans;
                }
                else if (Reason == AcquisitionReason::Mutation)
                {
                    ++measurements.FailedMutationScans;
                }
                if (totalMs >= measurements.WorstScan.TotalMs)
                {
                    measurements.WorstScan = {totalMs, ChunkExecutionMs, OwnedFactCopyingMs, DeferredAcquisitionMs,
                        ReconciliationMs, Chunks, Examined, Admitted, FastPathEligible,
                        FullAcquisitionRequired, Owner.Pending.size(), Added, Removed};
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

        static constexpr std::size_t MaxSplineEnumEntries = 1024;
        static constexpr std::uint32_t MaxSupportedSplineBuildingId = 4095;
        static constexpr std::array<std::pair<std::string_view, SplinePolicyId>, 25> SupportedSplineNames = {{
            {"ECrBuildingID::DroneRailT1", SplinePolicyId::DroneRail},
            {"ECrBuildingID::DroneRailT2", SplinePolicyId::DroneRail},
            {"ECrBuildingID::DroneRailT3", SplinePolicyId::DroneRail},
            {"ECrBuildingID::DroneRailT4", SplinePolicyId::DroneRail},
            {"ECrBuildingID::DroneRailT5", SplinePolicyId::DroneRail},
            {"ECrBuildingID::Zipline", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_250", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_320", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_390", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_460", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_530", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_600", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2_250", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2_320", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2_390", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2_460", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2_530", SplinePolicyId::Zipline},
            {"ECrBuildingID::Zipline_2x2_600", SplinePolicyId::Zipline},
            {"ECrBuildingID::ZipLine_OLD", SplinePolicyId::Zipline},
            {"ECrBuildingID::ZiplineSupport", SplinePolicyId::Zipline},
            {"ECrBuildingID::ZiplineSupport_2x2", SplinePolicyId::Zipline},
            {"ECrBuildingID::ZipRail", SplinePolicyId::Zipline},
            {"ECrBuildingID::Walkway", SplinePolicyId::Walkway},
            {"ECrBuildingID::RailingWalkway", SplinePolicyId::Walkway}
        }};

        /** Emits one initialization diagnostic when the service logger is available. */
        template<class... Args>
        static void LogSplinePolicy(Services::Level level, const char* format, Args... args) noexcept
        {
            if (auto* log = Services::TryLog())
                Services::Logf(*log, level, format, args...);
        }

        const std::chrono::milliseconds RetryInterval;
        std::vector<SplinePolicyId> SplinePolicyByBuildingId;
        bool SplinePolicyInitializationAttempted = false;

        /** Stores the immutable scan interval and rejects nonpositive values once. */
        explicit State(double sectionSizeMapUnits, std::chrono::milliseconds retryInterval)
            : RetryInterval(retryInterval)
            , SectionSizeMapUnits(sectionSizeMapUnits)
        {
            if (!std::isfinite(SectionSizeMapUnits) || SectionSizeMapUnits <= 0.0 ||
                RetryInterval <= std::chrono::milliseconds::zero())
            {
                throw std::invalid_argument("BuildingCollectorV2 requires positive finite Section size and retry interval.");
            }
        }

        /** Resolves the running build's supported enum names once before collection begins. */
        void InitializeSplinePolicyTable()
        {
            SplinePolicyInitializationAttempted = true;
            try
            {
                const auto entries = FindSplineEnumEntries();
                if (!entries.empty())
                    BuildSplinePolicyTable(entries);
            }
            catch (...)
            {
                SplinePolicyByBuildingId.clear();
                Services::LogMessage(Services::Level::Error,
                    "MiniMap: Spline policy initialization failed while reading ECrBuildingID.");
            }
        }

        /** Resolves and validates the resident enum object before copying its bounded name array. */
        std::vector<SplineEnumNameValueCopy> FindSplineEnumEntries()
        {
            auto* enumObject = FindSplineEnumObject();
            if (!enumObject)
                return {};
            return ReadSplineEnumEntries(enumObject);
        }

        /** Finds the exact enum object and checks its runtime UObject class and native size. */
        SDK::UObject* FindSplineEnumObject()
        {
            auto* objects = Services::TryObjects();
            auto* enumClass = SDK::UEnum::StaticClass();
            if (!objects || !objects->Ready() || !enumClass)
            {
                Services::LogMessage(Services::Level::Error,
                    "MiniMap: Spline policy initialization failed: UEnum lookup is unavailable.");
                return {};
            }

            const auto resident = objects->FindResident(
                enumClass, nullptr, L"/Script/AuActorPlacement.ECrBuildingID", true);
            auto* enumObject = resident.value ? *resident.value : nullptr;
            SDK::UClass* runtimeClass = nullptr;
            std::int32_t runtimeClassSize = 0;
            if (!enumObject ||
                !MiniMapNative::ReadMemory(reinterpret_cast<std::uintptr_t>(enumObject) +
                    offsetof(SDK::UObject, Class), &runtimeClass, sizeof(runtimeClass)) ||
                runtimeClass != enumClass ||
                !MiniMapNative::ReadMemory(reinterpret_cast<std::uintptr_t>(runtimeClass) +
                    offsetof(SDK::UStruct, Size), &runtimeClassSize, sizeof(runtimeClassSize)) ||
                runtimeClassSize != sizeof(SDK::UEnum))
            {
                Services::LogMessage(Services::Level::Error,
                    "MiniMap: Spline policy initialization failed: ECrBuildingID identity or UEnum size is incompatible.");
                return nullptr;
            }

            return enumObject;
        }

        /** Copies a bounded, readable Names array into collector-owned temporary entries. */
        std::vector<SplineEnumNameValueCopy> ReadSplineEnumEntries(SDK::UObject* enumObject)
        {
            SplineEnumArrayLayout names{};
            if (!MiniMapNative::ReadMemory(reinterpret_cast<std::uintptr_t>(enumObject) +
                offsetof(SDK::UEnum, Names), &names, sizeof(names)))
            {
                Services::LogMessage(Services::Level::Error,
                    "MiniMap: Spline policy initialization could not read ECrBuildingID Names metadata.");
                return {};
            }
            const auto count = names.Num;
            if (count <= 0 || static_cast<std::size_t>(count) > MaxSplineEnumEntries ||
                names.Max < count || names.Max < 0 || !names.Data)
            {
                LogSplinePolicy(Services::Level::Error,
                    "MiniMap: Spline policy initialization rejected ECrBuildingID Names metadata (num=%d max=%d).",
                    count, names.Max);
                return {};
            }

            std::vector<SplineEnumNameValueCopy> result(static_cast<std::size_t>(count));
            const auto byteCount = result.size() * sizeof(result.front());
            if (!MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<std::uintptr_t>(names.Data),
                    byteCount, alignof(SplineEnumNameValueCopy)) ||
                !MiniMapNative::ReadMemory(reinterpret_cast<std::uintptr_t>(names.Data),
                result.data(), byteCount))
            {
                Services::LogMessage(Services::Level::Error,
                    "MiniMap: Spline policy initialization could not read ECrBuildingID Names entries.");
                return {};
            }
            return result;
        }

        /** Builds a private table and publishes it only after enum entries validate. */
        void BuildSplinePolicyTable(
            const std::vector<SplineEnumNameValueCopy>& entries)
        {
            std::vector<SplinePolicyId> staged(MaxSupportedSplineBuildingId + 1, SplinePolicyId::Unknown);
            std::array<bool, SupportedSplineNames.size()> found{};
            std::uint32_t highestMappedId = 0;
            bool hasMappedId = false;
            for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex)
            {
                if (!StageSplineEnumEntry(entries[entryIndex], static_cast<std::int32_t>(entryIndex), staged, found,
                    highestMappedId, hasMappedId))
                    return;
            }
            ReportMissingSplineNames(found);
            if (hasMappedId)
                staged.resize(static_cast<std::size_t>(highestMappedId) + 1);
            else
                staged.clear();
            SplinePolicyByBuildingId = std::move(staged);
        }

        /** Stages one enum entry and rejects malformed or conflicting supported identities. */
        bool StageSplineEnumEntry(
            const SplineEnumNameValueCopy& entry,
            std::int32_t entryIndex,
            std::vector<SplinePolicyId>& staged,
            std::array<bool, SupportedSplineNames.size()>& found,
            std::uint32_t& highestMappedId,
            bool& hasMappedId)
        {
            if (entry.Name.ComparisonIndex < 0)
            {
                LogSplinePolicy(Services::Level::Error,
                    "MiniMap: Spline policy rejected invalid FName identity at enum index %d.", entryIndex);
                return false;
            }
            const std::string name = entry.Name.GetRawString();
            if (name.empty())
            {
                LogSplinePolicy(Services::Level::Error,
                    "MiniMap: Spline policy rejected an empty ECrBuildingID name at index %d.", entryIndex);
                return false;
            }

            const auto supportedIndex = FindSupportedSplineName(name);
            return StageSupportedSplineValue(name, supportedIndex, entry.Value, staged, found,
                highestMappedId, hasMappedId);
        }

        /** Applies a supported name/value pair after rejecting invalid bounds and ambiguity. */
        bool StageSupportedSplineValue(
            const std::string& name,
            std::size_t supportedIndex,
            std::int64_t value,
            std::vector<SplinePolicyId>& staged,
            std::array<bool, SupportedSplineNames.size()>& found,
            std::uint32_t& highestMappedId,
            bool& hasMappedId)
        {
            if (value < 0 || static_cast<std::uint64_t>(value) > MaxSupportedSplineBuildingId)
            {
                LogSplinePolicy(Services::Level::Warn,
                    "MiniMap: Spline policy ignored ECrBuildingID name=%s value=%lld outside supported bounds.",
                    name.c_str(), static_cast<long long>(value));
                return true;
            }
            if (supportedIndex == SupportedSplineNames.size())
                return true;
            if (found[supportedIndex])
            {
                LogSplinePolicy(Services::Level::Error,
                    "MiniMap: Spline policy rejected duplicate enumerator name=%s.", name.c_str());
                return false;
            }

            found[supportedIndex] = true;
            const auto buildingId = static_cast<std::uint32_t>(value);
            const auto policy = SupportedSplineNames[supportedIndex].second;
            if (staged[buildingId] != SplinePolicyId::Unknown && staged[buildingId] != policy)
            {
                LogSplinePolicy(Services::Level::Error,
                    "MiniMap: Spline policy rejected conflicting policies for BuildingId=%u.", buildingId);
                return false;
            }
            staged[buildingId] = policy;
            highestMappedId = (std::max)(highestMappedId, buildingId);
            hasMappedId = true;
            return true;
        }

        /** Finds one exact supported symbolic identity without case folding or substring matching. */
        static std::size_t FindSupportedSplineName(std::string_view name) noexcept
        {
            for (std::size_t index = 0; index < SupportedSplineNames.size(); ++index)
                if (name == SupportedSplineNames[index].first)
                    return index;
            return SupportedSplineNames.size();
        }

        /** Reports every expected supported name that was absent from the valid enum. */
        static void ReportMissingSplineNames(const std::array<bool, SupportedSplineNames.size()>& found)
        {
            for (std::size_t index = 0; index < SupportedSplineNames.size(); ++index)
            {
                if (!found[index])
                {
                    LogSplinePolicy(Services::Level::Warn,
                        "MiniMap: Spline policy name missing from ECrBuildingID: %.*s.",
                        static_cast<int>(SupportedSplineNames[index].first.size()),
                        SupportedSplineNames[index].first.data());
                }
            }
        }

        /** Returns Unknown when initialization failed or a runtime ID is not represented. */
        SplinePolicyId PolicyForBuildingId(std::uint32_t buildingId) const noexcept
        {
            if (!SplinePolicyInitializationAttempted || buildingId >= SplinePolicyByBuildingId.size())
                return SplinePolicyId::Unknown;
            return SplinePolicyByBuildingId[buildingId];
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

        /** Retains acquired facts, their definition owner, and completed contribution for one building identity. */
        struct Entry
        {
            Facts::Building Data;
            std::shared_ptr<DefinitionOwner> Definition;
            std::uint64_t CompletedContribution = 0;
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
            Internal::Bounds2D RegionFastAccept;
            Internal::Bounds2D RegionOverlappingAccept;
        };

        std::unordered_map<std::uint64_t, Entry> Entities;
        std::unordered_set<std::uint64_t> Pending;
        std::map<std::pair<std::int64_t, std::int64_t>,
            std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>> RetainedSectionPublications;
        std::unordered_map<SDK::UAuActorPlacementData*, std::shared_ptr<DefinitionOwner>> Definitions;
        std::shared_ptr<Metadata> Types;
        RuntimeFieldContract RuntimeFields{};
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
        bool FullFootprintRebuildPending = false;
        const double SectionSizeMapUnits;

        /** Packs the full session-private Index/Serial pair; never a content fingerprint. */
        static std::uint64_t Key(EntityId id) noexcept
        {
            return std::uint64_t(std::uint32_t(id.Index)) |
                (std::uint64_t(std::uint32_t(id.SerialNumber)) << 32);
        }

        /** Captures logical source lifetime before native operations. */
        Context CaptureContext() const noexcept
        {
            Context context{World, Manager, Generation, RequestGeneration, HasRegion, RequestedRegion};
            if (context.HasRegion)
            {
                context.RegionFastAccept = {
                    context.Region.X,
                    context.Region.Y,
                    context.Region.X + context.Region.Width,
                    context.Region.Y + context.Region.Height};
                context.RegionOverlappingAccept = {
                    context.RegionFastAccept.MinX - 5000.0,
                    context.RegionFastAccept.MinY - 5000.0,
                    context.RegionFastAccept.MaxX + 5000.0,
                    context.RegionFastAccept.MaxY + 5000.0};
            }
            return context;
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
            const bool snapshotCurrent = CommittedCoverageValid &&
                CoverageWorld == World && CoverageWorld == world && CoverageManager == Manager &&
                CoverageSession == Generation && CoverageRequest != 0 &&
                CoverageContentHash == AcquisitionContentHash;
            const bool contained = snapshotCurrent && IsRegionContained(RequestedRegion, CommittedCoverage);
            if (PendingReuseEligible && contained)
            {
                MembershipComplete = true;
                ImmediateScanRequired = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++Diagnostics.SuccessfulCoverageReuse;
#endif
            }
            else if (!snapshotCurrent)
            {
                ImmediateScanRequired = true;
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
            }
            RegionChangePendingValidation = false;
            PendingSnapshotEligible = false;
            PendingReuseEligible = false;
        }

        /** Tests the exact shared reach predicate against one acquired rectangle. */
        static bool IsBuildingAdmittedToRegion(
            const Facts::Building& building,
            const Facts::Region& region) noexcept
        {
            if (!BuildingCollectorV2::HasValidMapPosition(building) ||
                !std::isfinite(building.ResolvedReach) || building.ResolvedReach < 0.0)
            {
                return true;
            }
            const Internal::Bounds2D fastAccept{
                region.X,
                region.Y,
                region.X + region.Width,
                region.Y + region.Height};
            const Internal::Bounds2D overlappingAccept{
                fastAccept.MinX - 5000.0,
                fastAccept.MinY - 5000.0,
                fastAccept.MaxX + 5000.0,
                fastAccept.MaxY + 5000.0};
            if (!std::isfinite(fastAccept.MaxX) || !std::isfinite(fastAccept.MaxY) ||
                !std::isfinite(overlappingAccept.MinX) || !std::isfinite(overlappingAccept.MinY) ||
                !std::isfinite(overlappingAccept.MaxX) || !std::isfinite(overlappingAccept.MaxY))
            {
                return true;
            }
            return Internal::BuildingSpatialAdmission::IsBuildingAccepted(
                building.Position[0], building.Position[1], fastAccept, overlappingAccept,
                building.ResolvedReach);
        }

        /** Retains only publication owners whose Sections remain inside both committed rectangles. */
        std::size_t RetainOverlappingSectionPublications(
            const Facts::Region& previousRegion,
            const Facts::Region& newRegion) noexcept
        {
            const double previousRight = previousRegion.X + previousRegion.Width;
            const double previousBottom = previousRegion.Y + previousRegion.Height;
            const double newRight = newRegion.X + newRegion.Width;
            const double newBottom = newRegion.Y + newRegion.Height;
            std::size_t preservedCount = 0;
            for (auto iterator = RetainedSectionPublications.begin();
                iterator != RetainedSectionPublications.end();)
            {
                const auto& region = iterator->second->SectionRegion();
                const double sectionRight = region.xMapUnits + region.widthMapUnits;
                const double sectionBottom = region.yMapUnits + region.heightMapUnits;
                const bool insidePrevious = region.xMapUnits >= previousRegion.X &&
                    region.yMapUnits >= previousRegion.Y && sectionRight <= previousRight &&
                    sectionBottom <= previousBottom;
                const bool insideNew = region.xMapUnits >= newRegion.X && region.yMapUnits >= newRegion.Y &&
                    sectionRight <= newRight && sectionBottom <= newBottom;
                if (!insidePrevious || !insideNew)
                {
                    iterator = RetainedSectionPublications.erase(iterator);
                }
                else
                {
                    ++preservedCount;
                    ++iterator;
                }
            }
            return preservedCount;
        }

        /** Reports whether the last successful source snapshot still matches this collector. */
        bool HasComparableCommittedCoverage() const noexcept
        {
            return CommittedCoverageValid && CoverageWorld == World && CoverageManager == Manager &&
                CoverageSession == Generation && CoverageRequest != 0 &&
                CoverageContentHash == AcquisitionContentHash;
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
            RetainedSectionPublications.clear();
            FullFootprintRebuildPending = false;
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
            const Context& context,
            double px,
            double py,
            double resolvedReach) noexcept
        {
            return Internal::BuildingSpatialAdmission::IsBuildingAccepted(
                px, py, context.RegionFastAccept, context.RegionOverlappingAccept, resolvedReach);
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
            if (!context.HasRegion || !IsRegionallyAdmitted(context, x, y, resolvedReach))
                return std::nullopt;
            SDK::UAuActorPlacementData* placement = GetPlacementData(parameters);
            if (!IncludeDroneRails && placement != nullptr &&
                PolicyForBuildingId(static_cast<std::uint32_t>(placement->BuildingID)) == SplinePolicyId::DroneRail)
            {
                return std::nullopt;
            }
            if (!views.HasSplineConnections)
            {
                const auto retained = Entities.find(Key(id));
                OwnedScanOccurrence reusable;
                if (retained != Entities.end() && TryReuseCompleteNonSplineOccurrence(
                    views, index, id, resolvedReach, retained->second, reusable))
                {
                    return std::move(reusable);
                }
            }
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
                Facts::Spline spline = BuildingSplineSource::CopyFragment(
                    views.SplineConnections[index], RuntimeFields);
                occurrence.Data.SplineStatus = spline.SourceStatus;
                occurrence.Data.SplineFacts = std::make_shared<const Facts::Spline>(std::move(spline));
                if (BuildingSplineSource::NeedsFallback(*occurrence.Data.SplineFacts) && views.HasActorPlacement)
                {
                    occurrence.Payload = std::make_unique<PayloadHandleOwner>(
                        native, *MapRuntimeAccess::Field<SDK::FAuAPPayloadDataHandle>(
                            &views.ActorPlacement[index], RuntimeFields.ActorPlacementPayload));
                }
            }
            else
            {
                occurrence.Data.SplineStatus = Status::Ready;
            }
            if (placement)
            {
                occurrence.Placement = DefinitionAcquisition.Retain(
                    placement);
            }
            return std::move(occurrence);
        }

        /** Reuses complete immutable facts only when the current nonspline source still matches. */
        bool TryReuseCompleteNonSplineOccurrence(
            const MassChunkViews& views,
            int32 index,
            EntityId id,
            double resolvedReach,
            const Entry& retained,
            OwnedScanOccurrence& output)
        {
            auto* placement = GetPlacementData(views.BuildingParameters);
            if (!retained.Data.Complete || retained.Data.HasSpline || placement == nullptr ||
                retained.Data.DefinitionFacts == nullptr)
            {
                return false;
            }
            const auto definition = Definitions.find(placement);
            if (definition == Definitions.end() || definition->second->Facts == nullptr ||
                !BuildingDefinitionAcquisition::IsComplete(*definition->second->Facts) ||
                !(*definition->second->Facts == *retained.Data.DefinitionFacts))
            {
                return false;
            }
            Facts::Building current = retained.Data;
            CopyTransform(views.Transforms[index].Transform, current);
            current.Index = id.Index;
            current.Serial = id.SerialNumber;
            current.EntityStatus = Status::Ready;
            current.ResolvedReach = resolvedReach;
            current.SplineClassificationStatus = Status::Ready;
            current.SplineStatus = Status::Ready;
            current.HasSpline = false;
            if (!SameAcquiredFacts(retained.Data, current))
            {
                return false;
            }
            output.Data = retained.Data;
            output.Placement = definition->second;
            output.ReusedRetainedFacts = true;
            return true;
        }

        /** Reads the shared placement pointer through its admitted runtime member offset. */
        SDK::UAuActorPlacementData* GetPlacementData(
            const SDK::FCrBuildingParameters* parameters) const noexcept
        {
            if (!parameters)
                return nullptr;
            const auto* placement = MapRuntimeAccess::Field<SDK::UAuActorPlacementData*>(
                parameters, RuntimeFields.BuildingParametersPlacementData);
            return *placement;
        }

        /** Copies admitted chunk occurrences into owned storage and rejects duplicate identities. */
        void CaptureMassChunk(
            const Native& native,
            const Context& context,
            const MassChunkViews& views,
            std::vector<OwnedScanOccurrence>& occurrences,
            std::unordered_set<std::uint64_t>& seen
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++timing->Admitted;
                if (occurrences.back().ReusedRetainedFacts)
                    ++timing->FastPathEligible;
                else
                    ++timing->FullAcquisitionRequired;
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
                BuildingSplineSource::CopyFallback(
                    *occurrence.Payload->Get(), RuntimeFields, spline);
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

        /** Compares acquired values at source precision to preserve equivalent immutable owners. */
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
                first.Complete == second.Complete &&
                ::Services::Map::Buildings::SourceContentPrecision::Equal(first.Position, second.Position) &&
                ::Services::Map::Buildings::SourceContentPrecision::Equal(first.ResolvedReach, second.ResolvedReach) &&
                (first.TransformStatus == Facts::Status::Ready
                    ? ::Services::Map::Buildings::SourceContentPrecision::EqualQuaternion(first.Rotation, second.Rotation)
                    : ::Services::Map::Buildings::SourceContentPrecision::Equal(first.Rotation, second.Rotation)) &&
                ::Services::Map::Buildings::SourceContentPrecision::Equal(first.SignedScale, second.SignedScale) &&
                definitionsMatch && splinesMatch;
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Compares copied profile values without treating shared-owner identity as content. */
        static bool SameProfiles(
            const std::shared_ptr<const ::Services::Map::Buildings::SplineProfileSet>& first,
            const std::shared_ptr<const ::Services::Map::Buildings::SplineProfileSet>& second)
        {
            return first == second || (first && second && *first == *second);
        }

        /** Compares optional copied spline payload data by values. */
        static bool SameOptionalSplineData(const std::optional<Facts::SplineData>& first,
            const std::optional<Facts::SplineData>& second)
        {
            return first == second;
        }

        /** Classifies changed transform, status, and definition source fields. */
        static std::uint32_t CompareDefinitionAndPlacementFacts(
            const Facts::Building& previous, const Facts::Building& current, bool& transformChanged)
        {
            using Change = AcquiredFactChange;
            std::uint32_t changes = 0;
            if (previous.Index != current.Index || previous.Serial != current.Serial)
                changes |= static_cast<std::uint32_t>(Change::Identity);
            transformChanged = !::Services::Map::Buildings::SourceContentPrecision::Equal(
                    previous.Position, current.Position) ||
                (previous.TransformStatus == Facts::Status::Ready && current.TransformStatus == Facts::Status::Ready
                    ? !::Services::Map::Buildings::SourceContentPrecision::EqualQuaternion(
                        previous.Rotation, current.Rotation)
                    : !::Services::Map::Buildings::SourceContentPrecision::Equal(
                        previous.Rotation, current.Rotation)) ||
                !::Services::Map::Buildings::SourceContentPrecision::Equal(
                    previous.SignedScale, current.SignedScale);
            if (previous.TransformStatus != current.TransformStatus ||
                !::Services::Map::Buildings::SourceContentPrecision::Equal(previous.ResolvedReach, current.ResolvedReach) ||
                transformChanged)
                changes |= static_cast<std::uint32_t>(Change::Spatial);
            if (previous.EntityStatus != current.EntityStatus)
                changes |= static_cast<std::uint32_t>(Change::EntityStatus);
            if (previous.DefinitionStatus != current.DefinitionStatus || previous.Complete != current.Complete ||
                static_cast<bool>(previous.DefinitionFacts) != static_cast<bool>(current.DefinitionFacts))
                changes |= static_cast<std::uint32_t>(Change::DefinitionAvailability);
            AddDefinitionValueChanges(previous.DefinitionFacts, current.DefinitionFacts, changes);
            return changes;
        }

        /** Classifies copied definition names, identifiers, statuses, and interior facts. */
        static void AddDefinitionValueChanges(const std::shared_ptr<const Facts::Definition>& previous,
            const std::shared_ptr<const Facts::Definition>& current, std::uint32_t& changes)
        {
            using Change = AcquiredFactChange;
            if (!previous || !current)
                return;
            if (previous->ObjectIndex != current->ObjectIndex || previous->BuildingId != current->BuildingId ||
                previous->CanonicalClassPath != current->CanonicalClassPath ||
                previous->EntityConfigPath != current->EntityConfigPath || previous->ClassStatus != current->ClassStatus ||
                previous->ConfigReferenceStatus != current->ConfigReferenceStatus)
                changes |= static_cast<std::uint32_t>(Change::DefinitionValues);
            if (previous->InteriorStatus != current->InteriorStatus || previous->InteriorOnly != current->InteriorOnly)
                changes |= static_cast<std::uint32_t>(Change::Interior);
            if (previous->SplineProfileStatus != current->SplineProfileStatus ||
                !SameProfiles(previous->SplineProfiles, current->SplineProfiles))
                changes |= static_cast<std::uint32_t>(Change::SplineProfiles);
        }

        /** Classifies copied spline fragment, authored channels, and deferred payload facts. */
        static std::uint32_t CompareSplineFacts(const Facts::Building& previous, const Facts::Building& current,
            bool& channelsChanged)
        {
            using Change = AcquiredFactChange;
            std::uint32_t changes = 0;
            if (previous.SplineClassificationStatus != current.SplineClassificationStatus ||
                previous.HasSpline != current.HasSpline ||
                static_cast<bool>(previous.SplineFacts) != static_cast<bool>(current.SplineFacts))
                changes |= static_cast<std::uint32_t>(Change::SplineClassification);
            if (previous.SplineStatus != current.SplineStatus)
                changes |= static_cast<std::uint32_t>(Change::SplineFragment);
            if (!previous.SplineFacts || !current.SplineFacts)
                return changes;
            AddSplineChannelChanges(*previous.SplineFacts, *current.SplineFacts, changes, channelsChanged);
            if (previous.SplineFacts->FragmentData != current.SplineFacts->FragmentData)
                changes |= static_cast<std::uint32_t>(Change::SplineFragment);
            if (!SameOptionalSplineData(previous.SplineFacts->PayloadData, current.SplineFacts->PayloadData) ||
                previous.SplineFacts->Provenance != current.SplineFacts->Provenance ||
                previous.SplineFacts->SourceStatus != current.SplineFacts->SourceStatus)
                changes |= static_cast<std::uint32_t>(Change::SplinePayload);
            return changes;
        }

        /** Classifies each authored curve channel without inspecting object layout or padding. */
        static void AddSplineChannelChanges(const Facts::Spline& previous, const Facts::Spline& current,
            std::uint32_t& changes, bool& channelsChanged)
        {
            using Change = AcquiredFactChange;
            if (previous.CurveVersion != current.CurveVersion)
                changes |= static_cast<std::uint32_t>(Change::SplineCurveVersion);
            if (!(previous.Position == current.Position))
            {
                changes |= static_cast<std::uint32_t>(Change::SplinePosition);
                channelsChanged = true;
            }
            if (!(previous.Rotation == current.Rotation))
            {
                changes |= static_cast<std::uint32_t>(Change::SplineRotation);
                channelsChanged = true;
            }
            if (!(previous.Scale == current.Scale))
            {
                changes |= static_cast<std::uint32_t>(Change::SplineScale);
                channelsChanged = true;
            }
            if (!(previous.Reparameterization == current.Reparameterization))
            {
                changes |= static_cast<std::uint32_t>(Change::SplineReparameterization);
                channelsChanged = true;
            }
            if (previous.CurvesInitialized != current.CurvesInitialized)
                changes |= static_cast<std::uint32_t>(Change::SplineInitialization);
        }

        /** Builds a compact field mask after the existing deep equality check finds a change. */
        static std::uint32_t ClassifyAcquiredFactChanges(
            const Facts::Building& previous, const Facts::Building& current,
            bool& channelsChanged, bool& transformChanged)
        {
            const std::uint32_t changes = CompareDefinitionAndPlacementFacts(previous, current, transformChanged) |
                CompareSplineFacts(previous, current, channelsChanged);
            return changes != 0 ? changes : static_cast<std::uint32_t>(AcquiredFactChange::Other);
        }

        /** Names the scan reason included in a changed-facts summary. */
        static const char* AcquisitionReasonName(AcquisitionReason reason) noexcept
        {
            switch (reason)
            {
            case AcquisitionReason::Immediate: return "immediate";
            case AcquisitionReason::Mutation: return "mutation";
            }
            return "unknown";
        }

        /** Emits one bounded summary only when a retained occurrence changed. */
        void ReportAcquiredFactChanges(AcquisitionReason reason, const AcquiredFactChangeSummary& summary) const
        {
            if (summary.Changed == 0)
                return;
            if (auto* log = MiniMap::Services::TryLog())
            {
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                    "MiniMap: BuildingMapAcquiredFactsTrace reason=%s retained=%llu changed=%llu identity=%llu "
                    "spatial=%llu entityStatus=%llu definitionAvailability=%llu definitionValues=%llu interior=%llu "
                    "splineClassification=%llu splineFragment=%llu curveVersion=%llu splinePosition=%llu "
                    "splineRotation=%llu splineScale=%llu splineReparameterization=%llu splineInitialization=%llu "
                    "splineProfiles=%llu splinePayload=%llu other=%llu completeTransitions=%llu transformChanges=%llu "
                    "channelsUninitialized=%llu channelsBothUninitialized=%llu channelsOldUninitialized=%llu "
                    "channelsBothInitialized=%llu "
                    "channelsOldInitialized=%llu samples={%d:%d/0x%x,%d:%d/0x%x,%d:%d/0x%x,%d:%d/0x%x}",
                    AcquisitionReasonName(reason), static_cast<unsigned long long>(summary.RetainedExamined),
                    static_cast<unsigned long long>(summary.Changed), static_cast<unsigned long long>(summary.Identity),
                    static_cast<unsigned long long>(summary.Spatial), static_cast<unsigned long long>(summary.EntityStatus),
                    static_cast<unsigned long long>(summary.DefinitionAvailability),
                    static_cast<unsigned long long>(summary.DefinitionValues), static_cast<unsigned long long>(summary.Interior),
                    static_cast<unsigned long long>(summary.SplineClassification),
                    static_cast<unsigned long long>(summary.SplineFragment),
                    static_cast<unsigned long long>(summary.SplineCurveVersion),
                    static_cast<unsigned long long>(summary.SplinePosition), static_cast<unsigned long long>(summary.SplineRotation),
                    static_cast<unsigned long long>(summary.SplineScale),
                    static_cast<unsigned long long>(summary.SplineReparameterization),
                    static_cast<unsigned long long>(summary.SplineInitialization),
                    static_cast<unsigned long long>(summary.SplineProfiles), static_cast<unsigned long long>(summary.SplinePayload),
                    static_cast<unsigned long long>(summary.Other),
                    static_cast<unsigned long long>(summary.CompleteTransitions),
                    static_cast<unsigned long long>(summary.TransformChanges),
                    static_cast<unsigned long long>(summary.BothUninitializedChannels +
                        summary.OldUninitializedChannels + summary.OldInitializedChannels),
                    static_cast<unsigned long long>(summary.BothUninitializedChannels),
                    static_cast<unsigned long long>(summary.OldUninitializedChannels),
                    static_cast<unsigned long long>(summary.BothInitializedChannels),
                    static_cast<unsigned long long>(summary.OldInitializedChannels),
                    summary.Samples[0].Index, summary.Samples[0].Serial, summary.Samples[0].Mask,
                    summary.Samples[1].Index, summary.Samples[1].Serial, summary.Samples[1].Mask,
                    summary.Samples[2].Index, summary.Samples[2].Serial, summary.Samples[2].Mask,
                    summary.Samples[3].Index, summary.Samples[3].Serial, summary.Samples[3].Mask);
            }
        }

#if defined(_DEBUG)
        /** Reports exact double-precision values for a retained occurrence whose transform changed. */
        static void ReportTransformChange(AcquisitionReason reason,
            const Facts::Building& previous, const Facts::Building& current)
        {
            if (auto* log = MiniMap::Services::TryLog())
            {
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                    "MiniMap: BuildingMapTransformTrace index=%d serial=%d reason=%s "
                    "previous={transformStatus:%u resolvedReach:%.17g position:{%.17g,%.17g,%.17g} "
                    "normalizedQuaternion:{%.17g,%.17g,%.17g,%.17g} signedScale:{%.17g,%.17g,%.17g}} "
                    "current={transformStatus:%u resolvedReach:%.17g position:{%.17g,%.17g,%.17g} "
                    "normalizedQuaternion:{%.17g,%.17g,%.17g,%.17g} signedScale:{%.17g,%.17g,%.17g}}",
                    current.Index, current.Serial, AcquisitionReasonName(reason),
                    static_cast<unsigned>(previous.TransformStatus), previous.ResolvedReach,
                    previous.Position[0], previous.Position[1], previous.Position[2],
                    previous.Rotation[0], previous.Rotation[1], previous.Rotation[2], previous.Rotation[3],
                    previous.SignedScale[0], previous.SignedScale[1], previous.SignedScale[2],
                    static_cast<unsigned>(current.TransformStatus), current.ResolvedReach,
                    current.Position[0], current.Position[1], current.Position[2],
                    current.Rotation[0], current.Rotation[1], current.Rotation[2], current.Rotation[3],
                    current.SignedScale[0], current.SignedScale[1], current.SignedScale[2]);
            }
        }
#endif
#endif

        /** Captures one serial traversal into callback-owned snapshots. */
        bool CaptureMassSnapshot(
            const Native& native,
            const Context& context,
            std::vector<OwnedScanOccurrence>& occurrences
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
                [this, &native, &context, &occurrences, &seen
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , timing
#endif
                ](const MassChunkViews& views)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing->Chunks;
#endif
                    CaptureMassChunk(native, context, views, occurrences, seen
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
            std::uint64_t completedContribution,
            std::unordered_map<std::uint64_t, Entry>& reconciled,
            std::unordered_set<std::uint64_t>& pending,
            const Facts::Region& previousCoverage,
            bool canCompareCoverageMovement,
            bool& publicationFactsUnchanged
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , AcquiredFactChangeSummary* changes
#endif
        )
        {
            const auto key = Key({data.Index, data.Serial});
            auto previous = Entities.find(key);
            Entry entry;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            if (previous != Entities.end())
            {
                ++changes->RetainedExamined;
            }
#endif
            if (previous != Entities.end())
            {
                if (SameAcquiredFacts(previous->second.Data, data))
                {
                    entry.Data = previous->second.Data;
                    entry.Definition = previous->second.Definition;
                    entry.CompletedContribution = previous->second.CompletedContribution;
                }
                else
                {
                    publicationFactsUnchanged = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    bool splineChannelsChanged = false;
                    bool transformChanged = false;
                    const std::uint32_t mask = ClassifyAcquiredFactChanges(
                        previous->second.Data, data, splineChannelsChanged, transformChanged);
                    changes->Add(previous->second.Data, data, mask, splineChannelsChanged, transformChanged);
#if defined(_DEBUG)
                    if (transformChanged)
                    {
                        ReportTransformChange(changes->Reason, previous->second.Data, data);
                    }
#endif
#endif
                    entry.Data = std::move(data);
                    entry.CompletedContribution = completedContribution;
                }
            }
            else
            {
                if (!canCompareCoverageMovement ||
                    IsBuildingAdmittedToRegion(data, previousCoverage))
                {
                    publicationFactsUnchanged = false;
                }
                entry.Data = std::move(data);
                entry.CompletedContribution = completedContribution;
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
            const Facts::Region& previousCoverage,
            bool canCompareCoverageMovement,
            bool& publicationFactsUnchanged,
            AcquisitionHashAggregate& acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
            , AcquiredFactChangeSummary* changes
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
                if (occurrence.ReusedRetainedFacts)
                {
                    data = std::move(occurrence.Data);
                }
                else if (!AcquireOwnedOccurrence(native, context, occurrence,
                    attemptedCoreDefinitions, attemptedSplineProfiles, data))
                {
                    return false;
                }
                const std::uint64_t completedContribution =
                    CompleteAcquisitionContribution(occurrence.PreliminaryHashContribution, data);
                acquisitionHash.Add(completedContribution);
                ReconcileAcquiredOccurrence(data, occurrence, completedContribution, reconciled, pending,
                    previousCoverage, canCompareCoverageMovement, publicationFactsUnchanged
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , changes
#endif
                );
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing->DeferredAcquisitionMs = std::chrono::duration<double, std::milli>(Clock::now() - acquisitionStarted).count();
#endif
            return IsContextCurrent(native, context);
        }

        /** Separates boundary removals from missing identities that could affect an overlapping Section.
         * A Section contained in the new rectangle is a subset of that rectangle, so its shortest
         * XY distance from a building origin cannot be less than the distance to the whole rectangle.
         * The shared admission predicate therefore proves that a rejected old origin cannot contribute
         * to any retained overlap Section. The same proof is used for newly acquired origins against
         * the previous rectangle in ReconcileAcquiredOccurrence().
         */
        bool AreRemovedOccurrencesOutsideNewCoverage(
            const std::unordered_map<std::uint64_t, Entry>& reconciled,
            const Facts::Region& newCoverage,
            bool canCompareCoverageMovement
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , MembershipTiming* timing
#endif
        ) const
        {
            bool coverageOnly = true;
            for (const auto& previous : Entities)
            {
                if (!reconciled.contains(previous.first))
                {
                    if (!canCompareCoverageMovement ||
                        IsBuildingAdmittedToRegion(previous.second.Data, newCoverage))
                    {
                        coverageOnly = false;
                    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing->Removed;
#endif
                }
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            for (const auto& item : reconciled)
            {
                if (!Entities.contains(item.first))
                {
                    ++timing->Added;
                }
            }
#endif
            return coverageOnly;
        }

        /** Commits the fully acquired regional set only while its session and request remain current. */
        bool CommitRegionalState(
            const Native& native,
            const Context& context,
            std::unordered_map<std::uint64_t, Entry>& reconciled,
            std::unordered_set<std::uint64_t>& pending,
            bool publicationFactsUnchanged,
            bool canCompareCoverageMovement,
            const Facts::Region& previousCoverage,
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
            const bool removedOnlyOutsideNewCoverage = AreRemovedOccurrencesOutsideNewCoverage(
                reconciled, context.Region, canCompareCoverageMovement
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
#endif
            );
            if (!canCompareCoverageMovement || !publicationFactsUnchanged ||
                !removedOnlyOutsideNewCoverage)
            {
                RetainedSectionPublications.clear();
                FullFootprintRebuildPending = true;
            }
            else if (!(previousCoverage == context.Region))
            {
                const std::size_t preservedCount = RetainOverlappingSectionPublications(
                    previousCoverage, context.Region);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                Diagnostics.SectionOwnersPreservedOnMovement += preservedCount;
#endif
            }
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
            , AcquiredFactChangeSummary* changes
#endif
        )
        {
            if (!context.HasRegion || !IsContextCurrent(native, context))
                return false;
            const Facts::Region previousCoverage = CommittedCoverage;
            const bool canCompareCoverageMovement = HasComparableCommittedCoverage();
            std::vector<OwnedScanOccurrence> occurrences;
            AcquisitionHashAggregate acquisitionHash{};
            bool publicationFactsUnchanged = true;
            if (!CaptureMassSnapshot(native, context, occurrences
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
#endif
            ))
                return false;
            std::unordered_map<std::uint64_t, Entry> reconciled;
            std::unordered_set<std::uint64_t> pending;
            if (!BuildRegionalState(native, context, occurrences, reconciled, pending,
                previousCoverage, canCompareCoverageMovement, publicationFactsUnchanged,
                acquisitionHash
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
                , changes
#endif
            ))
                return false;
            return CommitRegionalState(native, context, reconciled, pending,
                publicationFactsUnchanged, canCompareCoverageMovement, previousCoverage,
                FinalizeAcquisitionHash(acquisitionHash)
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , timing
#endif
            );
        }

        /** Chooses required coverage work or an observed mutation after its quiet period. */
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
            AcquiredFactChangeSummary changes;
            changes.Reason = reason;
#endif
            try
            {
                complete = Reconcile(native, context
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , &timing, &changes
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
                RetryNotBefore = {};
                ImmediateScanRequired = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ReportAcquiredFactChanges(reason, changes);
#endif
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
        bool IsDroneRailDefinitionId(std::uint32_t buildingId) const noexcept
        {
            return PolicyForBuildingId(buildingId) == SplinePolicyId::DroneRail;
        }

        /** Requires applicable definition profiles before an occurrence leaves pending acquisition. */
        bool AreRequiredSplineProfilesResolved(
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
        bool IsDroneRail(const Facts::Building& building) const noexcept
        {
            if (!building.DefinitionFacts)
                return false;
            return IsDroneRailDefinitionId(building.DefinitionFacts->BuildingId);
        }

        /** Classifies only spline occurrences with a ready acquired definition ID. */
        SplinePolicyId ClassifySplinePolicyId(const Facts::Building& building) const noexcept
        {
            if (building.SplineClassificationStatus != Status::Ready || !building.HasSpline ||
                !building.DefinitionFacts ||
                building.DefinitionFacts->ClassStatus != Status::Ready)
            {
                return SplinePolicyId::Unknown;
            }

            return PolicyForBuildingId(building.DefinitionFacts->BuildingId);
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
        ::Services::Map::Buildings::BuildingCandidateSourceRecord BuildCandidateSourceRecord(
            const Facts::Building& building) const
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
                    source.syntheticThinRibbonHalfWidthMapUnits = 7.5;
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
        using PublicationKey = std::pair<std::int64_t, std::int64_t>;

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

            /** Copies one source value when a contributor is distributed to an earlier Section. */
            SectionContributor(
                std::uint64_t identity,
                const CandidateOccurrence& occurrence,
                const CandidateSourceRecord& source)
                : Identity(identity), Occurrence(occurrence), Source(source)
            {
            }

            /** Moves the owned source value into the final matching Section. */
            SectionContributor(
                std::uint64_t identity,
                const CandidateOccurrence& occurrence,
                CandidateSourceRecord&& source)
                : Identity(identity), Occurrence(occurrence), Source(std::move(source))
            {
            }
        };

        /** Holds the two rectangles prepared once for one requested Section. */
        struct SectionAdmissionBounds
        {
            Internal::Bounds2D FastAccept;
            Internal::Bounds2D OverlappingAccept;
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
            lookup.SectionSize = SectionSizeMapUnits;
            if (sectionRegions.front().widthMapUnits != lookup.SectionSize ||
                sectionRegions.front().heightMapUnits != lookup.SectionSize)
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

        /** Applies the shared exact circular reach test using precomputed Section bounds. */
        static bool IsContributorForSection(
            const SectionAdmissionBounds& bounds,
            double positionX,
            double positionY,
            double resolvedReach) noexcept
        {
            return Internal::BuildingSpatialAdmission::IsBuildingAccepted(
                positionX, positionY, bounds.FastAccept, bounds.OverlappingAccept, resolvedReach);
        }

        /** Precomputes each requested Section rectangle and its shared 5,000-unit reach bound. */
        static std::vector<SectionAdmissionBounds> BuildSectionAdmissionBounds(
            std::span<const CollectionRegion> sectionRegions)
        {
            std::vector<SectionAdmissionBounds> sectionBounds;
            sectionBounds.reserve(sectionRegions.size());
            for (const CollectionRegion& region : sectionRegions)
            {
                SectionAdmissionBounds bounds;
                bounds.FastAccept = {
                    region.xMapUnits,
                    region.yMapUnits,
                    region.xMapUnits + region.widthMapUnits,
                    region.yMapUnits + region.heightMapUnits};
                bounds.OverlappingAccept = {
                    bounds.FastAccept.MinX - 5000.0,
                    bounds.FastAccept.MinY - 5000.0,
                    bounds.FastAccept.MaxX + 5000.0,
                    bounds.FastAccept.MaxY + 5000.0};
                sectionBounds.push_back(bounds);
            }
            return sectionBounds;
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
            std::span<const SectionAdmissionBounds> sectionBounds,
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
                        if (IsContributorForSection(sectionBounds[sectionIndex],
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

        /** Distributes one occurrence by copying its source to earlier matches and moving it to the last. */
        static void AppendContributorToSections(
            const Facts::Building& building,
            const CandidateOccurrence& occurrence,
            CandidateSourceRecord& source,
            std::uint64_t contentHash,
            const std::vector<std::size_t>& matchingSections,
            std::vector<std::vector<SectionContributor>>& contributorsBySection,
            std::vector<AcquisitionHashAggregate>& hashesBySection
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , SectionPublicationBuildMetrics& metrics
#endif
        )
        {
            const std::uint64_t identity = Key({building.Index, building.Serial});
            for (std::size_t matchingIndex = 0; matchingIndex < matchingSections.size(); ++matchingIndex)
            {
                const std::size_t sectionIndex = matchingSections[matchingIndex];
                if (matchingIndex + 1 == matchingSections.size())
                {
                    contributorsBySection[sectionIndex].emplace_back(identity, occurrence, std::move(source));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++metrics.SourceMoves;
#endif
                }
                else
                {
                    contributorsBySection[sectionIndex].emplace_back(identity, occurrence, source);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++metrics.SourceCopies;
#endif
                }
                hashesBySection[sectionIndex].Add(contentHash);
            }
        }

        /** Freezes each Section after identity sorting and contiguous local ordinal assignment. */
        static std::vector<std::shared_ptr<const CandidatePublication>> FreezeSectionPublications(
            std::span<const CollectionRegion> sectionRegions,
            std::vector<std::vector<SectionContributor>>& contributorsBySection,
            const std::vector<AcquisitionHashAggregate>& hashesBySection
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , SectionPublicationBuildMetrics& metrics
#endif
        )
        {
            std::vector<std::shared_ptr<const CandidatePublication>> publications;
            publications.reserve(sectionRegions.size());
            for (std::size_t sectionIndex = 0; sectionIndex < sectionRegions.size(); ++sectionIndex)
            {
                auto& contributors = contributorsBySection[sectionIndex];
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                metrics.FinalOccurrences += contributors.size();
                const auto indexSortStarted = Clock::now();
#endif
                std::vector<std::size_t> contributorOrder;
                contributorOrder.reserve(contributors.size());
                for (std::size_t contributorIndex = 0; contributorIndex < contributors.size(); ++contributorIndex)
                {
                    contributorOrder.push_back(contributorIndex);
                }
                std::sort(contributorOrder.begin(), contributorOrder.end(), [&contributors](
                    std::size_t leftIndex, std::size_t rightIndex)
                {
                    return contributors[leftIndex].Identity < contributors[rightIndex].Identity;
                });
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                metrics.IndexSortMilliseconds +=
                    std::chrono::duration<double, std::milli>(Clock::now() - indexSortStarted).count();
                const auto materializationStarted = Clock::now();
#endif
                std::vector<CandidateOccurrence> occurrences;
                std::vector<CandidateSourceRecord> sourceRecords;
                occurrences.reserve(contributors.size());
                sourceRecords.reserve(contributors.size());
                for (std::size_t contributorIndex : contributorOrder)
                {
                    SectionContributor& contributor = contributors[contributorIndex];
                    CandidateOccurrence occurrence = contributor.Occurrence;
                    occurrence.publicationOrdinal = static_cast<std::uint64_t>(occurrences.size());
                    occurrences.push_back(std::move(occurrence));
                    sourceRecords.push_back(std::move(contributor.Source));
                }
                publications.push_back(std::make_shared<const CandidatePublication>(
                    sectionRegions[sectionIndex], std::move(occurrences), std::move(sourceRecords),
                    FinalizeAcquisitionHash(hashesBySection[sectionIndex])));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                metrics.MaterializationMilliseconds +=
                    std::chrono::duration<double, std::milli>(Clock::now() - materializationStarted).count();
#endif
            }
            return publications;
        }

        /** Builds ordered immutable Section results from the currently committed regional dataset. */
        std::vector<std::shared_ptr<const CandidatePublication>> BuildSectionPublications(
            std::span<const CollectionRegion> sectionRegions)
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
            std::map<PublicationKey, CollectionRegion> uniqueRegions;
            for (const CollectionRegion& region : sectionRegions)
            {
                SectionCellCoordinate coordinate;
                if (!TryGetSectionGridIndex(region.xMapUnits, SectionSizeMapUnits, coordinate.X) ||
                    !TryGetSectionGridIndex(region.yMapUnits, SectionSizeMapUnits, coordinate.Y))
                {
                    throw std::invalid_argument("Section rectangle is not aligned to the configured grid.");
                }
                uniqueRegions.emplace(PublicationKey{coordinate.X, coordinate.Y}, region);
            }
            return RetainRequestedSectionPublications(sectionRegions, uniqueRegions);
        }

        /** Transactionally resolves exact request order through cached and newly built owners. */
        std::vector<std::shared_ptr<const CandidatePublication>> RetainRequestedSectionPublications(
            std::span<const CollectionRegion> sectionRegions,
            const std::map<PublicationKey, CollectionRegion>& uniqueRegions)
        {
            std::vector<CollectionRegion> missingRegions;
            std::vector<PublicationKey> missingKeys;
            const std::uint64_t ownerReuses = FindMissingSectionPublications(
                uniqueRegions, missingRegions, missingKeys);
            if (missingRegions.empty())
            {
                auto ordered = BuildOrderedSectionPublications(sectionRegions, RetainedSectionPublications);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                Diagnostics.SectionOwnerReuses += ownerReuses;
                ++Diagnostics.RepeatedQueriesWithoutRebuild;
#endif
                return ordered;
            }
            auto replacement = RetainedSectionPublications;
            [[maybe_unused]] const bool fullFootprintRebuild = FullFootprintRebuildPending;
            const auto rebuilt = BuildMissingSectionPublications(missingRegions);
            for (std::size_t index = 0; index < rebuilt.size(); ++index)
            {
                replacement[missingKeys[index]] = rebuilt[index];
            }
            auto ordered = BuildOrderedSectionPublications(sectionRegions, replacement);
            RetainedSectionPublications.swap(replacement);
            FullFootprintRebuildPending = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            Diagnostics.SectionOwnerReuses += ownerReuses;
            Diagnostics.SectionPublicationsRebuilt += missingRegions.size();
            if (fullFootprintRebuild)
            {
                ++Diagnostics.FullFootprintRebuilds;
            }
#endif
            return ordered;
        }

        /** Finds reused owners and records unique requested cells that require construction. */
        std::uint64_t FindMissingSectionPublications(
            const std::map<PublicationKey, CollectionRegion>& uniqueRegions,
            std::vector<CollectionRegion>& missingRegions,
            std::vector<PublicationKey>& missingKeys) const
        {
            missingRegions.reserve(uniqueRegions.size());
            missingKeys.reserve(uniqueRegions.size());
            std::uint64_t ownerReuses = 0;
            for (const auto& [key, region] : uniqueRegions)
            {
                const auto retained = RetainedSectionPublications.find(key);
                const bool exactOwner = retained != RetainedSectionPublications.end() &&
                    retained->second->SectionRegion().xMapUnits == region.xMapUnits &&
                    retained->second->SectionRegion().yMapUnits == region.yMapUnits &&
                    retained->second->SectionRegion().widthMapUnits == region.widthMapUnits &&
                    retained->second->SectionRegion().heightMapUnits == region.heightMapUnits;
                if (exactOwner)
                {
                    ++ownerReuses;
                    continue;
                }
                missingKeys.push_back(key);
                missingRegions.push_back(region);
            }
            return ownerReuses;
        }

        /** Builds missing Section owners in one timed batch without mutating retained state. */
        std::vector<std::shared_ptr<const CandidatePublication>> BuildMissingSectionPublications(
            const std::vector<CollectionRegion>& missingRegions)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto buildStarted = Clock::now();
            SectionPublicationBuildMetrics metrics;
            metrics.RequestedSections = missingRegions.size();
            ++Diagnostics.PublicationBuilderCalls;
#endif
            try
            {
                RequestedSectionLookup lookup;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                const auto lookupStarted = Clock::now();
#endif
                BuildRequestedSectionLookup(missingRegions, lookup);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                metrics.LookupMilliseconds =
                    std::chrono::duration<double, std::milli>(Clock::now() - lookupStarted).count();
#endif
                auto publications = BuildRequestedSectionPublications(missingRegions, lookup
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , metrics
#endif
                );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                metrics.TotalMilliseconds =
                    std::chrono::duration<double, std::milli>(Clock::now() - buildStarted).count();
                RecordSectionPublicationBuild(metrics);
#endif
                return publications;
            }
            catch (...)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                metrics.TotalMilliseconds =
                    std::chrono::duration<double, std::milli>(Clock::now() - buildStarted).count();
                RecordSectionPublicationBuild(metrics);
#endif
                throw;
            }
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Commits one bounded cache-miss build to interval totals and the slowest invocation. */
        void RecordSectionPublicationBuild(const SectionPublicationBuildMetrics& metrics) noexcept
        {
            Diagnostics.PublicationBuildMs += metrics.TotalMilliseconds;
            Diagnostics.PublicationBuildTotals.Accumulate(metrics);
            if (metrics.TotalMilliseconds >= Diagnostics.SlowestPublicationBuild.TotalMilliseconds)
            {
                Diagnostics.SlowestPublicationBuild = metrics;
            }
        }
#endif

        /** Restores exact request order and duplicate coordinates from retained owners. */
        std::vector<std::shared_ptr<const CandidatePublication>> BuildOrderedSectionPublications(
            std::span<const CollectionRegion> sectionRegions,
            const std::map<PublicationKey, std::shared_ptr<const CandidatePublication>>& owners) const
        {
            std::vector<std::shared_ptr<const CandidatePublication>> ordered;
            ordered.reserve(sectionRegions.size());
            for (const CollectionRegion& region : sectionRegions)
            {
                SectionCellCoordinate coordinate;
                TryGetSectionGridIndex(region.xMapUnits, SectionSizeMapUnits, coordinate.X);
                TryGetSectionGridIndex(region.yMapUnits, SectionSizeMapUnits, coordinate.Y);
                ordered.push_back(owners.at(PublicationKey{coordinate.X, coordinate.Y}));
            }
            return ordered;
        }

        /** Runs the existing batch distribution once for the unique missing Section cells. */
        std::vector<std::shared_ptr<const CandidatePublication>> BuildRequestedSectionPublications(
            std::span<const CollectionRegion> sectionRegions,
            const RequestedSectionLookup& lookup
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , SectionPublicationBuildMetrics& metrics
#endif
        ) const
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto boundsStarted = Clock::now();
#endif
            const std::vector<SectionAdmissionBounds> sectionBounds =
                BuildSectionAdmissionBounds(sectionRegions);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            metrics.BoundsMilliseconds =
                std::chrono::duration<double, std::milli>(Clock::now() - boundsStarted).count();
#endif
            std::vector<std::vector<SectionContributor>> contributorsBySection(sectionRegions.size());
            std::vector<AcquisitionHashAggregate> hashesBySection(sectionRegions.size());
            std::vector<std::size_t> matchingSections;
            matchingSections.reserve(8);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto entityLoopStarted = Clock::now();
#endif
            CollectRequestedSectionContributors(lookup, sectionBounds,
                contributorsBySection, hashesBySection, matchingSections
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , metrics
#endif
            );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            metrics.EntityLoopMilliseconds =
                std::chrono::duration<double, std::milli>(Clock::now() - entityLoopStarted).count();
            const auto freezeStarted = Clock::now();
#endif
            auto publications = FreezeSectionPublications(sectionRegions, contributorsBySection, hashesBySection
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                , metrics
#endif
            );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            metrics.FreezeMilliseconds =
                std::chrono::duration<double, std::milli>(Clock::now() - freezeStarted).count();
#endif
            return publications;
        }

        /** Traverses committed entities once and distributes only policy and spatial matches. */
        void CollectRequestedSectionContributors(
            const RequestedSectionLookup& lookup,
            std::span<const SectionAdmissionBounds> sectionBounds,
            std::vector<std::vector<SectionContributor>>& contributorsBySection,
            std::vector<AcquisitionHashAggregate>& hashesBySection,
            std::vector<std::size_t>& matchingSections
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , SectionPublicationBuildMetrics& metrics
#endif
        ) const
        {
            for (const auto& entity : Entities)
            {
                const Entry& entry = entity.second;
                const Facts::Building& building = entry.Data;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++metrics.EntitiesVisited;
                bool includeCandidate = false;
                {
                    SampledPublicationPhaseTimer timer(metrics.CandidatePolicy);
                    includeCandidate = ShouldIncludeCandidate(building);
                }
#else
                const bool includeCandidate = ShouldIncludeCandidate(building);
#endif
                if (!includeCandidate)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++metrics.PolicyExcluded;
#endif
                    continue;
                }
                CandidateOccurrence occurrence;
                if (!FindRequestedSectionsForCandidate(building, lookup, sectionBounds,
                    matchingSections, occurrence
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , metrics
#endif
                ))
                {
                    continue;
                }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++metrics.DistinctContributors;
                ++metrics.EntitiesConverted;
#endif
                AppendRequestedContributor(entry, occurrence, matchingSections,
                    contributorsBySection, hashesBySection
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    , metrics
#endif
                );
            }
        }

        /** Computes exact requested Section matches for one policy-admitted occurrence. */
        static bool FindRequestedSectionsForCandidate(
            const Facts::Building& building,
            const RequestedSectionLookup& lookup,
            std::span<const SectionAdmissionBounds> sectionBounds,
            std::vector<std::size_t>& matchingSections,
            CandidateOccurrence& occurrence
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , SectionPublicationBuildMetrics& metrics
#endif
        )
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            {
                SampledPublicationPhaseTimer timer(metrics.CandidateOccurrence);
                occurrence = MakeCandidateOccurrence(building);
            }
            SectionGridRange reachedRange;
            bool hasReachedRange = false;
            {
                SampledPublicationPhaseTimer timer(metrics.ReachedRange);
                hasReachedRange = TryGetReachedSectionGridRange(building, occurrence, lookup, reachedRange);
            }
            if (!hasReachedRange)
            {
                ++metrics.SpatialBoundsExcluded;
                return false;
            }
            {
                SampledPublicationPhaseTimer timer(metrics.MatchingSections);
                FindMatchingSections(building, occurrence, reachedRange, sectionBounds, lookup, matchingSections);
            }
#else
            occurrence = MakeCandidateOccurrence(building);
            SectionGridRange reachedRange;
            if (!TryGetReachedSectionGridRange(building, occurrence, lookup, reachedRange))
            {
                return false;
            }
            FindMatchingSections(building, occurrence, reachedRange, sectionBounds, lookup, matchingSections);
#endif
            if (!matchingSections.empty())
            {
                return true;
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ++metrics.NoMatchingSection;
#endif
            return false;
        }

        /** Converts one admitted occurrence and reuses its committed contribution before distribution. */
        void AppendRequestedContributor(
            const Entry& entry,
            const CandidateOccurrence& occurrence,
            const std::vector<std::size_t>& matchingSections,
            std::vector<std::vector<SectionContributor>>& contributorsBySection,
            std::vector<AcquisitionHashAggregate>& hashesBySection
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            , SectionPublicationBuildMetrics& metrics
#endif
        ) const
        {
            const Facts::Building& building = entry.Data;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            CandidateSourceRecord source = [this, &building, &metrics]()
            {
                SampledPublicationPhaseTimer timer(metrics.SourceConversion);
                return BuildCandidateSourceRecord(building);
            }();
            metrics.ContributorCopies += matchingSections.size();
            {
                SampledPublicationPhaseTimer timer(metrics.ContributorDistribution);
                AppendContributorToSections(building, occurrence, source, entry.CompletedContribution, matchingSections,
                    contributorsBySection, hashesBySection, metrics);
            }
#else
            CandidateSourceRecord source = BuildCandidateSourceRecord(building);
            AppendContributorToSections(building, occurrence, source, entry.CompletedContribution, matchingSections,
                contributorsBySection, hashesBySection);
#endif
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
            LOG_INFO("MiniMap: BuildingMap regional_scan intervalMs=%.1f scans:%llu complete:%llu scan={n:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f} chunks:%llu examined:%llu admitted:%llu fastPath:%llu fullAcquisition:%llu missingOrigin:%llu added:%llu evicted:%llu failed:%llu entities:%llu pending:%llu requests={equal:%llu changed:%llu contained:%llu immediate:%llu} reuse={success:%llu rejected:%llu} scanReason={immediate:%llu mutation:%llu} failedByReason={immediate:%llu mutation:%llu}",
                intervalMs, static_cast<unsigned long long>(d.MembershipScans),
                static_cast<unsigned long long>(d.MembershipScans - d.FailedScans),
                static_cast<unsigned long long>(d.Membership.Samples), d.Membership.TotalMs, d.Membership.MeanMs(), d.Membership.MaxMs,
                static_cast<unsigned long long>(d.Chunks), static_cast<unsigned long long>(d.Examined),
                static_cast<unsigned long long>(d.Admitted),
                static_cast<unsigned long long>(d.FastPathEligible),
                static_cast<unsigned long long>(d.FullAcquisitionRequired),
                static_cast<unsigned long long>(d.MissingOrigin),
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
                static_cast<unsigned long long>(d.FailedImmediateScans),
                static_cast<unsigned long long>(d.FailedMutationScans));
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

        /** Emits retained Section publication reuse and build totals for one collector interval. */
        void ReportPublicationMeasurements(double intervalMs) const
        {
            const auto& d = Diagnostics;
            LOG_INFO("MiniMap: BuildingMap section_publications intervalMs=%.1f retained:%llu movementPreserved:%llu ownerReuses:%llu rebuilt:%llu fullFootprintRebuilds:%llu builderCalls:%llu repeatedQueriesWithoutBuild:%llu buildMs:%.3f",
                intervalMs,
                static_cast<unsigned long long>(RetainedSectionPublications.size()),
                static_cast<unsigned long long>(d.SectionOwnersPreservedOnMovement),
                static_cast<unsigned long long>(d.SectionOwnerReuses),
                static_cast<unsigned long long>(d.SectionPublicationsRebuilt),
                static_cast<unsigned long long>(d.FullFootprintRebuilds),
                static_cast<unsigned long long>(d.PublicationBuilderCalls),
                static_cast<unsigned long long>(d.RepeatedQueriesWithoutRebuild),
                d.PublicationBuildMs);
            ReportSectionPublicationBuildMeasurements(intervalMs);
        }

        /** Reports exact outer phases, sampled nested estimates, and the slowest cache-miss build. */
        void ReportSectionPublicationBuildMeasurements(double intervalMs) const
        {
            ReportSectionPublicationBuildTotals(intervalMs);
            ReportSectionPublicationBuildEstimates(intervalMs, Diagnostics.PublicationBuildTotals);
            if (Diagnostics.PublicationBuilderCalls == 0)
            {
                return;
            }
            ReportSlowestSectionPublicationBuild(intervalMs, Diagnostics.SlowestPublicationBuild);
        }

        /** Reports interval totals for inclusive time, nonoverlapping phases, and exact counts. */
        void ReportSectionPublicationBuildTotals(double intervalMs) const
        {
            const auto& metrics = Diagnostics.PublicationBuildTotals;
            LOG_INFO("MiniMap: BuildingMap section_publication_build intervalMs=%.1f calls:%llu inclusiveMs:%.3f exactMs={lookup:%.3f bounds:%.3f entityLoop:%.3f freezeSort:%.3f residual:%.3f} freezeSortChildrenMs={indexSort:%.3f materialization:%.3f} counts={visited:%llu policyExcluded:%llu spatialExcluded:%llu noMatchingSection:%llu converted:%llu contributors:%llu copies:%llu sourceCopies:%llu sourceMoves:%llu requestedSections:%llu finalOccurrences:%llu}",
                intervalMs,
                static_cast<unsigned long long>(Diagnostics.PublicationBuilderCalls),
                metrics.TotalMilliseconds,
                metrics.LookupMilliseconds,
                metrics.BoundsMilliseconds,
                metrics.EntityLoopMilliseconds,
                metrics.FreezeMilliseconds,
                metrics.ResidualMilliseconds(),
                metrics.IndexSortMilliseconds,
                metrics.MaterializationMilliseconds,
                static_cast<unsigned long long>(metrics.EntitiesVisited),
                static_cast<unsigned long long>(metrics.PolicyExcluded),
                static_cast<unsigned long long>(metrics.SpatialBoundsExcluded),
                static_cast<unsigned long long>(metrics.NoMatchingSection),
                static_cast<unsigned long long>(metrics.EntitiesConverted),
                static_cast<unsigned long long>(metrics.DistinctContributors),
                static_cast<unsigned long long>(metrics.ContributorCopies),
                static_cast<unsigned long long>(metrics.SourceCopies),
                static_cast<unsigned long long>(metrics.SourceMoves),
                static_cast<unsigned long long>(metrics.RequestedSections),
                static_cast<unsigned long long>(metrics.FinalOccurrences));
        }

        /** Reports the aggregate nested phase estimates and their sample/operation counts. */
        void ReportSectionPublicationBuildEstimates(
            double intervalMs,
            const SectionPublicationBuildMetrics& metrics) const
        {
            LOG_INFO("MiniMap: BuildingMap section_publication_build_estimate intervalMs=%.1f sampling:first+each256 nestedEstimateMs={policy:%.3f occurrence:%.3f reachedRange:%.3f matching:%.3f source:%.3f distribution:%.3f} samples={policy:%llu/%llu occurrence:%llu/%llu reachedRange:%llu/%llu matching:%llu/%llu source:%llu/%llu distribution:%llu/%llu}",
                intervalMs,
                metrics.CandidatePolicy.EstimatedMilliseconds(),
                metrics.CandidateOccurrence.EstimatedMilliseconds(),
                metrics.ReachedRange.EstimatedMilliseconds(),
                metrics.MatchingSections.EstimatedMilliseconds(),
                metrics.SourceConversion.EstimatedMilliseconds(),
                metrics.ContributorDistribution.EstimatedMilliseconds(),
                static_cast<unsigned long long>(metrics.CandidatePolicy.Samples),
                static_cast<unsigned long long>(metrics.CandidatePolicy.Operations),
                static_cast<unsigned long long>(metrics.CandidateOccurrence.Samples),
                static_cast<unsigned long long>(metrics.CandidateOccurrence.Operations),
                static_cast<unsigned long long>(metrics.ReachedRange.Samples),
                static_cast<unsigned long long>(metrics.ReachedRange.Operations),
                static_cast<unsigned long long>(metrics.MatchingSections.Samples),
                static_cast<unsigned long long>(metrics.MatchingSections.Operations),
                static_cast<unsigned long long>(metrics.SourceConversion.Samples),
                static_cast<unsigned long long>(metrics.SourceConversion.Operations),
                static_cast<unsigned long long>(metrics.ContributorDistribution.Samples),
                static_cast<unsigned long long>(metrics.ContributorDistribution.Operations));
        }

        /** Reports the slowest individual cache-miss builder call in this interval. */
        void ReportSlowestSectionPublicationBuild(
            double intervalMs,
            const SectionPublicationBuildMetrics& metrics) const
        {
            ReportSlowestSectionPublicationBuildTotals(intervalMs, metrics);
            ReportSectionPublicationBuildEstimatesForSlowest(intervalMs, metrics);
        }

        /** Reports exact outer phase timings and operation counts for the slowest call. */
        void ReportSlowestSectionPublicationBuildTotals(
            double intervalMs,
            const SectionPublicationBuildMetrics& metrics) const
        {
            LOG_INFO("MiniMap: BuildingMap section_publication_build_worst intervalMs=%.1f inclusiveMs:%.3f exactMs={lookup:%.3f bounds:%.3f entityLoop:%.3f freezeSort:%.3f residual:%.3f} freezeSortChildrenMs={indexSort:%.3f materialization:%.3f} counts={visited:%llu policyExcluded:%llu spatialExcluded:%llu noMatchingSection:%llu converted:%llu contributors:%llu copies:%llu sourceCopies:%llu sourceMoves:%llu requestedSections:%llu finalOccurrences:%llu}",
                intervalMs,
                metrics.TotalMilliseconds,
                metrics.LookupMilliseconds,
                metrics.BoundsMilliseconds,
                metrics.EntityLoopMilliseconds,
                metrics.FreezeMilliseconds,
                metrics.ResidualMilliseconds(),
                metrics.IndexSortMilliseconds,
                metrics.MaterializationMilliseconds,
                static_cast<unsigned long long>(metrics.EntitiesVisited),
                static_cast<unsigned long long>(metrics.PolicyExcluded),
                static_cast<unsigned long long>(metrics.SpatialBoundsExcluded),
                static_cast<unsigned long long>(metrics.NoMatchingSection),
                static_cast<unsigned long long>(metrics.EntitiesConverted),
                static_cast<unsigned long long>(metrics.DistinctContributors),
                static_cast<unsigned long long>(metrics.ContributorCopies),
                static_cast<unsigned long long>(metrics.SourceCopies),
                static_cast<unsigned long long>(metrics.SourceMoves),
                static_cast<unsigned long long>(metrics.RequestedSections),
                static_cast<unsigned long long>(metrics.FinalOccurrences));
        }

        /** Reports sampled nested phase estimates for the slowest call. */
        void ReportSectionPublicationBuildEstimatesForSlowest(
            double intervalMs,
            const SectionPublicationBuildMetrics& metrics) const
        {
            LOG_INFO("MiniMap: BuildingMap section_publication_build_worst_estimate intervalMs=%.1f sampling:first+each256 nestedEstimateMs={policy:%.3f occurrence:%.3f reachedRange:%.3f matching:%.3f source:%.3f distribution:%.3f} samples={policy:%llu/%llu occurrence:%llu/%llu reachedRange:%llu/%llu matching:%llu/%llu source:%llu/%llu distribution:%llu/%llu}",
                intervalMs,
                metrics.CandidatePolicy.EstimatedMilliseconds(),
                metrics.CandidateOccurrence.EstimatedMilliseconds(),
                metrics.ReachedRange.EstimatedMilliseconds(),
                metrics.MatchingSections.EstimatedMilliseconds(),
                metrics.SourceConversion.EstimatedMilliseconds(),
                metrics.ContributorDistribution.EstimatedMilliseconds(),
                static_cast<unsigned long long>(metrics.CandidatePolicy.Samples),
                static_cast<unsigned long long>(metrics.CandidatePolicy.Operations),
                static_cast<unsigned long long>(metrics.CandidateOccurrence.Samples),
                static_cast<unsigned long long>(metrics.CandidateOccurrence.Operations),
                static_cast<unsigned long long>(metrics.ReachedRange.Samples),
                static_cast<unsigned long long>(metrics.ReachedRange.Operations),
                static_cast<unsigned long long>(metrics.MatchingSections.Samples),
                static_cast<unsigned long long>(metrics.MatchingSections.Operations),
                static_cast<unsigned long long>(metrics.SourceConversion.Samples),
                static_cast<unsigned long long>(metrics.SourceConversion.Operations),
                static_cast<unsigned long long>(metrics.ContributorDistribution.Samples),
                static_cast<unsigned long long>(metrics.ContributorDistribution.Operations));
        }

        /** Emits the numeric-only worst scan retained for one completed interval. */
        void ReportWorstMembershipScan(double intervalMs) const
        {
            const auto& d = Diagnostics;
            if (d.WorstScan.TotalMs > 0.0)
            {
                const auto& worst = d.WorstScan;
                LOG_INFO("MiniMap: BuildingMap regional_scan_worst intervalMs=%.1f totalMs:%.3f chunkMs:%.3f ownedCopyMs:%.3f deferredMs:%.3f reconcileMs:%.3f chunks:%llu entities:%llu admitted:%llu fastPath:%llu fullAcquisition:%llu pending:%llu added:%llu evicted:%llu",
                    intervalMs, worst.TotalMs, worst.ChunkExecutionMs, worst.OwnedFactCopyingMs,
                    worst.DeferredAcquisitionMs, worst.ReconciliationMs,
                    static_cast<unsigned long long>(worst.Chunks), static_cast<unsigned long long>(worst.Entities),
                    static_cast<unsigned long long>(worst.Admitted),
                    static_cast<unsigned long long>(worst.FastPathEligible),
                    static_cast<unsigned long long>(worst.FullAcquisitionRequired),
                    static_cast<unsigned long long>(worst.Pending), static_cast<unsigned long long>(worst.Added),
                    static_cast<unsigned long long>(worst.Removed));
            }
        }

        /** Emits and resets bounded measurement aggregates only from a permitted Update call. */
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
                ReportPublicationMeasurements(intervalMs);
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
        const bool negate = ::Services::Map::Buildings::SourceContentPrecision::QuaternionNeedsNegation(rotation);
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

    /** Hashes one floating value with the shared six-decimal source policy. */
    void BuildingCollectorV2::AppendHashDouble(std::uint64_t& hash, double value) noexcept
    {
        AppendHashInteger(hash, ::Services::Map::Buildings::SourceContentPrecision::CanonicalBits(value));
    }

    /** Exposes a finite XY origin independently of rotation, scale or Z validity. */
    bool BuildingCollectorV2::HasValidMapPosition(const BuildingAcquiredFacts::Building& building) noexcept
    {
        return std::isfinite(building.Position[0]) && std::isfinite(building.Position[1]);
    }

    /** Creates inert CPU state and validates the Section size and retry interval. */
    BuildingCollectorV2::BuildingCollectorV2(
        double sectionSizeMapUnits,
        std::chrono::milliseconds retryInterval)
        : state_(std::make_unique<State>(sectionSizeMapUnits, retryInterval))
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
        if (!state_->SplinePolicyInitializationAttempted)
            state_->InitializeSplinePolicyTable();
        if (state_->Types)
            return true;
        auto types = std::make_shared<State::Metadata>();
        if (!types->Initialize() || !state_->DefinitionAcquisition.Initialize())
            return false;
        if (!BuildingSplineSource::CopyRuntimeFieldContract(state_->RuntimeFields))
            return false;
        state_->Types = std::move(types);
        return true;
    }

    /** Releases native resources before the collector is destroyed. */
    void BuildingCollectorV2::Shutdown()
    {
        ShutdownResources();
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
        if (state_->IncludeDroneRails == enabled)
        {
            return;
        }
        state_->IncludeDroneRails = enabled;
        state_->RetainedSectionPublications.clear();
        state_->FullFootprintRebuildPending = true;
        state_->ImmediateScanRequired = true;
    }

    /** Reads the stored hash for the currently committed regional dataset. */
    std::uint64_t BuildingCollectorV2::AcquisitionHash() const noexcept
    {
        return state_->AcquisitionContentHash;
    }

    /** Looks up or transactionally builds exact retained Section publications. */
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
