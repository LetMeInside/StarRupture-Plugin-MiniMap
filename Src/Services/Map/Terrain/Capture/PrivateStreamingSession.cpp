// Orchestrates one retained private region using typed SDK ownership and isolated genuine ABI bridges.
// Submission is internal and explicit; this file adds no automatic request, scheduler or capture work.
#if defined(MODLOADER_CLIENT_BUILD)
#include "PrivateStreamingSession.h"
#include "PackageLoadOperation.h"
#include "WorldPartitionAdmissionScope.h"
#include "WorldPartitionUnregistration.h"
#include "Native/NativeApi.h"
#include "Services/Log.h"
#include "SDK/Engine_classes.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace Services::Map::Terrain::Capture
{

    /** Owns the private manager's native return array; instances remain owned by the retained world graph. */
    struct PrivateStreamingSession::DataLayerList final
    {
        SDK::TArray<SDK::UDataLayerInstance*> Values;
        /** Uses the private manager's native enumeration without constructing SDK-owned native containers. */
        explicit DataLayerList(const SDK::UDataLayerManager* manager)
            : Values(manager->GetDataLayerInstances()) {}
        /** Balances the native return allocation even when layer validation rejects a requirement. */
        ~DataLayerList()
        {
            MiniMapNative::Get()->texture.memoryFree(const_cast<SDK::UDataLayerInstance**>(Values.GetDataPtr()));
        }
    };

    /** Keeps construction inert, including hook and render-fence state. */
    PrivateStreamingSession::PrivateStreamingSession() = default;

    /** Never converts failure to successful retirement by dropping pointers. */
    PrivateStreamingSession::~PrivateStreamingSession()
    {
        if ((status_ != Status::Inactive && status_ != Status::Retired) || hook_.HasRetirementDebt())
            std::terminate();
    }

    /** Requires game-thread ownership and complete native dependencies outside collection/purge work. */
    bool PrivateStreamingSession::CanAdvance() const
    {
        const auto* api = MiniMapNative::Get();
        return api && api->captureAdmission.IsComplete() && api->packageLoad.IsComplete() &&
            api->player.isGameThread() && !api->world.isGarbageCollectingAndLocking() &&
            !api->world.isIncrementalPurgePending() && !api->world.isIncrementalReachabilityPending() &&
            api->asset.exitPurge && !*api->asset.exitPurge && api->texture.memoryFree;
    }

    /** Rejects vacuous evidence, reused private package identities and invalid regional requirements. */
    bool PrivateStreamingSession::IsDefinitionValid(const PrivateStreamingDefinition& definition)
    {
        if (!definition.SourcePackage.IsValid() || !definition.DestinationPackage.IsValid() ||
            definition.SourcePackage == definition.DestinationPackage || definition.RequiredCells.empty() ||
            definition.TargetGrids.empty() || !std::isfinite(definition.Radius) || definition.Radius <= 0)
            return false;
        for (double position : definition.SourcePosition)
        {
            if (!std::isfinite(position))
                return false;
        }
        for (const auto& grid : definition.TargetGrids)
        {
            if (!grid.IsValid())
                return false;
        }
        for (std::size_t index = 0; index < definition.DataLayers.size(); ++index)
        {
            const auto& layer = definition.DataLayers[index];
            if (!layer.AssetPackage.IsValid() || !layer.AssetName.IsValid() || layer.State > 2)
                return false;
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (definition.DataLayers[previous].AssetPackage == layer.AssetPackage &&
                    definition.DataLayers[previous].AssetName == layer.AssetName)
                    return false;
            }
        }
        for (std::size_t index = 0; index < definition.RequiredCells.size(); ++index)
        {
            const auto& cell = definition.RequiredCells[index];
            if (!cell.SourcePackage.IsValid() || !cell.PrivatePackage.IsValid() ||
                cell.SourcePackage == cell.PrivatePackage)
                return false;
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (definition.RequiredCells[previous].SourcePackage == cell.SourcePackage ||
                    definition.RequiredCells[previous].PrivatePackage == cell.PrivatePackage)
                    return false;
            }
        }
        return true;
    }

    /** Publishes ownership before any subsequent native call can remove the object from its world graph. */
    void PrivateStreamingSession::Retain(SDK::UObject* object)
    {
        if (object && std::find(owners_.begin(), owners_.end(), object) == owners_.end())
        {
            owners_.push_back(object);
            MiniMapNative::Get()->asset.retainObject(object);
        }
    }

    /** Uses the existing SDK lookup rather than writing a native package table or reusing an unrelated package. */
    bool PrivateStreamingSession::IsDestinationUnused(const StreamingName& name)
    {
        return !SDK::UObject::FindObjectFast(SDK::FName(name.ComparisonIndex, name.Number).ToString());
    }

    /** Installs the one scoped-admission hook, then submits the genuine asynchronous package instance request. */
    bool PrivateStreamingSession::Submit(PrivateStreamingDefinition definition, SDK::UWorld* gameplayWorld,
        std::uint64_t generation, const ITerrainDataProvider::CaptureDefinition& residencyDefinition,
        MiniMap::Services::INativeHooks& hooks)
    {
        if (status_ != Status::Inactive || !CanAdvance() || !gameplayWorld || !generation ||
            !IsDefinitionValid(definition) || !IsDestinationUnused(definition.DestinationPackage))
            return false;
        for (const auto& cell : definition.RequiredCells)
        {
            if (!IsDestinationUnused(cell.PrivatePackage))
                return false;
        }
        auto originalWorldPackage = residencyDefinition.OriginalWorldPackage;
        auto cookedCellPackages = residencyDefinition.CookedCellPackages;
        auto targetGridNames = residencyDefinition.TargetGrids;
        auto activatedDataLayerAssets = residencyDefinition.ActivatedDataLayerAssets;
        const auto streamingBounds = residencyDefinition.StreamingBounds;
        if (!hook_.Install(hooks))
            return false;
        definition_ = std::move(definition);
        originalWorldPackage_ = std::move(originalWorldPackage);
        cookedCellPackages_ = std::move(cookedCellPackages);
        targetGridNames_ = std::move(targetGridNames);
        activatedDataLayerAssets_ = std::move(activatedDataLayerAssets);
        streamingBounds_ = streamingBounds;
        gameplayWorld_ = gameplayWorld;
        generation_ = generation;
        status_ = Status::LoadingRoot;
        return SubmitRoot();
    }

    /** Compares exact stable residency metadata while ignoring output and generated private identities. */
    bool PrivateStreamingSession::HasCompatibleResidency(
        const ITerrainDataProvider::CaptureDefinition& definition) const noexcept
    {
        const auto& left = streamingBounds_;
        const auto& right = definition.StreamingBounds;
        return originalWorldPackage_ == definition.OriginalWorldPackage &&
            cookedCellPackages_ == definition.CookedCellPackages && targetGridNames_ == definition.TargetGrids &&
            activatedDataLayerAssets_ == definition.ActivatedDataLayerAssets &&
            left.Minimum.X == right.Minimum.X && left.Minimum.Y == right.Minimum.Y &&
            left.Minimum.Z == right.Minimum.Z && left.Maximum.X == right.Maximum.X &&
            left.Maximum.Y == right.Maximum.Y && left.Maximum.Z == right.Maximum.Z;
    }

    /** Requires current ready residency and a debt-free slot before creating a fresh native capture owner. */
    bool PrivateStreamingSession::RequestCapture(
        const ITerrainDataProvider::CaptureDefinition& definition, std::uint64_t generation,
        bool retainDiagnosticPixels)
    {
        if (advancing_ || retirementRequested_ || generation != generation_ || status_ != Status::Ready ||
            capture_ || completedImage_ || !CanAdvance() || !HasCompatibleResidency(definition) || !HasRequiredCells())
            return false;
        capture_ = std::make_unique<TerrainImageCapture>();
        advancing_ = true;
        bool accepted = false;
        try
        {
            accepted = capture_->Submit(world_, definition, retainDiagnosticPixels);
        }
        catch (...)
        {
            advancing_ = false;
            if (capture_->GetStatus() == TerrainImageCapture::Status::Inactive)
                capture_.reset();
            throw;
        }
        advancing_ = false;
        if (!accepted && capture_->GetStatus() == TerrainImageCapture::Status::Inactive)
            capture_.reset();
        else if (!accepted)
        {
            captureResultStatus_ = CaptureResultStatus::Failed;
            captureResultReason_ = capture_->GetReason();
            if (captureResultReason_.empty())
                captureResultReason_ = "Capture submission failed after creating native retirement debt.";
            capture_->RequestRetirement();
        }
        if (accepted)
        {
            captureResultStatus_ = CaptureResultStatus::Pending;
            captureResultReason_.clear();
        }
        return accepted;
    }

    /** Transfers immutable output only before session retirement closes image issuance. */
    std::shared_ptr<const TerrainImage> PrivateStreamingSession::TakeCompletedImage()
    {
        if (retirementRequested_)
            return nullptr;
        auto image = std::exchange(completedImage_, nullptr);
        if (image)
            captureResultStatus_ = CaptureResultStatus::Completed;
        return image;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Moves short-lived raw RGB samples to the current TerrainService consumer. */
    TerrainCaptureDiagnosticPixels PrivateStreamingSession::TakeCompletedDiagnosticPixels() noexcept
    {
        auto pixels = std::move(completedDiagnosticPixels_);
        completedDiagnosticPixels_ = {};
        return pixels;
    }
#endif

    /** Advances one capture operation without progressing private residency while the world is borrowed. */
    void PrivateStreamingSession::AdvanceCapture()
    {
        if (!capture_)
            return;
        if (retirementRequested_)
            capture_->RequestRetirement();
        capture_->Advance();
        if (!retirementRequested_ && capture_->GetStatus() == TerrainImageCapture::Status::Ready)
        {
            completedImage_ = capture_->GetImage();
            if (completedImage_)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                completedDiagnosticPixels_ = capture_->TakeDiagnosticPixels();
#endif
                captureResultStatus_ = CaptureResultStatus::ImageAvailable;
            }
            else
            {
                captureResultStatus_ = CaptureResultStatus::Failed;
                captureResultReason_ = "Capture reached Ready without an immutable image.";
            }
            capture_->RequestRetirement();
        }
        if (capture_->GetStatus() == TerrainImageCapture::Status::Retired)
        {
            if (captureResultStatus_ == CaptureResultStatus::Pending)
            {
                captureResultStatus_ = CaptureResultStatus::Failed;
                captureResultReason_ = capture_->GetReason().empty()
                    ? "Capture retired before producing an image."
                    : capture_->GetReason();
            }
            capture_.reset();
        }
    }

    /** Requests asynchronous capture retirement and reports whether the operation released all native debt. */
    bool PrivateStreamingSession::RetireCapture()
    {
        if (!capture_)
            return true;
        capture_->RequestRetirement();
        if (capture_->GetStatus() != TerrainImageCapture::Status::Retired)
            return false;
        if (captureResultStatus_ == CaptureResultStatus::Pending)
        {
            captureResultStatus_ = CaptureResultStatus::Failed;
            captureResultReason_ = capture_->GetReason().empty()
                ? "Capture retired before producing an image."
                : capture_->GetReason();
        }
        capture_.reset();
        return true;
    }

    /** Uses the existing synchronous retirement only at safe lifecycle or explicit shutdown boundaries. */
    bool PrivateStreamingSession::RetireCaptureSynchronously()
    {
        if (!capture_)
            return true;
        if (advancing_ || !CanAdvance())
            return false;
        capture_->RequestRetirement();
        advancing_ = true;
        bool retired = false;
        try
        {
            retired = capture_->RetireSynchronously();
        }
        catch (...)
        {
            advancing_ = false;
            throw;
        }
        advancing_ = false;
        if (!retired)
        {
            reason_ = "Synchronous terrain capture retirement failed; capture ownership remains retained.";
            return false;
        }
        if (captureResultStatus_ == CaptureResultStatus::Pending)
        {
            captureResultStatus_ = CaptureResultStatus::Failed;
            captureResultReason_ = capture_->GetReason().empty()
                ? "Capture retired before producing an image."
                : capture_->GetReason();
        }
        capture_.reset();
        return true;
    }

    /** Defers logical cancellation through a synchronous loader reentry without changing native queues. */
    bool PrivateStreamingSession::SubmitRoot()
    {
        advancing_ = true;
        bool accepted = false;
        try
        {
            rootLoad_ = std::make_unique<PackageLoadOperation>();
            accepted = rootLoad_->Submit(definition_.SourcePackage.ComparisonIndex, definition_.SourcePackage.Number,
                definition_.DestinationPackage.ComparisonIndex, definition_.DestinationPackage.Number);
        }
        catch (...)
        {
            advancing_ = false;
            Fail("Native root submission threw; ownership remains retained.");
            throw;
        }
        advancing_ = false;
        if (!accepted)
        {
            Fail("Native root-package submission rejected; native ownership still requires retirement.");
            return false;
        }
        if (status_ == Status::Retiring)
            rootLoad_->Cancel();
        return status_ != Status::Retiring;
    }

    /** Consumes only a package whose callback and genuine delegate copies have all retired. */
    bool PrivateStreamingSession::AdoptRoot()
    {
        const auto* api = MiniMapNative::Get();
        auto* package = rootLoad_->GetRetainedPackage();
        if (!package || package->Name != SDK::FName(definition_.DestinationPackage.ComparisonIndex,
            definition_.DestinationPackage.Number))
            return false;
        auto* candidate = api->captureAdmission.findWorldInPackage(package);
        if (!candidate || candidate != rootLoad_->GetRetainedWorld() || candidate == gameplayWorld_ ||
            candidate->Outer != package ||
            !api->captureAdmission.isGameWorld(candidate) || !api->captureAdmission.isPreviewWorld(candidate) ||
            candidate->NetDriver || candidate->DemoNetDriver || candidate->OwningGameInstance)
            return false;
        auto* level = candidate->PersistentLevel;
        if (!level || level->Outer != candidate || level->OwningWorld != candidate || !level->WorldSettings ||
            level->WorldSettings->Outer != level || !level->WorldSettings->WorldPartition)
            return false;
        auto* partition = level->WorldSettings->WorldPartition;
        if (partition->Outer != level->WorldSettings || !partition->RuntimeHash ||
            partition->RuntimeHash->Outer != partition || api->captureAdmission.isPartitionInitialized(partition))
            return false;
        package_ = rootLoad_->TransferRetainedPackage(owners_);
        if (package_ != package)
            return false;
        world_ = candidate;
        partition_ = partition;
        Retain(candidate);
        Retain(level);
        Retain(level->WorldSettings);
        Retain(partition);
        Retain(partition->RuntimeHash);
        return true;
    }

    /** Applies private per-partition modes before native caches are populated; initializes the cooked graph once. */
    bool PrivateStreamingSession::InitializeWorld()
    {
        const auto* api = MiniMapNative::Get();
        if (!partition_->bEnableStreaming)
            return false;
        partition_->ServerStreamingMode = SDK::EWorldPartitionServerStreamingMode::Enabled;
        partition_->ServerStreamingOutMode = SDK::EWorldPartitionServerStreamingOutMode::Enabled;
        {
            WorldPartitionAdmissionScope scope(world_, package_, generation_, gameplayWorld_);
            if (!scope.IsOpen())
                return false;
            initialized_ = true;
            unregistered_ = false;
            api->world.initWorld(reinterpret_cast<std::uintptr_t>(world_), MiniMapNative::WorldInitializationValues{});
        }
        if (!levels_.Observe(world_, owners_))
            return false;
        if (!api->captureAdmission.isGameWorld(world_) || !api->captureAdmission.isPreviewWorld(world_) ||
            world_->NetDriver || world_->DemoNetDriver || world_->OwningGameInstance ||
            world_->Outer != package_ || world_->PersistentLevel->OwningWorld != world_ ||
            world_->PersistentLevel->WorldSettings->WorldPartition != partition_)
            return false;
        subsystem_ = static_cast<SDK::UWorldPartitionSubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
            world_, SDK::UWorldPartitionSubsystem::StaticClass()));
        Retain(subsystem_);
        if (!subsystem_ || subsystem_->Class != SDK::UWorldPartitionSubsystem::StaticClass() ||
            subsystem_->Outer != world_ || !api->captureAdmission.isPartitionInitialized(partition_) ||
            !partition_->StreamingPolicy || partition_->StreamingPolicy->Outer != partition_ ||
            !api->captureAdmission.canStream(partition_))
            return false;
        Retain(partition_->StreamingPolicy);
        const bool serverStreaming = api->captureAdmission.serverStreamingEnabled(partition_);
        const bool serverStreamingOut = api->captureAdmission.serverStreamingOutEnabled(partition_);
        // Require effective regional modes without relying on how a particular build classifies standalone.
        if (!serverStreaming || !serverStreamingOut)
            return false;
        api->world.updateWorldComponents(reinterpret_cast<std::uintptr_t>(world_), false, false, nullptr);
        return ConfigureDataLayers();
    }

    /** Keeps content-specific names external and observes the effective state after normal native state changes. */
    bool PrivateStreamingSession::ConfigureDataLayers()
    {
        auto* manager = partition_->DataLayerManager;
        if (!manager || manager->Outer != partition_)
            return false;
        Retain(manager);
        for (const auto& requirement : definition_.DataLayers)
        {
            const auto* layer = ResolveDataLayer(requirement);
            if (!layer || !layer->IsRuntime() || !layer->Outer ||
                !layer->Outer->IsA(SDK::AWorldDataLayers::StaticClass()) ||
                layer->Outer->Outer != world_->PersistentLevel)
                return false;
            Retain(const_cast<SDK::UDataLayerInstance*>(layer));
            dataLayers_.push_back(layer);
            const auto state = static_cast<SDK::EDataLayerRuntimeState>(requirement.State);
            manager->SetDataLayerInstanceRuntimeState(layer, state, false);
            if (manager->GetDataLayerInstanceEffectiveRuntimeState(layer) != state)
                return false;
        }
        return true;
    }

    /** Matches original package/object asset identity, then asks only the private manager for its instance. */
    const SDK::UDataLayerInstance* PrivateStreamingSession::ResolveDataLayer(const StreamingDataLayer& requirement) const
    {
        auto* manager = partition_->DataLayerManager;
        DataLayerList layers(manager);
        for (const auto* layer : layers.Values)
        {
            if (!layer)
                continue;
            const auto* asset = layer->GetAsset();
            if (asset && asset->Name == SDK::FName(requirement.AssetName.ComparisonIndex, requirement.AssetName.Number) &&
                asset->Outer && asset->Outer->IsA(SDK::UPackage::StaticClass()) &&
                asset->Outer->Name == SDK::FName(requirement.AssetPackage.ComparisonIndex, requirement.AssetPackage.Number) &&
                manager->GetDataLayerInstanceFromAsset(asset) == layer)
                return layer;
        }
        return nullptr;
    }

    /** Creates only generic engine actors/components; the private world never enters gameplay. */
    bool PrivateStreamingSession::CreateSource()
    {
        const auto* api = MiniMapNative::Get();
        if (!api || !api->asset.spawnObject || !api->asset.retainObject ||
            !api->player.registerComponent || !api->player.unregisterComponent)
        {
            Fail("Required native construction, retention or registration bindings are unavailable for the private streaming source.");
            return false;
        }
        SDK::FTransform identity{};
        identity.Rotation.W = 1;
        identity.Scale3D = SDK::FVector(1, 1, 1);
        auto* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world_, SDK::AActor::StaticClass(),
            identity, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr,
            SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
        if (!actor)
        {
            Fail("BeginDeferredActorSpawnFromClass returned null for the private streaming actor.");
            return false;
        }
        Retain(actor);
        if (SDK::UGameplayStatics::FinishSpawningActor(actor, identity,
            SDK::ESpawnActorScaleMethod::MultiplyWithRoot) != actor)
        {
            Fail("FinishSpawningActor did not return the private streaming actor.");
            return false;
        }
        return CreateSourceRoot(actor) && ConfigureSource(actor);
    }

    /** Uses direct construction because actor reflection requires gameplay actor initialization. */
    bool PrivateStreamingSession::CreateSourceRoot(SDK::AActor* actor)
    {
        if (actor->RootComponent || !world_->PersistentLevel || actor->Outer != world_->PersistentLevel ||
            world_->PersistentLevel->OwningWorld != world_ || !PrivateWorldNative::CanCreateComponentInWorld(world_))
        {
            Fail("The private streaming actor does not have an empty root in the live private persistent level.");
            return false;
        }
        auto* rootClass = SDK::USceneComponent::StaticClass();
        if (!rootClass || rootClass->GetFullName() != "Class Engine.SceneComponent")
        {
            Fail("The SDK did not resolve the Engine.SceneComponent class for the private streaming actor.");
            return false;
        }
        const auto* api = MiniMapNative::Get();
        SDK::TSubclassOf<SDK::UObject> objectClass(rootClass);
        auto* root = static_cast<SDK::USceneComponent*>(api->asset.spawnObject(&objectClass, actor));
        Retain(root);
        if (!root)
        {
            Fail("Native object construction returned null for the private streaming actor root.");
            return false;
        }
        if (root->GetOwner() != actor)
        {
            Fail("The new root component is not owned by the private streaming actor.");
            return false;
        }
        actor->RootComponent = root;
        api->player.registerComponent(root, world_, nullptr);
        if (actor->RootComponent != root || !PrivateWorldNative::IsComponentRegistered(root))
        {
            Fail("The private streaming actor root was not assigned and registered.");
            return false;
        }
        SDK::FTransform placement{};
        placement.Rotation.W = 1;
        placement.Scale3D = SDK::FVector(1, 1, 1);
        placement.Translation = SDK::FVector(definition_.SourcePosition[0], definition_.SourcePosition[1],
            definition_.SourcePosition[2]);
        root->K2_SetWorldTransform(placement, false, nullptr, true);
        if (!(root->K2_GetComponentLocation() == placement.Translation))
        {
            Fail("The registered root did not reach the private streaming source position.");
            return false;
        }
        return true;
    }

    /** Constructs an actor-owned source and configures its native arrays before provider registration. */
    bool PrivateStreamingSession::ConfigureSource(SDK::AActor* actor)
    {
        if (!world_->PersistentLevel || actor->Outer != world_->PersistentLevel ||
            world_->PersistentLevel->OwningWorld != world_ || !PrivateWorldNative::CanCreateComponentInWorld(world_))
        {
            Fail("The private streaming actor is not in the live private world's PersistentLevel.");
            return false;
        }
        auto* sourceClass = SDK::UWorldPartitionStreamingSourceComponent::StaticClass();
        if (!sourceClass || sourceClass->GetFullName() != "Class Engine.WorldPartitionStreamingSourceComponent")
        {
            Fail("The SDK did not resolve the Engine.WorldPartitionStreamingSourceComponent class.");
            return false;
        }
        const auto* api = MiniMapNative::Get();
        SDK::TSubclassOf<SDK::UObject> objectClass(sourceClass);
        auto* source = static_cast<SDK::UWorldPartitionStreamingSourceComponent*>(api->asset.spawnObject(&objectClass, actor));
        Retain(source);
        if (!source)
        {
            Fail("Native object construction returned null for the private streaming source component.");
            return false;
        }
        if (source->Outer != actor || source->GetOwner() != actor)
        {
            Fail("The streaming source component is not owned by its private actor.");
            return false;
        }
        source_ = source;
        source_->bStreamingSourceEnabled = false;
        source_->TargetState = SDK::EStreamingSourceTargetState::Activated;
        source_->TargetBehavior = SDK::EStreamingSourceTargetBehavior::Include;
        const char* configurationFailure = nullptr;
        if (!PrivateWorldNative::ConfigureSource(source_, definition_, &configurationFailure))
        {
            Fail(configurationFailure ? configurationFailure :
                "PrivateWorldNative::ConfigureSource failed without a detailed reason.");
            return false;
        }
        api->player.registerComponent(source_, world_, nullptr);
        if (!PrivateWorldNative::IsComponentRegistered(source_))
        {
            Fail("Native registration did not register the private streaming source component.");
            return false;
        }
        source_->EnableStreamingSource();
        if (source_->IsStreamingSourceEnabled())
            return true;
        Fail("EnableStreamingSource returned without enabling the private source.");
        return false;
    }

    /** Preserves asynchronous acquisition; only a later observed callback-destruction boundary permits adoption. */
    void PrivateStreamingSession::AdvanceRoot()
    {
        if (!rootLoad_->PollNativeRetirement())
            return;
        if (rootLoad_->GetResult() != PackageLoadOperation::Result::Succeeded || !AdoptRoot())
        {
            Fail("Native root load or independent package/world graph validation failed.");
            return;
        }
        if (status_ == Status::Retiring)
            return;
        if (!InitializeWorld())
        {
            Fail("Private initialization, effective modes or Data Layer validation failed.");
            return;
        }
        if (status_ == Status::Retiring)
            return;
        auto* hlodClass = SDK::UWorldPartitionHLODRuntimeSubsystem::StaticClass();
        auto* hlodSubsystem = hlodClass ? SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world_, hlodClass) : nullptr;
        if (!hlodSubsystem || hlodSubsystem->Class != hlodClass || hlodSubsystem->Outer != world_)
        {
            Fail("The private world did not initialize its World Partition HLOD runtime subsystem.");
            return;
        }
        Retain(hlodSubsystem);
        if (!CreateSource())
            return;
        if (status_ != Status::Retiring)
            status_ = Status::Streaming;
    }

    /** Proves required package/level ownership and completed native registration without diagnostic actors. */
    bool PrivateStreamingSession::HasCellEvidence(const RequiredStreamingCell& cell,
        const SDK::ULevelStreaming* stream) const
    {
        auto* level = stream->LoadedLevel;
        if (!level || level->OwningWorld != world_ || !PrivateWorldNative::IsLevelReady(level))
            return false;
        if (!level->Outer || !level->Outer->IsA(SDK::UWorld::StaticClass()))
            return false;
        auto* cellWorld = static_cast<SDK::UWorld*>(level->Outer);
        if (cellWorld == world_ ||
            !cellWorld->Outer || cellWorld->Outer->Name != SDK::FName(cell.PrivatePackage.ComparisonIndex,
                cell.PrivatePackage.Number) || cellWorld->PersistentLevel != level ||
            !MiniMapNative::Get()->captureAdmission.isPreviewWorld(cellWorld) ||
            MiniMapNative::Get()->captureAdmission.findWorldInPackage(
                static_cast<SDK::UPackage*>(cellWorld->Outer)) != cellWorld)
            return false;
        return true;
    }

    /** Requires every explicit cell to be currently loaded and activated; an empty completion result cannot pass. */
    bool PrivateStreamingSession::HasRequiredCells() const
    {
        if (definition_.RequiredCells.empty() || !source_ || !source_->IsStreamingCompleted() ||
            MiniMapNative::Get()->captureAdmission.visibilityPending(world_) ||
            MiniMapNative::Get()->captureAdmission.hasEndOfFrameUpdates(world_) || !levels_.GetRequestIds().empty())
            return false;
        if (dataLayers_.size() != definition_.DataLayers.size())
            return false;
        for (std::size_t index = 0; index < dataLayers_.size(); ++index)
        {
            const auto* layer = dataLayers_[index];
            if (!layer || !layer->GetAsset() ||
                partition_->DataLayerManager->GetDataLayerInstanceFromAsset(layer->GetAsset()) != layer ||
                partition_->DataLayerManager->GetDataLayerInstanceEffectiveRuntimeState(layer) !=
                static_cast<SDK::EDataLayerRuntimeState>(definition_.DataLayers[index].State))
                return false;
        }
        for (const auto& cell : definition_.RequiredCells)
        {
            bool found = false;
            for (auto* stream : world_->StreamingLevels)
            {
                if (stream && stream->Outer == world_ && stream->PackageNameToLoad ==
                    SDK::FName(cell.SourcePackage.ComparisonIndex, cell.SourcePackage.Number) &&
                    stream->IsLevelLoaded() && stream->IsLevelVisible() &&
                    !stream->IsStreamingStatePending() && HasCellEvidence(cell, stream))
                    found = true;
            }
            if (!found)
                return false;
        }
        return true;
    }

    /** Services only the private partition policy and private level streaming, with retained snapshots on both sides. */
    void PrivateStreamingSession::AdvanceStreaming()
    {
        if (!levels_.Observe(world_, owners_))
        {
            Fail("Foreign private streaming-level ownership observed.");
            return;
        }
        PrivateWorldNative::AdvancePartition(subsystem_);
        if (!levels_.Observe(world_, owners_))
        {
            Fail("Foreign streaming-level ownership after policy update.");
            return;
        }
        if (status_ == Status::Retiring)
            return;
        PrivateWorldNative::AdvanceLevels(world_);
        if (!levels_.Observe(world_, owners_))
        {
            Fail("Foreign cell-level ownership after level update.");
            return;
        }
        if (status_ == Status::Retiring)
            return;
        // This world is not engine-ticked: service deferred component work before testing render-state readiness.
        MiniMapNative::Get()->captureAdmission.sendEndOfFrame(world_);
        if (status_ != Status::Retiring && HasRequiredCells())
        {
            native_.BeginFence();
            if (status_ != Status::Retiring)
                status_ = Status::SubmittingDeferredWork;
        }
    }

    /** Serializes engine progression and leaves all ownership intact if an exception interrupts it. */
    void PrivateStreamingSession::Advance()
    {
        if (status_ == Status::Inactive || status_ == Status::Retired || advancing_ || !CanAdvance())
            return;
        advancing_ = true;
        try
        {
            if (capture_)
                AdvanceCapture();
            else if (status_ == Status::Retiring)
                AdvanceRetirement();
            else if (status_ == Status::LoadingRoot)
                AdvanceRoot();
            else if (status_ == Status::Streaming)
                AdvanceStreaming();
            else if (status_ == Status::SubmittingDeferredWork && native_.PollFence())
            {
                const bool ready = HasRequiredCells();
                if (status_ != Status::Retiring)
                {
                    status_ = ready ? Status::Ready : Status::Streaming;
                    if (ready)
                    {
                        if (auto* log = MiniMap::Services::TryLog())
                            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                                "MiniMap: private terrain residency ready generation=%llu",
                                static_cast<unsigned long long>(generation_));
                    }
                }
            }
            else if (status_ == Status::Ready && !HasRequiredCells() && status_ != Status::Retiring)
                status_ = Status::Streaming;
        }
        catch (...)
        {
            advancing_ = false;
            Fail("Native private streaming advancement threw; ownership remains retained.");
            throw;
        }
        advancing_ = false;
        if (status_ == Status::Retiring && rootLoad_)
            rootLoad_->Cancel();
    }

    /** Separates logical cancellation from native unregistration, package completion and final cleanup. */
    void PrivateStreamingSession::RequestRetirement()
    {
        if (status_ == Status::Inactive || status_ == Status::Retired)
            return;
        retirementRequested_ = true;
        if (captureResultStatus_ == CaptureResultStatus::ImageAvailable)
            captureResultStatus_ = CaptureResultStatus::Completed;
        completedImage_.reset();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        completedDiagnosticPixels_ = {};
#endif
        RetireCapture();
        status_ = Status::Retiring;
        if (rootLoad_ && !advancing_)
            rootLoad_->Cancel();
    }

    /** Records failure without discarding native work or claiming readiness/retirement. */
    void PrivateStreamingSession::Fail(const char* reason)
    {
        reason_ = reason;
        RequestRetirement();
    }

    /** Retains the complete observed union before normal native uninitialization joins policy work and unregisters. */
    bool PrivateStreamingSession::CloseRegistration()
    {
        if (!RetireCapture())
            return false;
        if (unregistered_)
            return true;
        if (!world_ || WorldPartitionAdmissionScope::HasOpenScope() || !levels_.Observe(world_, owners_))
            return false;
        if (!WorldPartitionUnregistration::Unregister(world_, source_, owners_))
            return false;
        unregistered_ = true;
        if (!levels_.Observe(world_, owners_))
            return false;
        levels_.RequestUnload();
        return true;
    }

    /** Must complete in the existing before-EndPlay notification, before gameplay's partition unregisters. */
    bool PrivateStreamingSession::BeforeGameplayWorldEnd(SDK::UWorld* world)
    {
        if (world != gameplayWorld_ || status_ == Status::Inactive || status_ == Status::Retired)
            return true;
        RequestRetirement();
        if (advancing_)
            return unregistered_;
        if (capture_ && !RetireCaptureSynchronously())
            return false;
        if (unregistered_)
            return true;
        return CanAdvance() && CloseRegistration();
    }

    /** Keeps all owners through real package, visibility, component and RHI-submission acknowledgement boundaries. */
    void PrivateStreamingSession::AdvanceRetirement()
    {
        if (capture_)
        {
            AdvanceCapture();
            if (capture_)
                return;
        }
        if (rootLoad_ && !rootLoad_->PollNativeRetirement())
            return;
        if (!initialized_)
        {
            FinishRetirement();
            return;
        }
        if (!CloseRegistration() || !native_.PollFence())
            return;
        if (cleaned_)
        {
            FinishRetirement();
            return;
        }
        if (!levels_.Observe(world_, owners_))
            return;
        levels_.RequestUnload();
        PrivateWorldNative::AdvanceLevels(world_);
        if (!levels_.Observe(world_, owners_) || !levels_.IsUnloaded() ||
            MiniMapNative::Get()->captureAdmission.visibilityPending(world_))
            return;
        MiniMapNative::Get()->captureAdmission.sendEndOfFrame(world_);
        MiniMapNative::Get()->world.cleanupWorld(reinterpret_cast<std::uintptr_t>(world_), true, true, 0);
        cleaned_ = true;
        native_.BeginFence();
    }

    /** Stops the admission hook before releasing owners after all native requests and cleanup have retired. */
    void PrivateStreamingSession::FinishRetirement()
    {
        if (capture_ || native_.HasFence() || !unregistered_ || (rootLoad_ && !rootLoad_->IsNativeRetired()))
            return;
        if (!hook_.Stop())
            return;
        if (initialized_)
            levels_.ClearAfterRetirement();
        rootLoad_.reset();
        dataLayers_.clear();
        for (auto* owner : owners_)
            MiniMapNative::Get()->asset.releaseObject(owner);
        owners_.clear();
        world_ = nullptr;
        package_ = nullptr;
        partition_ = nullptr;
        subsystem_ = nullptr;
        source_ = nullptr;
        gameplayWorld_ = nullptr;
        initialized_ = false;
        status_ = Status::Retired;
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: private terrain world retirement acknowledged generation=%llu",
                static_cast<unsigned long long>(generation_));
    }

    /** Blocks only on actual owned package IDs; unresolved native work or hook debt prevents shutdown success. */
    bool PrivateStreamingSession::TryShutdown()
    {
        if (advancing_)
            return false;
        if ((status_ == Status::Inactive || status_ == Status::Retired) && !capture_)
            return hook_.Stop();
        if (!CanAdvance())
            return false;
        if (status_ != Status::Inactive && status_ != Status::Retired)
            RequestRetirement();
        if (!RetireCaptureSynchronously())
            return false;
        if (status_ == Status::Inactive || status_ == Status::Retired)
            return hook_.Stop();
        RequestRetirement();
        if (!DrainOwnedRequests())
            return false;
        Advance();
        if (status_ != Status::Retired)
        {
            if (!DrainOwnedRequests())
                return false;
            Advance();
        }
        return (status_ == Status::Inactive || status_ == Status::Retired) && hook_.Stop();
    }

    /** Waits only at explicit shutdown and leaves every native owner alive when completion is not acknowledged. */
    bool PrivateStreamingSession::DrainOwnedRequests()
    {
        if (capture_)
            return false;
        advancing_ = true;
        bool drained = false;
        try
        {
            drained = CloseRegistration() &&
                (!rootLoad_ || rootLoad_->WaitForNativeRetirement()) &&
                (!initialized_ || (levels_.Observe(world_, owners_) && levels_.WaitForRequests()));
            if (drained && initialized_ && !cleaned_)
            {
                levels_.RequestUnload();
                PrivateWorldNative::FlushVisibility(world_);
                drained = levels_.Observe(world_, owners_) && levels_.IsUnloaded() &&
                    !MiniMapNative::Get()->captureAdmission.visibilityPending(world_);
            }
            if (drained)
                drained = native_.WaitFence();
        }
        catch (...)
        {
            advancing_ = false;
            Fail("Native request-specific retirement wait threw; ownership remains retained.");
            throw;
        }
        advancing_ = false;
        return drained;
    }
}
#endif
