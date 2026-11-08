// Copies rooted authored definition and spline-profile values with per-instance metadata.
// It converts profile FRotator values to the established neutral quaternion convention.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingDefinitionAcquisition.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/Chimera_classes.hpp>
#include <SDK/Engine_classes.hpp>
#include <SDK/Engine_parameters.hpp>
#include <SDK/MassRepresentation_classes.hpp>
#include <SDK/MassSpawner_classes.hpp>
#include <SDK/MassSpawner_structs.hpp>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MiniMap::BuildingAcquisition
{
    using Facts = BuildingAcquiredFacts;
    using Status = Facts::Status;

    /** Roots conversion metadata independently; a Copy call retains it across reentry. */
    struct BuildingDefinitionAcquisition::Metadata
    {
        std::vector<SDK::UObject*> Roots;
        SDK::UObject* Library = nullptr;
        SDK::UClass* BuildingData = nullptr;
        SDK::UClass* ConfigAsset = nullptr;
        SDK::UClass* Blueprint = nullptr;
        SDK::UClass* SplineMesh = nullptr;
        SDK::UClass* Walkway = nullptr;
        SDK::UClass* CosmeticTrait = nullptr;
        SDK::UClass* VisualizationTrait = nullptr;
        SDK::UFunction* ClassConversion = nullptr;
        SDK::UFunction* ObjectConversion = nullptr;

        /** Releases balanced native roots while native services are still alive. */
        ~Metadata()
        {
            const auto* native = MiniMapNative::Get();
            assert(Roots.empty() || native);
            if (native)
                for (auto* root : Roots)
                    native->asset.releaseObject(root);
        }

        /** Finds resident metadata and roots it with allocation-safe bookkeeping. */
        SDK::UObject* Lookup(const wchar_t* path, SDK::UClass* type = nullptr, SDK::UObject* outer = nullptr)
        {
            auto* objects = Services::TryObjects();
            const auto result = objects->FindResident(type, outer, path, outer != nullptr);
            auto* value = result.value ? *result.value : nullptr;
            if (value)
            {
                Roots.push_back(value);
                MiniMapNative::Get()->asset.retainObject(value);
            }
            return value;
        }

        /** Initializes required metadata only; destruction rolls back partial roots. */
        bool Initialize()
        {
            auto* libraryType = static_cast<SDK::UClass*>(Lookup(L"/Script/Engine.KismetSystemLibrary"));
            auto* functionType = static_cast<SDK::UClass*>(Lookup(L"/Script/CoreUObject.Function"));
            BuildingData = static_cast<SDK::UClass*>(Lookup(L"/Script/Chimera.CrBuildingData"));
            ConfigAsset = static_cast<SDK::UClass*>(Lookup(L"/Script/MassSpawner.MassEntityConfigAsset"));
            Blueprint = static_cast<SDK::UClass*>(Lookup(L"/Script/Engine.BlueprintGeneratedClass"));
            SplineMesh = static_cast<SDK::UClass*>(Lookup(L"/Script/Engine.SplineMeshComponent"));
            Walkway = static_cast<SDK::UClass*>(Lookup(L"/Script/Chimera.CrModularRampWalkway"));
            CosmeticTrait = static_cast<SDK::UClass*>(Lookup(L"/Script/Chimera.CrMassRepVisCosmeticsTrait"));
            VisualizationTrait = static_cast<SDK::UClass*>(Lookup(L"/Script/MassRepresentation.MassVisualizationTrait"));
            if (!libraryType || !functionType || !BuildingData || !ConfigAsset || !Blueprint || !SplineMesh ||
                !Walkway || !CosmeticTrait || !VisualizationTrait || !libraryType->ClassDefaultObject)
                return false;
            Library = libraryType->ClassDefaultObject;
            Roots.push_back(Library);
            MiniMapNative::Get()->asset.retainObject(Library);
            ClassConversion = static_cast<SDK::UFunction*>(Lookup(
                L"Conv_SoftClassReferenceToString", functionType, libraryType));
            ObjectConversion = static_cast<SDK::UFunction*>(Lookup(
                L"Conv_SoftObjectReferenceToString", functionType, libraryType));
            return ClassConversion && ObjectConversion;
        }

        /** Balances temporary asset roots across profile traversal and any nested loads. */
        struct TemporaryRoots
        {
            std::vector<SDK::UObject*> Values;
            /** Releases temporary authored assets before the caller returns to collector state. */
            ~TemporaryRoots()
            {
                const auto* native = MiniMapNative::Get();
                assert(Values.empty() || native);
                if (native)
                    for (auto value = Values.rbegin(); value != Values.rend(); ++value)
                        native->asset.releaseObject(*value);
            }
            /** Retains a loaded object after allocation-safe ownership bookkeeping. */
            void Retain(SDK::UObject* value)
            {
                Values.push_back(value);
                MiniMapNative::Get()->asset.retainObject(value);
            }
        };

        /** Orders profile candidates exactly as the legacy definition preparation does. */
        struct ProfileCandidate
        {
            ::Services::Map::Buildings::SplineProfileAlternative Values;
            std::array<std::int64_t, 12> Order{};
        };

        /** Validates an array before reading class templates or Mass traits. */
        template<class Array>
        static bool IsArrayReadable(const Array& values)
        {
            return values.Num() >= 0 && values.Max() >= values.Num() &&
                (values.Num() == 0 || values.GetDataPtr() != nullptr);
        }

        /** Copies a native vector into a neutral XYZ value. */
        static std::array<double, 3> CopyVector(const SDK::FVector& value)
        {
            return {value.X, value.Y, value.Z};
        }

        /** Copies valid StaticMesh bounds while preserving invalid authored values for reporting. */
        static bool CopyBounds(SDK::UStaticMesh* mesh,
            ::Services::Map::Buildings::SplineProfileAlternative& output)
        {
            if (!mesh)
            {
                output.Status = ::Services::Map::Buildings::SplineProfileAlternativeStatus::MissingMesh;
                return false;
            }
            const auto& bounds = mesh->ExtendedBounds;
            output.HasBounds = true;
            output.Bounds.Origin = CopyVector(bounds.Origin);
            output.Bounds.Extent = CopyVector(bounds.BoxExtent);
            output.Bounds.SphereRadius = bounds.SphereRadius;
            for (const double extent : output.Bounds.Extent)
                if (!std::isfinite(extent) || extent < 0.0)
                    return false;
            if (!std::isfinite(output.Bounds.SphereRadius) || output.Bounds.SphereRadius < 0.0)
                return false;
            for (const double coordinate : output.Bounds.Origin)
                if (!std::isfinite(coordinate))
                    return false;
            return true;
        }

        /** Copies component values and converts FRotator with the established legacy convention. */
        static ::Services::Map::Buildings::SplineProfileAlternativeStatus CopyComponent(
            const SDK::USplineMeshComponent& component,
            ::Services::Map::Buildings::SplineProfileAlternative& output)
        {
            output.Transform.Translation = CopyVector(component.RelativeLocation);
            output.Transform.RotationDegrees = {component.RelativeRotation.Pitch,
                component.RelativeRotation.Yaw, component.RelativeRotation.Roll};
            constexpr double halfRadians = 3.14159265358979323846 / 360.0;
            const double pitch = std::remainder(output.Transform.RotationDegrees[0], 360.0) * halfRadians;
            const double yaw = std::remainder(output.Transform.RotationDegrees[1], 360.0) * halfRadians;
            const double roll = std::remainder(output.Transform.RotationDegrees[2], 360.0) * halfRadians;
            const double sinPitch = std::sin(pitch);
            const double cosPitch = std::cos(pitch);
            const double sinYaw = std::sin(yaw);
            const double cosYaw = std::cos(yaw);
            const double sinRoll = std::sin(roll);
            const double cosRoll = std::cos(roll);
            output.Transform.Rotation = {
                cosRoll * sinPitch * sinYaw - sinRoll * cosPitch * cosYaw,
                -cosRoll * sinPitch * cosYaw - sinRoll * cosPitch * sinYaw,
                cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw,
                cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw};
            output.Transform.Scale = CopyVector(component.RelativeScale3D);
            const auto& params = component.SplineParams;
            output.CrossSection.StartScale = {params.StartScale.X, params.StartScale.Y};
            output.CrossSection.EndScale = {params.EndScale.X, params.EndScale.Y};
            output.CrossSection.StartOffset = {params.StartOffset.X, params.StartOffset.Y};
            output.CrossSection.EndOffset = {params.EndOffset.X, params.EndOffset.Y};
            output.CrossSection.StartRoll = params.StartRoll;
            output.CrossSection.EndRoll = params.EndRoll;
            output.CrossSection.Up = CopyVector(component.SplineUpDir);
            output.ForwardAxis = static_cast<std::uint8_t>(component.ForwardAxis);
            const auto finite = [](double value) { return std::isfinite(value); };
            for (const double value : output.Transform.Translation)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.Transform.RotationDegrees)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.Transform.Rotation)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.Transform.Scale)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.CrossSection.StartScale)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.CrossSection.EndScale)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.CrossSection.StartOffset)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.CrossSection.EndOffset)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            for (const double value : output.CrossSection.Up)
                if (!finite(value)) return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            if (!finite(output.CrossSection.StartRoll) || !finite(output.CrossSection.EndRoll))
                return ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidValues;
            if (output.ForwardAxis > 2)
                return ::Services::Map::Buildings::SplineProfileAlternativeStatus::Unsupported;
            return ::Services::Map::Buildings::SplineProfileAlternativeStatus::Complete;
        }

        /** Reads the walkway class-default spline mesh before its Blueprint component templates. */
        bool AppendWalkwayDefault(SDK::UClass* type, int rank, std::vector<ProfileCandidate>& candidates) const
        {
            auto* cdo = type ? type->ClassDefaultObject : nullptr;
            if (!cdo || !cdo->IsA(Walkway))
                return true;
            const auto* walkway = static_cast<const SDK::ACrModularRampWalkway*>(cdo);
            ProfileCandidate candidate;
            candidate.Order = {rank, 0, type == Walkway ? 0 : 1, type->Index, 0, 0, 0, 0, 0,
                cdo->Index, 0, static_cast<std::int64_t>(candidates.size())};
            if (!CopyBounds(walkway->SplineMesh, candidate.Values))
            {
                candidate.Values.Status = walkway->SplineMesh
                    ? ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidBounds
                    : ::Services::Map::Buildings::SplineProfileAlternativeStatus::MissingMesh;
            }
            candidates.push_back(std::move(candidate));
            return true;
        }

        /** Copies ordered spline mesh templates throughout a class's Blueprint superclass chain. */
        bool AppendBlueprintTemplates(SDK::UClass* type, int rank,
            std::vector<ProfileCandidate>& candidates) const
        {
            auto* cdo = type ? type->ClassDefaultObject : nullptr;
            if (!type || !cdo)
                return false;
            for (auto* current = type; current; current = static_cast<SDK::UClass*>(current->SuperStruct))
            {
                if (!current->IsA(Blueprint))
                    continue;
                const auto& components = static_cast<SDK::UBlueprintGeneratedClass*>(current)->ComponentTemplates;
                if (!IsArrayReadable(components))
                    return false;
                for (auto* component : components)
                {
                    if (!component || !component->IsA(SplineMesh))
                        continue;
                    const auto* spline = static_cast<const SDK::USplineMeshComponent*>(component);
                    ProfileCandidate candidate;
                    candidate.Order = {rank, 1, 1, type->Index, 0, current->Index, 0,
                        component->Index, 0, cdo->Index, 0, static_cast<std::int64_t>(candidates.size())};
                    const bool validBounds = CopyBounds(spline->StaticMesh, candidate.Values);
                    const auto componentStatus = CopyComponent(*spline, candidate.Values);
                    if (!validBounds)
                        candidate.Values.Status = spline->StaticMesh
                            ? ::Services::Map::Buildings::SplineProfileAlternativeStatus::InvalidBounds
                            : ::Services::Map::Buildings::SplineProfileAlternativeStatus::MissingMesh;
                    else if (componentStatus != ::Services::Map::Buildings::SplineProfileAlternativeStatus::Complete)
                        candidate.Values.Status = componentStatus;
                    candidates.push_back(std::move(candidate));
                }
            }
            return true;
        }

        /** Appends one actor class using the legacy CDO-then-template ordering. */
        bool AppendActorProfiles(SDK::UClass* type, int rank,
            std::vector<ProfileCandidate>& candidates) const
        {
            if (!type || !type->ClassDefaultObject)
                return false;
            return AppendWalkwayDefault(type, rank, candidates) &&
                AppendBlueprintTemplates(type, rank, candidates);
        }

        /** Walks one rooted entity-config parent chain and appends recognized visualization templates. */
        ::Services::Map::Buildings::SplineProfileAcquisitionStatus AppendConfigProfiles(
            SDK::UMassEntityConfigAsset* config, std::vector<ProfileCandidate>& candidates) const
        {
            std::unordered_set<const SDK::UMassEntityConfigAsset*> visited;
            for (; config; config = config->Config.Parent)
            {
                if (!visited.insert(config).second || !IsArrayReadable(config->Config.Traits))
                    return ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Invalid;
                for (auto* trait : config->Config.Traits)
                {
                    if (!trait)
                        continue;
                    SDK::UClass* high = nullptr;
                    SDK::UClass* low = nullptr;
                    if (trait->IsA(CosmeticTrait))
                    {
                        const auto* visual = static_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait);
                        high = visual->HighResTemplateActor;
                        low = visual->LowResTemplateActor;
                    }
                    else if (trait->IsA(VisualizationTrait))
                    {
                        const auto* visual = static_cast<const SDK::UMassVisualizationTrait*>(trait);
                        high = visual->HighResTemplateActor;
                        low = visual->LowResTemplateActor;
                    }
                    if ((high && !AppendActorProfiles(high, 0, candidates)) ||
                        (low && !AppendActorProfiles(low, 1, candidates)))
                        return ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Invalid;
                }
            }
            return ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Complete;
        }

        /** Loads the copied authored-class reference and appends its rank-two profile candidates. */
        ::Services::Map::Buildings::SplineProfileAcquisitionStatus AppendAuthoredProfiles(
            SDK::TSoftClassPtr<SDK::UClass> reference, TemporaryRoots& roots,
            std::vector<ProfileCandidate>& candidates) const
        {
            const auto* native = MiniMapNative::Get();
            auto* authored = static_cast<SDK::UClass*>(native->asset.loadSynchronous(&reference));
            if (!authored)
                return ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Unavailable;
            roots.Retain(authored);
            if (!AppendActorProfiles(authored, 2, candidates))
                return ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Invalid;
            return ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Complete;
        }

        /** Stable-sorts and freezes profile alternatives while preserving the established selector order. */
        static std::shared_ptr<const ::Services::Map::Buildings::SplineProfileSet> FreezeProfiles(
            std::vector<ProfileCandidate>& candidates)
        {
            std::stable_sort(candidates.begin(), candidates.end(),
                [](const ProfileCandidate& first, const ProfileCandidate& second)
                {
                    return first.Order < second.Order;
                });
            auto profiles = std::make_shared<::Services::Map::Buildings::SplineProfileSet>();
            profiles->Alternatives.reserve(candidates.size());
            for (auto& candidate : candidates)
                profiles->Alternatives.push_back(std::move(candidate.Values));
            return profiles;
        }

        /** Acquires config and authored-class profiles using temporary roots and copied soft references. */
        std::pair<::Services::Map::Buildings::SplineProfileAcquisitionStatus,
            std::shared_ptr<const ::Services::Map::Buildings::SplineProfileSet>> CopyProfiles(
                const DefinitionOwner& definition) const
        {
            const auto& placement = *definition.Root;
            auto configReference = placement.EntityType.EntityConfig;
            auto authoredReference = placement.ActorClass;
            auto* config = placement.EntityType.EntityConfigPtr;
            TemporaryRoots roots;
            if (!config && configReference.ObjectID.AssetPath.PackageName.ComparisonIndex != 0)
            {
                config = static_cast<SDK::UMassEntityConfigAsset*>(
                    MiniMapNative::Get()->asset.loadSynchronous(&configReference));
                if (!config)
                    return {::Services::Map::Buildings::SplineProfileAcquisitionStatus::Unavailable, {}};
            }
            if (config && !config->IsA(ConfigAsset))
                return {::Services::Map::Buildings::SplineProfileAcquisitionStatus::Invalid, {}};
            if (config)
                roots.Retain(config);
            std::vector<ProfileCandidate> candidates;
            if (config)
            {
                const auto configStatus = AppendConfigProfiles(config, candidates);
                if (configStatus != ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Complete)
                    return {configStatus, FreezeProfiles(candidates)};
            }
            const auto authoredStatus = AppendAuthoredProfiles(authoredReference, roots, candidates);
            if (authoredStatus != ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Complete)
                return {authoredStatus, FreezeProfiles(candidates)};
            auto profiles = FreezeProfiles(candidates);
            if (profiles->Alternatives.empty())
                return {::Services::Map::Buildings::SplineProfileAcquisitionStatus::Unsupported, profiles};
            return {::Services::Map::Buildings::SplineProfileAcquisitionStatus::Complete, profiles};
        }

        /** Converts engine text to owned UTF-8; empty/None represents a missing reference. */
        static Status CopyString(const SDK::FString& value, std::string& output)
        {
            const auto count = value.Num();
            if (count == 0)
                return Status::Missing;
            if (count < 1 || value.Max() < count || !value.CStr() || value.CStr()[count - 1] != 0)
                return Status::Invalid;
            if (count == 1)
                return Status::Missing;
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                value.CStr(), count - 1, nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return Status::Invalid;
            std::string path(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                value.CStr(), count - 1, path.data(), size, nullptr, nullptr) != size)
                return Status::Invalid;
            if (path == "None")
                return Status::Missing;
            output = std::move(path);
            return Status::Ready;
        }

        /** Releases the returned FString even on invocation/conversion/allocation failure. */
        struct ReturnedString
        {
            SDK::FString& Value;
            Services::IObjects& Objects;
            /** Returns the engine allocation using the existing object-service contract. */
            ~ReturnedString()
            {
                if (auto* text = Value.CStr())
                {
                    const auto status = Objects.FreeEngineAllocation(const_cast<wchar_t*>(text));
                    if (status.error != Services::Error::None)
                        Services::LogMessage(Services::Level::Error, "MiniMap: BuildingMap soft-reference string release failed");
                }
            }
        };

        /** Invokes one rooted conversion with an already-copied soft reference. */
        template<class Parameters>
        Status Convert(SDK::UFunction* function, Parameters& parameters, std::string& output) const
        {
            auto* objects = Services::TryObjects();
            if (!objects)
                return Status::Unavailable;
            ReturnedString owner{parameters.ReturnValue, *objects};
            const auto invocation = objects->Invoke(Library, function, &parameters);
            if (invocation.error != Services::Error::None)
                return Status::Unavailable;
            return CopyString(parameters.ReturnValue, output);
        }

        /** Copies authored Interior evidence before any reflection call can reenter. */
        void CopyInterior(const SDK::UAuActorPlacementData& placement, Facts::Definition& output) const
        {
            output.InteriorStatus = Status::Ready;
            if (!placement.IsA(BuildingData))
                return;
            const auto& tags = static_cast<const SDK::UCrBuildingData&>(placement).Tags;
            if (tags.Num() < 0 || tags.Max() < tags.Num() || (tags.Num() && !tags.GetDataPtr()))
            {
                output.InteriorStatus = Status::Invalid;
                return;
            }
            for (const auto tag : tags)
                if (tag.Number == 0 && MiniMapNative::Get()->asset.nameEqualsWide(tag, L"Interior"))
                    output.InteriorOnly = true;
        }

        /** Copies soft-reference arguments before invoking either engine conversion. */
        std::shared_ptr<const Facts::Definition> Copy(const DefinitionOwner& definition) const
        {
            auto result = std::make_shared<Facts::Definition>();
            const auto& placement = *definition.Root;
            result->ObjectIndex = placement.Index;
            result->BuildingId = static_cast<std::uint32_t>(placement.BuildingID);
            CopyInterior(placement, *result);
            SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString classParameters{};
            SDK::Params::KismetSystemLibrary_Conv_SoftObjectReferenceToString configParameters{};
            classParameters.SoftClassReference = placement.ActorClass;
            static_cast<SDK::FSoftObjectPtr&>(configParameters.SoftObjectReference) = placement.EntityType.EntityConfig;
            result->ClassStatus = Convert(ClassConversion, classParameters, result->CanonicalClassPath);
            result->ConfigReferenceStatus = Convert(ObjectConversion, configParameters, result->EntityConfigPath);
            return result;
        }
    };

    /** Releases a retained placement when the cache and in-flight local owners release it. */
    BuildingDefinitionAcquisition::DefinitionOwner::~DefinitionOwner()
    {
        const auto* native = MiniMapNative::Get();
        assert(!Root || native);
        if (Root && native)
            native->asset.releaseObject(Root);
    }

    /** Creates empty, instance-local metadata ownership. */
    BuildingDefinitionAcquisition::BuildingDefinitionAcquisition() = default;

    /** Enforces explicit shutdown rather than initiating a native lifecycle here. */
    BuildingDefinitionAcquisition::~BuildingDefinitionAcquisition()
    {
        assert(!metadata_);
    }

    /** Builds a temporary root owner and commits it only after successful initialization. */
    bool BuildingDefinitionAcquisition::Initialize()
    {
        if (metadata_)
            return true;
        const auto* native = MiniMapNative::Get();
        auto* objects = Services::TryObjects();
        if (!native || !native->player.isGameThread() || !objects || !objects->Ready())
            return false;
        auto candidate = std::make_shared<Metadata>();
        if (!candidate->Initialize())
            return false;
        metadata_ = std::move(candidate);
        return true;
    }

    /** Withdraws metadata; any synchronous Copy retains its own root owner until return. */
    void BuildingDefinitionAcquisition::Shutdown()
    {
        metadata_.reset();
    }

    /** Retains one placement without inserting a second definition cache. */
    std::shared_ptr<BuildingDefinitionAcquisition::DefinitionOwner>
        BuildingDefinitionAcquisition::Retain(SDK::UAuActorPlacementData* placement) const
    {
        auto owner = std::make_shared<DefinitionOwner>();
        MiniMapNative::Get()->asset.retainObject(placement);
        owner->Root = placement;
        return owner;
    }

    /** Copies through a locally retained metadata owner, reusing equivalent immutable facts. */
    std::shared_ptr<const Facts::Definition> BuildingDefinitionAcquisition::Copy(
        const std::shared_ptr<DefinitionOwner>& definition) const
    {
        const auto metadata = metadata_;
        if (!metadata)
            return {};
        auto facts = metadata->Copy(*definition);
        if (definition->Facts && *definition->Facts == *facts)
            return definition->Facts;
        return facts;
    }

    /** Copies definition-cached spline alternatives into immutable pointer-free facts. */
    std::shared_ptr<const Facts::Definition> BuildingDefinitionAcquisition::CopySplineProfiles(
        const std::shared_ptr<DefinitionOwner>& definition) const
    {
        const auto metadata = metadata_;
        if (!metadata || !definition || !definition->Facts)
            return {};
        const auto [status, profiles] = metadata->CopyProfiles(*definition);
        auto facts = std::make_shared<Facts::Definition>(*definition->Facts);
        facts->SplineProfileStatus = status;
        facts->SplineProfiles = profiles;
        if (*definition->Facts == *facts)
            return definition->Facts;
        return facts;
    }

    /** An absent optional config reference is known; missing canonical class remains pending. */
    bool BuildingDefinitionAcquisition::IsComplete(const Facts::Definition& facts) noexcept
    {
        return facts.ClassStatus == Status::Ready && facts.InteriorStatus == Status::Ready &&
            (facts.ConfigReferenceStatus == Status::Ready || facts.ConfigReferenceStatus == Status::Missing);
    }
}
#endif
