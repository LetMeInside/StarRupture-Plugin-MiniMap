#pragma once

// The names of the game functions MiniMap looks up, in the loader's version-map
// form: "Class::Function" in Dumper-7 naming, or the global's name. They are the
// RequiredScan names in Fingerprints.cpp and the keys the map features list
// (Adapters/AlienX/GameFields.h). A scan of kind Code is a site of MiniMap's own,
// which the loader reads as "MiniMap/<key>".

namespace MiniMapNative::Keys
{
    // The player's pose and the objects every layer reads
    inline constexpr const char* CommonUIActivated   = "UCommonActivatableWidget::NativeOnActivated";
    inline constexpr const char* CommonUIDeactivated = "UCommonActivatableWidget::NativeOnDeactivated";
    inline constexpr const char* FirstPlayerController = "UWorld::GetFirstPlayerController";
    inline constexpr const char* ComponentLocation   = "USceneComponent::K2_GetComponentLocation";
    inline constexpr const char* ControlRotation     = "AController::GetControlRotation";
    inline constexpr const char* InForgottenEngine   = "ACrCharacterPlayerBase::IsPlayerInForgottenEngine";
    inline constexpr const char* GameThread          = "IsInGameThread";
    inline constexpr const char* RetainObject        = "UObject::AddRef";
    inline constexpr const char* ReleaseObject       = "UObject::ReleaseRef";
    inline constexpr const char* NameEqualsWide      = "FName::EqualsWide";
    inline constexpr const char* ObjectArray         = "GUObjectArray (anchor)";

    // Textures and the terrain
    inline constexpr const char* LoadSynchronous     = "FSoftObjectPtr::LoadSynchronous";
    inline constexpr const char* BrushTexture        = "UWidgetBlueprintLibrary::GetBrushResourceAsTexture2D";
    inline constexpr const char* ForceMips           = "UStreamableRenderAsset::SetForceMipLevelsToBeResident";
    inline constexpr const char* WaitForStreaming    = "UStreamableRenderAsset::WaitForStreaming";
    inline constexpr const char* ResidentMips        = "UTexture2D::GetNumResidentMips";
    inline constexpr const char* AllowedMips         = "UTexture2D::GetNumMipsAllowed";
    inline constexpr const char* NumMips             = "UTexture2D::GetNumMips";
    inline constexpr const char* StreamIn            = "UTexture2D::StreamIn";
    inline constexpr const char* WaitForPendingInit  = "UStreamableRenderAsset::WaitForPendingInitOrStreaming";
    inline constexpr const char* PlatformData        = "UTexture2D::GetPlatformData";
    inline constexpr const char* BulkDataSize        = "FBulkData::GetBulkDataSize";
    inline constexpr const char* CanLoadFromDisk     = "FBulkData::CanLoadFromDisk";
    inline constexpr const char* BulkDataCopy        = "FBulkData::GetCopy";
    inline constexpr const char* MemoryFree          = "FMemory::Free";

    // Points of interest and foundables
    inline constexpr const char* GetAllActorsOfClass = "UGameplayStatics::GetAllActorsOfClass";
    inline constexpr const char* PoiStaticClass      = "ACrPointOfInterestMarkerActor::StaticClass";
    inline constexpr const char* PoiCategory         = "UCrMapMenuDevSettings::FindPOIMarkerCategoryData";
    inline constexpr const char* MarkerFilter        = "UCrPlayerMapMenuDataComponent::GetMapMenuMarkerFiltersOnOffStatus";
    inline constexpr const char* AbandonBaseCompleted = "FCrAbandonBaseData::IsCompleted";
    inline constexpr const char* FoundableCategory   = "UCrMapMenuDevSettings::FindFoundableMarkerCateroryData";

    // Building tracking across save loads
    inline constexpr const char* SaveLoaded          = "UCrMassSaveSubsystem::OnSaveLoaded";
    inline constexpr const char* PostSaveLoaded      = "UCrMassSaveSubsystem::OnPostSaveLoaded";
    inline constexpr const char* BuildingAdd         = "UCrBuildingStabilitySubsystem::OnCreateBuildingEntity";
    inline constexpr const char* BuildingRemove      = "UCrBuildingStabilitySubsystem::OnDestroyBuildingEntity";

    // The texture the building pictures are published through (UpdateResource is a
    // site of MiniMap's own, read by the loader as "MiniMap/UTexture::UpdateResource")
    inline constexpr const char* CreateTransient     = "UTexture2D::CreateTransient";
    inline constexpr const char* BulkLock            = "FBulkData::Lock";
    inline constexpr const char* BulkUnlock          = "FBulkData::Unlock";
    inline constexpr const char* UpdateResource      = "UTexture::UpdateResource";
    inline constexpr const char* GetResource         = "UTexture::GetResource";

    // Mass entity queries
    inline constexpr const char* MassSubsystem       = "UWorld::GetSubsystem<UMassEntitySubsystem>";
    inline constexpr const char* QueryConstruct      = "FMassEntityQuery::FMassEntityQuery";
    inline constexpr const char* QueryDestruct       = "FMassEntityQuery::~FMassEntityQuery";
    inline constexpr const char* MatchingHandles     = "FMassEntityQuery::GetMatchingEntityHandles";
    inline constexpr const char* AddInventoryRequirement = "FMassFragmentRequirements::AddRequirement<FCrInventoryFragment>";
    inline constexpr const char* AddTransformRequirement = "FMassFragmentRequirements::AddRequirement<FTransformFragment>";
    inline constexpr const char* AddEnemyStateRequirement = "FMassFragmentRequirements::AddRequirement<FMassEnemyStateFragment>";
    inline constexpr const char* AddFoundableParameters = "FMassFragmentRequirements::AddConstSharedRequirement<FCrMassFoundableParameters>";
    inline constexpr const char* AddFoundableTag     = "FMassFragmentRequirements::AddTagRequirement<FCrMassFoundableTag>";
    inline constexpr const char* AddTagRequirement   = "FMassFragmentRequirements::AddTagRequirement";
    inline constexpr const char* FragmentDataPtr     = "FMassEntityManager::InternalGetFragmentDataPtr";
    inline constexpr const char* ConstSharedPtr      = "FMassEntityManager::InternalGetConstSharedFragmentPtr";
    inline constexpr const char* IsEntityBuilt       = "FMassEntityManager::IsEntityBuilt";
    inline constexpr const char* IsEntityValid       = "FMassEntityManager::IsEntityValid";
    inline constexpr const char* TransformStaticStruct = "FTransformFragment::StaticStruct";
    inline constexpr const char* InventoryStaticStruct = "FCrInventoryFragment::StaticStruct";
    inline constexpr const char* FoundableStaticStruct = "FCrMassFoundableParameters::StaticStruct";
    inline constexpr const char* EnemyStateStaticStruct = "FMassEnemyStateFragment::StaticStruct";
    inline constexpr const char* EnemyTagStaticStruct = "FMassEnemyTag::StaticStruct";
    inline constexpr const char* NeutralTagStaticStruct = "FMassNeutralAITag::StaticStruct";

    // Buildings: the entity grid and the spline payload
    inline constexpr const char* GridSubsystem       = "UWorld::GetSubsystem<UCrEntityGridSubsystem>";
    inline constexpr const char* GridForEachCell     = "UCrEntityGridSubsystem::ForEachCellInRadius";
    inline constexpr const char* SplinePayloadGet    = "SplinePayload::GetSplineActorData";
    inline constexpr const char* SplinePayloadDestroy = "FAuAPSplineActorPayloadData::~FAuAPSplineActorPayloadData";
    inline constexpr const char* SplinePayloadStaticStruct = "FAuAPSplineActorPayloadData::StaticStruct";

    // Building sprites: the private capture world, the render target and the readback
    inline constexpr const char* SpawnObject         = "StaticConstructObject (TSubclassOf)";
    inline constexpr const char* SetObjectFlags      = "UObjectBase::AtomicallySetFlags";
    inline constexpr const char* RegisterComponent   = "UActorComponent::RegisterComponentWithWorld";
    inline constexpr const char* UnregisterComponent = "UActorComponent::UnregisterComponent";
    inline constexpr const char* DestroyComponent    = "UActorComponent::DestroyComponent";
    inline constexpr const char* InitRenderTarget    = "UTextureRenderTarget2D::InitCustomFormat";
    inline constexpr const char* RenderTargetResource = "UTextureRenderTarget2D::GameThread_GetRenderTargetResource";
    inline constexpr const char* TransitionResources = "FRHIComputeCommandList::Transition";
    inline constexpr const char* FenceConstruct      = "FRenderCommandFence::FRenderCommandFence";
    inline constexpr const char* FenceDestroy        = "FRenderCommandFence::~FRenderCommandFence";
    inline constexpr const char* FenceBegin          = "FRenderCommandFence::BeginFence";
    inline constexpr const char* FencePoll           = "FRenderCommandFence::IsFenceComplete";
    inline constexpr const char* FromValidEName      = "FNameEntryId::FromValidEName";
    inline constexpr const char* SubmitRenderCommand = "EnqueueUniqueRenderCommand";
    inline constexpr const char* ImmediateCommandList = "FRHICommandListExecutor::GetImmediateCommandList";
    inline constexpr const char* CreateRHITexture    = "FDynamicRHI::RHICreateTexture";
    inline constexpr const char* ReadbackConstruct   = "FRHIGPUTextureReadback::FRHIGPUTextureReadback";
    inline constexpr const char* ReadbackDestroy     = "FRHIGPUTextureReadback::~FRHIGPUTextureReadback";
    inline constexpr const char* ReadbackCopy        = "FRHIGPUTextureReadback::EnqueueCopy";
    inline constexpr const char* ReadbackLock        = "FRHIGPUTextureReadback::Lock";
    inline constexpr const char* ReadbackUnlock      = "FRHIGPUTextureReadback::Unlock";
    inline constexpr const char* GpuFencePoll        = "FRHIGPUFence::Poll";
    inline constexpr const char* MarkForDelete       = "FRHIResource::MarkForDelete";
    inline constexpr const char* MemoryMalloc        = "FMemory::Malloc";
    inline constexpr const char* ReadbackFree        = "FMemory::Free (readback)";
    inline constexpr const char* DynamicRHI          = "GDynamicRHI (anchor)";
    inline constexpr const char* RenderCommandPipe   = "GRenderCommandPipe (anchor)";
    inline constexpr const char* ThreadedRendering   = "GIsThreadedRendering (anchor)";
    inline constexpr const char* RHIMultithreading   = "GRHISupportsRHIThread (anchor)";
    inline constexpr const char* GpuCount            = "GNumExplicitGPUsForRendering (anchor)";
    inline constexpr const char* CreateWorld         = "UWorld::CreateWorld";
    inline constexpr const char* InitWorld           = "UWorld::InitWorld";
    inline constexpr const char* UpdateWorldComponents = "UWorld::UpdateWorldComponents";
    inline constexpr const char* CleanupWorld        = "UWorld::CleanupWorld";
    inline constexpr const char* WorldContext        = "UEngine::GetWorldContextFromWorld";
    inline constexpr const char* GetSubsystemInternal = "FSubsystemCollectionBase::GetSubsystemInternal";
    inline constexpr const char* GarbageCollectingLocking = "IsGarbageCollectingAndLockingUObjectHashTables";
    inline constexpr const char* PurgePending        = "IsIncrementalPurgePending";
    inline constexpr const char* ReachabilityPending = "IsIncrementalReachabilityPending";
    inline constexpr const char* TickableTickType    = "FTickableGameObject::GetTickableTickType";
    inline constexpr const char* VisibilityPending   = "UWorld::IsVisibilityRequestPending";
    inline constexpr const char* DestroyActor        = "UWorld::DestroyActor";
    inline constexpr const char* ActiveGarbageCollection = "GIsGarbageCollecting (anchor)";
    inline constexpr const char* EngineStorage       = "GEngine (anchor)";
    inline constexpr const char* ReadbackGlobals     = "Readback globals (executable headers)";
    inline constexpr const char* ReleaseGuard        = "UObject::ReleaseRef (guard)";
}
