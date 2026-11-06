// Copies rooted authored definition values with per-instance reflection metadata.
// No legacy identity globals, asset loading, profiles or rendering policy are used.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingDefinitionAcquisition.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/Chimera_classes.hpp>
#include <SDK/Engine_parameters.hpp>
#include <Windows.h>
#include <cassert>
#include <utility>

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
            if (!libraryType || !functionType || !BuildingData || !libraryType->ClassDefaultObject)
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
                        Services::LogMessage(Services::Level::Error, "V2 soft-reference string release failed");
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

    /** An absent optional config reference is known; missing canonical class remains pending. */
    bool BuildingDefinitionAcquisition::IsComplete(const Facts::Definition& facts) noexcept
    {
        return facts.ClassStatus == Status::Ready && facts.InteriorStatus == Status::Ready &&
            (facts.ConfigReferenceStatus == Status::Ready || facts.ConfigReferenceStatus == Status::Missing);
    }
}
#endif
