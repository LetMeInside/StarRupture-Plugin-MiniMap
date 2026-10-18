// Included inside PrivateCaptureWorld::Implementation. One in-flight candidate,
// serviced only by BuildingSpriteCapture's existing guarded game-thread tick.
// All render callbacks belong to ReadbackNativeAdapter and carry Job*, never Actor*.
using BN = SDK::UObject;
using Class = SDK::UClass;
static constexpr size_t PrivateLevelActorLimit = 4096;
struct BlueprintCounters
{
    uint64_t Encountered{}, Eligible{}, Constructed{}, ConstructionFailed{}, Unsupported{},
        Captured{}, CaptureFailed{}, ValidReadbacks{}, Retired{}, Quarantines{};
} totals;
enum class BlueprintPhase { Idle, Prepared, C0, Readback, Retiring, Quarantined };
struct BlueprintWork
{
    BlueprintPhase Current = BlueprintPhase::Idle;
    bool Active = false, CaptureUsed = false, Committed = false, Detached = false, Failed = false, VisualIncomplete = false, ResourceCreationUncertain = false, NativeAccessViolation = false, ActorSpawnAttempted = false;
    uint64_t Id{}, Tick{}, PreparedTick{}, C0Tick{};
    uint32_t Building{};
    std::string Key, ClassPath;
    const char* Failure = "none";
    Reference Actor, Capture, Target;
    std::array<Reference, 96> Metadata{};
    size_t MetadataCount{};
    uintptr_t SelectedClass{};
    uint64_t LevelHash{};
    int32_t LevelCount{};
    SDK::FVector Center{}, Extent{};
    MiniMapReadbackNative::Job* Job{};
    std::vector<uint8_t> PrivateImage;
} blueprint;
uint64_t nextBlueprintId{}, blueprintTick{};
MiniMapCaptureLeases::Token ownerLease; // Existing production lease, held through native retirement.
std::unordered_set<std::string> attemptedBlueprints; // Exact candidate identities retained until session reset.
// One waiting candidate, containing only copied CPU identity/policy/path data.
// No selected-class pointer or borrowed engine object survives the offer.
struct DeferredBlueprint
{
    bool Pending=false, ClassificationObserved=false;
    MiniMapSpriteDiscoveryCore::Key Identity;
    int32_t ActorObject=-1, ActorName=-1;
    uint32_t ActorNumber{}, Building{};
    MiniMapSpriteDiscoveryCore::Tier Tier=MiniMapSpriteDiscoveryCore::Tier::Unknown;
    MiniMapSpriteClassification::Exclusion Exclusion=MiniMapSpriteClassification::Exclusion::Unknown;
    uint64_t Generation{}, Id{};
    size_t SourceCursor{};
    double SourcePublication=-1;
    uintptr_t SessionToken{}; // equality-only address token; never dereferenced
    std::string Key, ClassPath;
} deferredBlueprint;
void DiscardDeferredBlueprint(const char* reason)
{
    if (!deferredBlueprint.Pending) return;
    LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=deferred candidate outcome=discarded reason=%s",
        (unsigned long long)deferredBlueprint.Id,reason);
    deferredBlueprint={};
    if (ownerLease) { blueprint.Active=true; BlueprintFail("deferred candidate",reason); }
}
MiniMapCaptureRetirement::FenceOwner blueprintFence;

std::string BlueprintText(const char* value)
{
    std::string text=value?value:"unknown reason";
    std::replace(text.begin(),text.end(),'_',' ');
    return text;
}
void BlueprintLog(const char* stage, bool success, const char* reason = "verified")
{
    const auto readableStage=BlueprintText(stage);
    const auto readableReason=BlueprintText(reason);
    LOG_INFO("MiniMap: private_blueprint_actor id=%llu generation=%llu building=%u step=%s outcome=%s reason=%s",
        (unsigned long long)blueprint.Id, (unsigned long long)state.Generation, blueprint.Building,
        readableStage.c_str(), success ? "success" : "failure", readableReason.c_str());
}
void BlueprintTotals()
{
    LOG_INFO("MiniMap: private_blueprint_actor totals encountered=%llu eligible=%llu constructed=%llu construction_failed=%llu unsupported=%llu captured=%llu capture_failed=%llu valid_readbacks=%llu retired=%llu quarantines=%llu",
        (unsigned long long)totals.Encountered, (unsigned long long)totals.Eligible,
        (unsigned long long)totals.Constructed, (unsigned long long)totals.ConstructionFailed,
        (unsigned long long)totals.Unsupported, (unsigned long long)totals.Captured,
        (unsigned long long)totals.CaptureFailed, (unsigned long long)totals.ValidReadbacks,
        (unsigned long long)totals.Retired, (unsigned long long)totals.Quarantines);
}
void BlueprintQuarantine(const char* reason)
{
    if (blueprint.Current != BlueprintPhase::Quarantined)
    {
        ++totals.Quarantines;
        BlueprintLog("quarantine", false, reason);
        BlueprintTotals();
    }
    if (ownerLease)
    {
        MiniMapBuildingSprites::RecordForLease(ownerLease,MiniMapBuildingSpriteCore::State::Failed,reason);
        MiniMapBuildingSprites::AbandonCapture(ownerLease);
    }
    blueprint.Current = BlueprintPhase::Quarantined;
    if (blueprint.Job) MiniMapReadbackNative::Cancel(blueprint.Job);
    Quarantine(reason); // prevents world cleanup and future access to dependent pointers
}
void ResetBlueprintSession()
{
    DiscardDeferredBlueprint("session reset");
    // Arm has excluded active/quarantined world ownership before reaching here.
    if (blueprint.Active || blueprint.Current == BlueprintPhase::Quarantined) return;
    if (totals.Encountered || totals.Eligible || totals.Constructed || totals.ConstructionFailed || totals.Unsupported ||
        totals.Captured || totals.CaptureFailed || totals.ValidReadbacks || totals.Retired || totals.Quarantines)
        BlueprintTotals();
    totals = {};
    blueprint = {};
    attemptedBlueprints.clear();
}
BN* FindBlueprintObject(const wchar_t* path, Class* type = nullptr, BN* outer = nullptr, bool exact = false)
{
    auto* hooks = GetHooks();
    auto* events = hooks ? hooks->Engine : nullptr;
    // Engine interface is acquired through the same loader getter used by the worker.
    const auto address = events ? events->GetStaticFindObjectByNameAddress() : 0;
    if (!address) return nullptr;
    using Find = BN*(*)(Class*, BN*, const wchar_t*, bool);
    auto* found = reinterpret_cast<Find>(address)(type, outer, path, exact);
    Identity id;
    return found && Identify(reinterpret_cast<uintptr_t>(found), id) ? found : nullptr;
}
BN* KeepBlueprintMetadata(BN* object)
{
    const auto bits = reinterpret_cast<uintptr_t>(object);
    if (!object) return nullptr;
    for (size_t i = 0; i < blueprint.MetadataCount; ++i)
        if (blueprint.Metadata[i].Object.Bits == bits)
            return CheckOwned(blueprint.Metadata[i]) ? object : nullptr;
    if (blueprint.MetadataCount == blueprint.Metadata.size()) return nullptr;
    auto& ref = blueprint.Metadata[blueprint.MetadataCount++];
    return Hold(bits, ref) ? object : nullptr;
}
Class* BlueprintClass(const wchar_t* path)
{
    auto* object = FindBlueprintObject(path);
    Identity id;
    uintptr_t nativeClassType{};
    if (!object || !Identify(reinterpret_cast<uintptr_t>(object), id, sizeof(Class)) ||
        !Read(metadata[World].Ref.Object.Bits+0x10,nativeClassType) ||
        reinterpret_cast<uintptr_t>(object->Class)!=nativeClassType) return nullptr;
    return static_cast<Class*>(KeepBlueprintMetadata(object));
}
// Typed SDK calls retain the existing leaf SEH boundary without reflecting,
// marshalling, or second-guessing generated function metadata. Keep these
// helpers free of destructible locals so MSVC permits __try/__except.
bool GuardedBeginDeferredActorSpawn(SDK::UObject* worldContext,
    SDK::TSubclassOf<SDK::AActor> actorClass, const SDK::FTransform* transform,
    SDK::ESpawnActorCollisionHandlingMethod collision, SDK::AActor* owner,
    SDK::ESpawnActorScaleMethod scale, SDK::AActor** result) noexcept
{
    __try
    {
        *result=SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
            worldContext,actorClass,*transform,collision,owner,scale);
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
bool GuardedFinishSpawningActor(SDK::AActor* actor, const SDK::FTransform* transform,
    SDK::ESpawnActorScaleMethod scale, SDK::AActor** result) noexcept
{
    __try
    {
        *result=SDK::UGameplayStatics::FinishSpawningActor(actor,*transform,scale);
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
bool GuardedActorClassContract(Class* type, Class* actorClass, bool* isActorClass) noexcept
{
    __try
    {
        *isActorClass=SDK::UKismetMathLibrary::ClassIsChildOf(
            SDK::TSubclassOf<SDK::UObject>(type),SDK::TSubclassOf<SDK::UObject>(actorClass));
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
bool GuardedKismetGetActorBounds(const SDK::AActor* actor,
    SDK::FVector* origin, SDK::FVector* extent) noexcept
{
    __try
    {
        SDK::UKismetSystemLibrary::GetActorBounds(actor,origin,extent);
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}
bool GuardedSetShowFlagSettings(SDK::USceneCaptureComponent* capture,
    const SDK::TArray<SDK::FEngineShowFlagsSetting>* settings) noexcept
{
    __try
    {
        capture->SetShowFlagSettings(*settings);
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
bool GuardedSetWorldLocationAndRotation(SDK::USceneComponent* component,
    const SDK::FVector* location, const SDK::FRotator* rotation,
    bool sweep, bool teleport) noexcept
{
    __try
    {
        component->K2_SetWorldLocationAndRotation(*location,*rotation,sweep,nullptr,teleport);
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
bool GuardedCaptureScene(SDK::USceneCaptureComponent2D* capture) noexcept
{
    __try
    {
        capture->CaptureScene();
        return true;
    }
    __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
struct PointerArray { uintptr_t Data{}; int32_t Count{}, Capacity{}; };
bool BlueprintArray(uintptr_t address, PointerArray& out, size_t limit = PrivateLevelActorLimit)
{
    return Read(address, out) && out.Count >= 0 && out.Capacity >= out.Count && size_t(out.Capacity) <= limit &&
        (!out.Count || (out.Data && N::IsMemoryRangeReadable(out.Data, size_t(out.Count) * 8, 8)));
}
bool BlueprintLevel(uint64_t& hash, int32_t& count)
{
    PointerArray a;
    if (!CheckOwned(state.PersistentLevel) || !BlueprintArray(state.PersistentLevel.Object.Bits + 0xa0, a, PrivateLevelActorLimit)) return false;
    hash = 0; count = 0;
    for (int32_t i = 0; i < a.Count; ++i)
    {
        uintptr_t actor{}; Identity id;
        if (!Read(a.Data + size_t(i) * 8, actor)) return false;
        if (!actor) continue;
        if (!Identify(actor, id, sizeof(SDK::AActor))) return false;
        hash ^= actor * 1099511628211ULL; ++count;
    }
    return true;
}
bool BlueprintLevelContains(uintptr_t target, bool expected)
{
    PointerArray actors;
    if (!target || !CheckOwned(state.PersistentLevel) ||
        !BlueprintArray(state.PersistentLevel.Object.Bits+0xa0,actors,PrivateLevelActorLimit)) return false;
    bool found=false;
    for (int32_t i=0;i<actors.Count;++i)
    {
        uintptr_t actor{}; Identity id;
        if (!Read(actors.Data+size_t(i)*8,actor)) return false;
        if (!actor) continue;
        if (!Identify(actor,id,sizeof(SDK::AActor))) return false;
        found |= actor==target;
    }
    return found==expected;
}
bool BlueprintActorState(bool finished)
{
    const auto actor = blueprint.Actor.Object.Bits;
    uint8_t a{}, b{}; uintptr_t outer{}, owner{}, type{}, instigator{};
    return CheckOwned(blueprint.Actor) && Read(actor + 0x10, type) && type == blueprint.SelectedClass &&
        Read(actor + 0x20, outer) && outer == state.PersistentLevel.Object.Bits &&
        Read(actor + offsetof(SDK::AActor, Owner), owner) && !owner &&
        Read(actor + offsetof(SDK::AActor, Instigator), instigator) && !instigator &&
        Read(actor + 0x5c, a) && bool(a & 1) == finished && !(a & 2) &&
        Read(actor + 0x5d, b) && !(b & 0x1a) && PrivateState(true) && Unchanged() && QuietGC() &&
        BlueprintLevelContains(actor,true);
}

static SDK::FTransform BlueprintIdentityTransform()
{
    SDK::FTransform t{};
    t.Rotation.W = 1;
    t.Scale3D = SDK::FVector(1, 1, 1);
    return t;
}
// World-space AABB criteria for Kismet framing.
// This helper only inspects copied numbers; it cannot change capture state.
static const char* BlueprintBoundsRejection(const SDK::FVector& origin, const SDK::FVector& extent)
{
    const bool finiteBounds=std::isfinite(origin.X) && std::isfinite(origin.Y) && std::isfinite(origin.Z) &&
        std::isfinite(extent.X) && std::isfinite(extent.Y) && std::isfinite(extent.Z);
    if (!finiteBounds) return "nonfinite_center_or_extent";
    if (!(extent.X>0 && extent.Y>0 && extent.Z>=0)) return "nonpositive_XY_or_negative_Z_extent";
    const double cameraZ=origin.Z+extent.Z+1000.0;
    const float orthoWidth=float(2.0*(std::max)(extent.X,extent.Y)*1.1);
    if (!std::isfinite(cameraZ)) return "nonfinite_camera_height";
    if (!std::isfinite(orthoWidth) || !(orthoWidth>0)) return "nonfinite_or_nonpositive_ortho_width";
    return nullptr;
}

bool GetBlueprintActorBounds()
{
    SDK::FVector origin{},extent{};
    auto* actor=reinterpret_cast<SDK::AActor*>(blueprint.Actor.Object.Bits);
    if (!GuardedKismetGetActorBounds(actor,&origin,&extent))
    { blueprint.NativeAccessViolation=true; blueprint.Failure="Kismet actor-bounds call was interrupted"; return false; }
    const bool usable=BlueprintBoundsRejection(origin,extent)==nullptr;
    if (!usable)
    {
        origin=SDK::FVector(0.0,0.0,0.0);
        // Identity spawn is at the origin. Use a practical 10 m footprint
        // (11 m orthographic width including the existing 10% padding).
        extent=SDK::FVector(500.0,500.0,500.0);
        blueprint.Center=origin;
        blueprint.Extent=extent;
        return true;
    }
    blueprint.Center=origin;
    blueprint.Extent=extent;
    return true;
}
bool AdmitBlueprint(Class* type)
{
    auto* actorClass=BlueprintClass(L"/Script/Engine.Actor");
    if (!type || !actorClass)
    { blueprint.Failure="Actor class metadata is unavailable"; return false; }
    bool isActorClass=false;
    if (!GuardedActorClassContract(type,actorClass,&isActorClass))
    { blueprint.Failure="Actor class contract call was interrupted"; return false; }
    if (!isActorClass)
    { blueprint.Failure="resolved Blueprint class is not an Actor subclass"; return false; }
    Identity cdoId;
    if (!type->ClassDefaultObject || type->ClassDefaultObject->Class!=type ||
        !Identify(reinterpret_cast<uintptr_t>(type->ClassDefaultObject),cdoId) ||
        !KeepBlueprintMetadata(type->ClassDefaultObject))
    { blueprint.Failure="Blueprint class default object identity could not be verified"; return false; }
    return true;
}
bool BlueprintBindings()
{
    const auto* api = N::Get();
    return N::IsInitialized() && api && api->world.destroyActor;
}

// All opaque native factory/registration calls are isolated here. The bindings
// are already verified by the production worker; no new executable patch/scan.
bool BlueprintNative(unsigned operation, uintptr_t object, uintptr_t argument = 0, uintptr_t* result = nullptr) noexcept
{
    __try
    {
        const auto& b=*MiniMapNative::Get();
        switch (operation)
        {
        case 0: {
            const Class* type = reinterpret_cast<Class*>(argument);
            struct Subclass { const Class* Type; } value{type};
            *result = reinterpret_cast<uintptr_t(*)(const Subclass*,uintptr_t)>(b.asset.spawnObject)(&value,object);
            break;
        }
        case 1: reinterpret_cast<void(*)(uintptr_t,uint32_t)>(b.asset.setObjectFlags)(object,0x40); break;
        case 2: reinterpret_cast<void(*)(uintptr_t,uintptr_t,void*)>(b.player.registerComponent)(object,argument,nullptr); break;
        case 3: reinterpret_cast<void(*)(uintptr_t)>(b.player.unregisterComponent)(object); break;
        case 4: *result = N::Get()->world.destroyActor(object,false,false); break;
        case 5: reinterpret_cast<void(*)(uintptr_t,bool)>(b.player.destroyComponent)(object,false); break;
        case 6: reinterpret_cast<void(*)(uintptr_t,uint32_t,uint32_t,uint8_t,bool)>(b.texture.initRenderTarget)(object,256,256,10,true); break;
        case 7: *result = reinterpret_cast<uintptr_t(*)(uintptr_t)>(b.texture.getRenderTargetResource)(object); break;
        default: return false;
        }
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; return false; }
}
bool NewBlueprintResource(Class* type, Reference& ref)
{
    uintptr_t bits{}; Identity extent;
    if (!type || type->Size<0x28 || type->Size>0x4000) return false;
    blueprint.ResourceCreationUncertain=true;
    if (!BlueprintNative(0,state.Private.Object.Bits,reinterpret_cast<uintptr_t>(type),&bits)) return false;
    if (!bits) return false;
    if (!Hold(bits, ref) || !Identify(bits,extent,size_t(type->Size))) return false;
    uintptr_t actual{}, outer{};
    if (!Read(bits+0x10,actual) || actual!=reinterpret_cast<uintptr_t>(type) ||
        !Read(bits+0x20,outer) || outer!=state.Private.Object.Bits || !BlueprintNative(1,bits)) return false;
    blueprint.ResourceCreationUncertain=false;
    return true;
}
bool PrepareBlueprintCapture()
{
    auto* captureClass = BlueprintClass(L"/Script/Engine.SceneCaptureComponent2D");
    auto* targetClass = BlueprintClass(L"/Script/Engine.TextureRenderTarget2D");
    if (!captureClass || !targetClass || captureClass->Size!=sizeof(SDK::USceneCaptureComponent2D) ||
        targetClass->Size!=sizeof(SDK::UTextureRenderTarget2D) ||
        !NewBlueprintResource(captureClass,blueprint.Capture) || !NewBlueprintResource(targetClass,blueprint.Target)) return false;
    auto* capture = reinterpret_cast<SDK::USceneCaptureComponent2D*>(blueprint.Capture.Object.Bits);
    auto* target = reinterpret_cast<SDK::UTextureRenderTarget2D*>(blueprint.Target.Object.Bits);
    ConfigureSceneCapture(capture, blueprint.Extent);
    // The private scene has no borrowed gameplay lighting. Unlit material capture
    // is deliberate and logged; it is not asserted equivalent to the lit producer.
    SDK::FEngineShowFlagsSetting flags[11]{};
    const wchar_t* names[] = {L"Fog",L"Atmosphere",L"VolumetricFog",L"Cloud",L"MotionBlur",L"TemporalAA",L"AntiAliasing",L"PostProcessing",L"Bloom",L"EyeAdaptation",L"Lighting"};
    for (int i=0;i<11;++i) { flags[i].ShowFlagName=SDK::FString(names[i]); flags[i].Enabled=false; }
    SDK::TArray<SDK::FEngineShowFlagsSetting> showFlags(flags,11,11);
    if (!GuardedSetShowFlagSettings(capture,&showFlags)) return false;
    target->bAutoGenerateMips=false; target->bSupportsUAV=false;
    uint32_t targetFlags{};
    if (!Read(blueprint.Target.Object.Bits+0x144,targetFlags)) return false;
    targetFlags &= ~uint32_t(1);
    std::memcpy(reinterpret_cast<uint8_t*>(target)+0x144,&targetFlags,4);
    target->ClearColor=SDK::FLinearColor(0,0,0,1);
    if (!BlueprintNative(6,blueprint.Target.Object.Bits)) return false;
    capture->TextureTarget=target;
    const SDK::FVector cameraLocation(blueprint.Center.X,blueprint.Center.Y,blueprint.Center.Z+blueprint.Extent.Z+1000.0);
    const SDK::FRotator cameraRotation(-90,-90,0);
    if (!GuardedSetWorldLocationAndRotation(capture,&cameraLocation,&cameraRotation,false,true) ||
        !BlueprintNative(2,blueprint.Capture.Object.Bits,state.Private.Object.Bits)) return false;
    uint8_t registered{}; uintptr_t world{};
    if (!Read(blueprint.Capture.Object.Bits+0x94,registered) || !(registered&1) ||
        !Read(blueprint.Capture.Object.Bits+0xa8,world) || world!=state.Private.Object.Bits) return false;
    uintptr_t resource{};
    if (!BlueprintNative(7,blueprint.Target.Object.Bits,0,&resource) || !resource) return false;
    blueprint.Job=MiniMapReadbackNative::CreateExternalJob(reinterpret_cast<void*>(resource),256,256);
    if (!blueprint.Job) return false;
    return true;
}

struct BlueprintRetirementOperations
{
    Implementation* Owner;
    bool Available() const
    {
        const auto& b=*MiniMapNative::Get();
        return Owner->state.NativePrerequisitesVerified && MiniMapNative::Get() && MiniMapNative::Get()->player.isGameThread && MiniMapNative::Get()->player.isGameThread() && MiniMapReadbackNative::CaptureRetirementRendererAvailable() &&
            b.texture.renderFenceConstruct && b.texture.renderFenceDestroy && b.texture.renderFenceBegin && b.texture.renderFencePoll;
    }
    bool Construct(void* storage) const { reinterpret_cast<void(*)(void*)>(MiniMapNative::Get()->texture.renderFenceConstruct)(storage); return true; }
    bool Begin(void* storage) const { reinterpret_cast<void(*)(void*,int32_t)>(MiniMapNative::Get()->texture.renderFenceBegin)(storage,1); return true; }
    bool Poll(void* storage) const { return reinterpret_cast<bool(*)(const void*)>(MiniMapNative::Get()->texture.renderFencePoll)(storage); }
    void Destroy(void* storage) const { reinterpret_cast<void(*)(void*)>(MiniMapNative::Get()->texture.renderFenceDestroy)(storage); }
};
// Only callable contiguously after THIS owner's admitted engine destruction.
// It deliberately does not call Identify or modify its live-object policy.
bool ReleaseRetiringOwned(Reference& ref)
{
    if (!ref.Owned || ref.Uncertain || !QuietGC()) return false;
    DefinitionSlots::Slot slot; SlotReader reader; int32_t index{}, count{}; uint32_t flags{};
    const auto bits=ref.Object.Bits;
    if (DefinitionSlots::ReadSlot(reader,reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray),ref.Object.Index,slot)!=DefinitionSlots::Status::Success || slot.Object!=bits ||
        (slot.Flags&0x10000000) || !(slot.Flags&0x20000000) || !(slot.Flags&0x00200000) ||
        !N::IsMemoryRangeReadable(bits,0x28,8) || !Read(bits+0xc,index) || index!=ref.Object.Index ||
        !Read(bits+8,flags) || (flags&0x1b400) || !Read(slot.Item+0x14,count) || count<1) return false;
    if (slot.Serial!=ref.Object.Serial)
    {
        if (ref.Object.Serial!=0 || slot.Serial<=0) return false;
        ref.Object.Serial=slot.Serial; // continuously owned, never reacquired after uncertainty
    }
    if (!Invoke(Operation::Release,bits)) { ref.Uncertain=true; return false; }
    ref.Owned=false; ref.Attempted=false;
    return true;
}
bool RetireBlueprintObjects()
{
    if (!QuietGC() || !Unchanged() || !PrivateState(true) || blueprint.Job) return false;
    if (blueprint.Actor.Owned)
    {
        // Finished can be false for a deferred Actor if construction wasn't entered.
        uint8_t finished{};
        if (!Read(blueprint.Actor.Object.Bits+0x5c,finished) || !BlueprintActorState(bool(finished&1))) return false;
        uintptr_t destroyed{};
        if (!BlueprintNative(4,blueprint.Actor.Object.Bits,0,&destroyed) || !destroyed) return false;
        uint8_t a{}, b{};
        if (!Read(blueprint.Actor.Object.Bits+0x5c,a) || (a&2) ||
            !Read(blueprint.Actor.Object.Bits+0x5d,b) || !(b&2) || (b&0x18) ||
            !BlueprintLevelContains(blueprint.Actor.Object.Bits,false)) return false;
        if (!ReleaseRetiringOwned(blueprint.Actor)) return false;
    }
    if (blueprint.Capture.Owned)
    {
        uint8_t init{};
        if (!CheckOwned(blueprint.Capture) || !Read(blueprint.Capture.Object.Bits+0x97,init) || (init&0x28) ||
            !BlueprintNative(5,blueprint.Capture.Object.Bits)) return false;
        uint8_t flags{};
        if (!Read(blueprint.Capture.Object.Bits+0x94,flags) || (flags&3) || !ReleaseRetiringOwned(blueprint.Capture)) return false;
    }
    if (!Drop(blueprint.Target)) return false;
    for (size_t i=blueprint.MetadataCount;i>0;--i) if (!Drop(blueprint.Metadata[i-1])) return false;
    return Unchanged() && PrivateState(true) && QuietGC();
}
void CompleteBlueprintRetirement()
{
    if (!RetireBlueprintObjects()) { BlueprintQuarantine("actor_or_resource_retirement_postcondition_or_release_uncertain"); return; }
    ++totals.Retired;
    // Remove callable/borrowed native pointers after the exact reference ledger
    // has been consumed. Only copied CPU images/identity remain.
    blueprint.Actor={}; blueprint.Capture={}; blueprint.Target={};
    blueprint.Metadata={};
    blueprint.MetadataCount=0;
    blueprint.SelectedClass=0;
    blueprint.ResourceCreationUncertain=blueprint.NativeAccessViolation=blueprint.ActorSpawnAttempted=false;
    if (blueprint.Failed)
        MiniMapBuildingSprites::RecordForLease(ownerLease,MiniMapBuildingSpriteCore::State::Failed,blueprint.Failure);
    MiniMapBuildingSprites::AbandonCapture(ownerLease);
    const auto fence=blueprintFence.GetState();
    MiniMapCaptureLeases::RetirementReceipt receipt{
        blueprint.Job==nullptr,
        fence==MiniMapCaptureRetirement::FenceState::Empty||fence==MiniMapCaptureRetirement::FenceState::Destroyed,
        !blueprint.Actor.Owned&&!blueprint.Capture.Owned&&!blueprint.Target.Owned,
        blueprint.MetadataCount==0,
        !deferredBlueprint.Pending,
        blueprint.SelectedClass==0,
        !blueprint.ResourceCreationUncertain&&!blueprint.NativeAccessViolation&&!blueprint.ActorSpawnAttempted};
    if (ownerLease&&!MiniMapBuildingSprites::ReturnLease(ownerLease,receipt))
    { BlueprintQuarantine("production_lease_retirement_not_certified"); return; }
    ownerLease={};
    blueprint.Active=false; blueprint.Current=BlueprintPhase::Idle;
    if (blueprint.Failed) BlueprintLog("final",false,blueprint.Failure);
    else LOG_INFO("MiniMap: sprite capture building=%u result=success blueprint=%s resolution=256x256",
        blueprint.Building,blueprint.ClassPath.c_str());
}
void BeginBlueprintRetirement()
{
    if (!blueprint.Active || blueprint.Current==BlueprintPhase::Quarantined) return;
    if (!QuietGC() || !Unchanged() || !PrivateState(true)) { BlueprintQuarantine("retirement_admission_invalid"); return; }
    if (blueprint.Capture.Owned)
    {
        uint8_t registered{};
        if (!CheckOwned(blueprint.Capture) || !Read(blueprint.Capture.Object.Bits+0x94,registered) ||
            ((registered&1) && !BlueprintNative(3,blueprint.Capture.Object.Bits)))
        { BlueprintQuarantine("capture_unregistration_uncertain"); return; }
        if (!Read(blueprint.Capture.Object.Bits+0x94,registered) || (registered&3))
        { BlueprintQuarantine("capture_unregistration_postconditions"); return; }
    }
    blueprint.Detached=true;
    if (!blueprint.CaptureUsed)
    {
        BlueprintRetirementOperations operations{this};
        if (!MiniMapReadbackNative::DiscardExternalBeforeCapture(blueprint.Job) || !blueprintFence.Release(operations))
        { BlueprintQuarantine("unused_job_or_fence_release_failed"); return; }
        CompleteBlueprintRetirement(); return;
    }
    BlueprintRetirementOperations operations{this};
    // CaptureScene/C0 may have queued renderer work even when C10 wasn't reached.
    // An ordered existing RHI-depth fence, never a flush/GC, drains that use first.
    if (!blueprintFence.Insert(operations,true)) { BlueprintQuarantine("capture_drain_fence_insertion_uncertain"); return; }
    blueprint.Current=BlueprintPhase::Retiring;
}
void BlueprintFail(const char* stage, const char* reason, bool unsupported = false)
{
    if (!blueprint.Failed)
    {
        blueprint.Failed=true; blueprint.Failure=reason;
        MiniMapBuildingSprites::RecordForLease(ownerLease,MiniMapBuildingSpriteCore::State::Failed,reason);
        BlueprintLog(stage,false,reason);
        if (unsupported) ++totals.Unsupported;
        else if (!blueprint.Actor.Owned) ++totals.ConstructionFailed;
        else ++totals.CaptureFailed;
    }
    BeginBlueprintRetirement();
}
void BlueprintSkipBeforeConstruction(const char* reason)
{
    ++totals.Unsupported;
    BlueprintLog("candidate admission",false,reason);
    blueprint.Active=true;
    BlueprintFail("candidate admission",reason);
}
void BlueprintCandidateException() noexcept
{
    try
    {
        if (blueprint.ActorSpawnAttempted || blueprint.Actor.Owned || blueprint.ResourceCreationUncertain || blueprint.NativeAccessViolation)
            BlueprintQuarantine("candidate processing threw after native construction or resource work began");
        else if (blueprint.MetadataCount || blueprint.Capture.Owned || blueprint.Target.Owned)
            BlueprintFail("candidate processing","candidate processing threw before Actor creation; retained references will be retired",true);
        else BlueprintSkipBeforeConstruction("candidate processing threw before creating a native object");
    }
    catch (...) { BlueprintQuarantine("candidate exception recovery could not establish safe ownership"); }
}
std::filesystem::path BlueprintOutputPath(const wchar_t* suffix)
{
    std::filesystem::path pluginDirectory;
    if (!MiniMapPluginPaths::GetPluginDirectory(pluginDirectory)) return {};
    const auto directory=pluginDirectory/L"MiniMap"/L"Cache"/L"Buildings";
    std::error_code error; std::filesystem::create_directories(directory,error);
    if (error) return {};
    const auto name=L"private_building_"+std::to_wstring(blueprint.Building)+
        L"_g_"+std::to_wstring(state.Generation)+L"_candidate_"+std::to_wstring(blueprint.Id)+suffix;
    return directory/name;
}
bool WriteBlueprintImage(const std::vector<uint8_t>& rgba, const wchar_t* suffix)
{
    const auto path=BlueprintOutputPath(suffix);
    if (path.empty()) { BlueprintLog("image_output",false,"module_path_or_directory_unavailable"); return false; }
    const auto result=MiniMapCapturePng::Write(path,256,256,rgba,false);
    if (!result.Ok)
    {
        const auto readableStage=BlueprintText(result.Stage);
        LOG_ERROR("MiniMap: private_blueprint_actor id=%llu step=image output outcome=failure path=%ls reason=%s HRESULT=%08lx production publication=independent",
            (unsigned long long)blueprint.Id,path.c_str(),readableStage.c_str(),(unsigned long)result.Error);
    }
    return result.Ok;
}
void ExportBlueprintReadback()
{
    const auto* pixels=MiniMapReadbackNative::FloatPixels(blueprint.Job);
    const auto* stats=MiniMapReadbackNative::FloatStats(blueprint.Job);
    MiniMapCapturePng::Image image;
    if (!pixels || !stats || !MiniMapCapturePng::Convert(pixels->data(),pixels->size(),256,256,256,256,image) ||
        image.Rgba.size()!=256*256*4 || image.Sanitized || stats->Nonfinite || !stats->Foreground || !stats->Background)
    {
        blueprint.Failed=true; blueprint.Failure="readback_pixels_dimensions_nonfinite_or_empty_alpha";
        ++totals.CaptureFailed; BlueprintLog("image_validation",false,blueprint.Failure); return;
    }
    ++totals.ValidReadbacks;
    blueprint.PrivateImage=std::move(image.Rgba);
    MiniMapStaticAssembly::Footprint bounds;
    bounds.Min={blueprint.Center.X-blueprint.Extent.X,blueprint.Center.Y-blueprint.Extent.Y,blueprint.Center.Z-blueprint.Extent.Z};
    bounds.Max={blueprint.Center.X+blueprint.Extent.X,blueprint.Center.Y+blueprint.Extent.Y,blueprint.Center.Z+blueprint.Extent.Z};
    bounds.Initialized=true;
    MiniMapBuildingSprites::Complete(ownerLease,bounds,blueprint.PrivateImage);
#if MINIMAP_DEBUG_UI
    // Optional export is independent of production texture publication.
    try { WriteBlueprintImage(blueprint.PrivateImage,L"_blueprint.png"); }
    catch (...) { BlueprintLog("image output",false,"optional PNG output failed"); }
#endif
}

void OfferBlueprint(const Candidate& candidate, uintptr_t selectedClass, uint32_t building, void* gameplay, bool resumed=false, uint64_t resumedId=0)
{
    const auto key=SessionVisualKey(candidate);
    if (!resumed && deferredBlueprint.Pending && deferredBlueprint.Key==key) return;
    if (!resumed) ++totals.Encountered;
    const auto id=resumed?resumedId:++nextBlueprintId;
    if (blueprint.Active || blueprint.Current==BlueprintPhase::Quarantined || state.Current!=Phase::Ready)
    {
        LOG_INFO("MiniMap: private_blueprint_actor id=%llu generation=%llu building=%u candidate=%s step=owner readiness outcome=skipped reason=%s",
            (unsigned long long)id,(unsigned long long)candidate.Identity.Generation,building,key.c_str(),
            blueprint.Active?"private capture owner is busy":state.Current==Phase::Quarantined?"private capture session is quarantined":"private capture world is not ready");
        return;
    }
    blueprint={}; blueprint.Active=true; blueprint.Id=id; blueprint.Building=building; blueprint.Key=key;
    blueprint.Failure="Blueprint class identity is incomplete";
    if (!state.NativePrerequisitesVerified || !MiniMapNative::Get() || !MiniMapNative::Get()->player.isGameThread || !MiniMapNative::Get()->player.isGameThread())
    { BlueprintSkipBeforeConstruction("candidate was not offered on the Unreal game thread"); return; }
    if (gameplay!=state.SessionWorld)
    { BlueprintQuarantine("candidate gameplay world does not match the private capture session"); return; }
    QuietGCDetail gcDetail;
    if (!QuietGC(&gcDetail))
    {
        if (!deferredBlueprint.Pending && candidate.CanonicalBlueprint && candidate.CanonicalBlueprint->size()<=1024)
        {
            DeferredBlueprint pending;
            pending.Identity=candidate.Identity; pending.ActorObject=candidate.ActorObject;
            pending.ActorName=candidate.ActorName; pending.ActorNumber=candidate.ActorNumber;
            pending.Tier=candidate.Tier; pending.ClassificationObserved=candidate.ClassificationObserved;
            pending.Exclusion=candidate.Classification.ExcludeFromMiniMap;
            pending.Building=building; pending.Generation=state.Generation; pending.Id=id;
            pending.SessionToken=reinterpret_cast<uintptr_t>(gameplay);
            pending.Key=key; pending.ClassPath=*candidate.CanonicalBlueprint; pending.Pending=true;
            deferredBlueprint=std::move(pending);
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=candidate admission outcome=deferred reason=%s capacity=1",
                (unsigned long long)id,gcDetail.Step);
        }
        else LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=candidate admission outcome=skipped reason=deferred slot full or copied path unavailable no terminal attempt=1",(unsigned long long)id);
        blueprint.Active=false; blueprint.Current=BlueprintPhase::Idle;
        return;
    }
    attemptedBlueprints.insert(key);
    if (!Unchanged())
    { BlueprintQuarantine("gameplay world identity or baseline changed"); return; }
    if (!PrivateState(true))
    { BlueprintQuarantine("private capture world lifecycle validation failed"); return; }
    if (candidate.Identity.Spline || candidate.Identity.TintBits!=MiniMapSpriteDiscoveryCore::Key{}.TintBits)
    {
        blueprint.VisualIncomplete=true;
        BlueprintLog("visual customization","observed","candidate has spline or tint customization not reproduced by the reference capture");
    }
    if (!MiniMapReadbackNative::Preflight()) { BlueprintFail("native ABI","shared GPU readback backend is unavailable",true); return; }
    if (!BlueprintBindings()) { BlueprintFail("native ABI", "native Actor destruction entry is unavailable",true); return; }
    if (!candidate.CanonicalBlueprint || candidate.CanonicalBlueprint->empty() || candidate.CanonicalBlueprint->size()>1024 ||
        !candidate.CanonicalBlueprint->starts_with("/Game/") || !candidate.CanonicalBlueprint->ends_with("_C"))
    { BlueprintFail("class resolution","exact authored generated Blueprint class path is unavailable",true); return; }
    blueprint.ClassPath=*candidate.CanonicalBlueprint;
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,blueprint.ClassPath.c_str(),-1,nullptr,0);
    if (length<=1 || length>1025) { BlueprintFail("class resolution","Blueprint class path is not valid UTF-8",true); return; }
    std::wstring wide(size_t(length),L'\0');
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,blueprint.ClassPath.c_str(),-1,wide.data(),length))
    { BlueprintFail("class resolution","Blueprint class path conversion failed",true); return; }
    auto* generated=BlueprintClass(L"/Script/Engine.BlueprintGeneratedClass");
    auto* found=FindBlueprintObject(wide.c_str(),generated,nullptr,true);
    Identity identity;
    if (resumed) selectedClass=reinterpret_cast<uintptr_t>(found);
    if (!found || reinterpret_cast<uintptr_t>(found)!=selectedClass ||
        !Identify(selectedClass,identity,sizeof(SDK::UBlueprintGeneratedClass)) ||
        (resumed && identity.Index!=candidate.Identity.VisualClass) || found->Class!=generated)
    {
        if (resumed) LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=deferred admission outcome=permanent failure reason=resident class absent or copied visual class identity differs",(unsigned long long)id);
        BlueprintFail("class resolution","resident class is absent or differs from the selected visual class",true); return;
    }
    auto* type=static_cast<Class*>(found);
    if (!KeepBlueprintMetadata(type))
    { BlueprintFail("class resolution","Blueprint class identity could not be retained",true); return; }
    blueprint.SelectedClass=selectedClass;
    if (!AdmitBlueprint(type)) { BlueprintFail("construction admission",blueprint.Failure,true); return; }
    if (!BlueprintLevel(blueprint.LevelHash,blueprint.LevelCount))
    { BlueprintQuarantine("private persistent level could not be inspected before spawn"); return; }
    if (!QuietGC()) { BlueprintQuarantine("garbage collection became active before spawn"); return; }
    if (!PrivateState(true)) { BlueprintQuarantine("private capture world failed validation before spawn"); return; }
    if (!Unchanged()) { BlueprintQuarantine("gameplay world baseline changed before spawn"); return; }
    ++totals.Eligible;
    SDK::TSubclassOf<SDK::AActor> actorClass(type);
    const auto spawnTransform=BlueprintIdentityTransform();
    constexpr auto collision=SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    constexpr auto scale=SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
    SDK::AActor* spawnedActor{};
    blueprint.ActorSpawnAttempted=true;
    if (!GuardedBeginDeferredActorSpawn(reinterpret_cast<SDK::UObject*>(state.Private.Object.Bits),
        actorClass,&spawnTransform,collision,nullptr,scale,&spawnedActor))
    {
        BlueprintQuarantine("deferred spawn raised an access violation or private world ownership cannot be confirmed");
        return;
    }
    if (!spawnedActor)
    {
        uint64_t hash{}; int32_t count{};
        if (!Unchanged() || !PrivateState(true) || !BlueprintLevel(hash,count) || hash!=blueprint.LevelHash || count!=blueprint.LevelCount)
        { BlueprintQuarantine("Unreal returned no Actor and the private or gameplay world baseline changed"); return; }
        blueprint.ActorSpawnAttempted=false;
        BlueprintFail("deferred spawn","Unreal returned no Actor"); return;
    }
    if (!Hold(reinterpret_cast<uintptr_t>(spawnedActor),blueprint.Actor) || !BlueprintActorState(false))
    { ++totals.ConstructionFailed; BlueprintQuarantine("deferred_actor_retention_level_or_initialization_invalid"); return; }
    SDK::AActor* finishedActor{};
    if (!GuardedFinishSpawningActor(spawnedActor,&spawnTransform,scale,&finishedActor) ||
        finishedActor!=spawnedActor || !BlueprintActorState(true))
    { ++totals.ConstructionFailed; BlueprintQuarantine("FinishSpawning_return_or_ownership_isolation_uncertain"); return; }
    ++totals.Constructed;
    if (!GetBlueprintActorBounds())
    { BlueprintFail("actor bounds",blueprint.Failure); return; }
    if (!PrepareBlueprintCapture())
    {
        if (!blueprint.ResourceCreationUncertain && !blueprint.NativeAccessViolation && QuietGC() && Unchanged() && PrivateState(true) && BlueprintActorState(true))
            BlueprintFail("capture setup","private render setup is unsupported or incomplete; the owned Actor will be retired");
        else BlueprintQuarantine("private render setup failed with uncertain resource ownership or world state");
        return;
    }
    BlueprintRetirementOperations operations{this};
    if (!blueprintFence.Rearm() || !blueprintFence.Prepare(operations)) { BlueprintQuarantine("capture_retirement_fence_preparation_uncertain"); return; }
    blueprint.Current=BlueprintPhase::Prepared; blueprint.PreparedTick=blueprintTick;
}
void ServiceDeferredBlueprint()
{
    if (!deferredBlueprint.Pending || blueprint.Active || blueprint.Current==BlueprintPhase::Quarantined) return;
    auto& pending=deferredBlueprint;
    if (!MiniMapMap::GameplayReady() || reinterpret_cast<uintptr_t>(state.SessionWorld)!=pending.SessionToken ||
        MiniMapMap::GetWorld()!=state.SessionWorld || state.Generation!=pending.Generation)
    { DiscardDeferredBlueprint("session or gameplay world generation stale"); return; }
    const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
    if (!snapshot || !snapshot->Complete) return; // incomplete publication does not prove removal
    if (snapshot->Generation!=pending.Identity.Generation)
    { DiscardDeferredBlueprint("source generation stale"); return; }
    if (!QuietGC()) return; // silent while pending; no object access
    if (pending.SourcePublication!=snapshot->PublishedAtSeconds)
    { pending.SourcePublication=snapshot->PublishedAtSeconds; pending.SourceCursor=0; }
    bool current=false;
    size_t visits=0;
    // CPU source reconciliation is sliced too: at most 128 copied records per tick.
    while (pending.SourceCursor<snapshot->Records.size() && visits++<128)
    {
        const auto& source=snapshot->Records[pending.SourceCursor++];
        if (source.BuildingId!=pending.Building) continue;
        auto copied=MiniMapSpriteCandidates::Copy(source);
        copied.Identity.Representation=pending.Identity.Representation;
        if (copied.DefinitionValid && copied.VisualInputsValid && copied.Identity==pending.Identity &&
            source.CanonicalBlueprint && *source.CanonicalBlueprint==pending.ClassPath) { current=true; break; }
    }
    if (!current)
    {
        if (pending.SourceCursor>=snapshot->Records.size()) DiscardDeferredBlueprint("source identity or Blueprint path stale");
        return;
    }
    Candidate candidate;
    candidate.Identity=pending.Identity; candidate.ActorObject=pending.ActorObject;
    candidate.ActorName=pending.ActorName; candidate.ActorNumber=pending.ActorNumber; candidate.Tier=pending.Tier;
    candidate.ClassificationObserved=pending.ClassificationObserved;
    candidate.Classification.ExcludeFromMiniMap=pending.Exclusion;
    candidate.CanonicalBlueprint=std::make_shared<const std::string>(pending.ClassPath);
    const auto id=pending.Id; const auto building=pending.Building;
    deferredBlueprint={};
    // Offer repeats the admission GC gate, then resolves the path afresh. No cached class.
    OfferBlueprint(candidate,0,building,state.SessionWorld,true,id);
}
void ServiceBlueprint()
{
    if (!state.NativePrerequisitesVerified || !MiniMapNative::Get() || !MiniMapNative::Get()->player.isGameThread || !MiniMapNative::Get()->player.isGameThread()) { BlueprintQuarantine("service_off_game_thread"); return; }
    ++blueprintTick;
    ServiceDeferredBlueprint();
    if (!blueprint.Active || blueprint.Current==BlueprintPhase::Quarantined) return;
    if (!QuietGC() || !Unchanged() || !PrivateState(true)) { BlueprintQuarantine("service_native_lifetime_or_world_baseline_changed"); return; }
    if (blueprint.Current==BlueprintPhase::Retiring)
    {
        BlueprintRetirementOperations operations{this};
        if (!operations.Available()) { BlueprintQuarantine("capture_drain_renderer_or_fence_unavailable"); return; }
        if (!blueprintFence.Poll(operations)) return; // existing nonblocking completion opportunity, no timer
        if (blueprint.Job)
        {
            if (!blueprint.Committed)
            {
                if (!MiniMapReadbackNative::DiscardExternalBeforeCapture(blueprint.Job)) { BlueprintQuarantine("C0_only_job_discard_failed"); return; }
            }
            else
            {
                if (MiniMapReadbackNative::ExternalRejectedWithoutReadback(blueprint.Job) &&
                    !MiniMapReadbackNative::CompleteExternalCaptureRetirement(blueprint.Job))
                { BlueprintQuarantine("empty_rejected_job_completion_failed"); return; }
                if (!MiniMapReadbackNative::DestroyIfRetired(blueprint.Job)) { BlueprintQuarantine("readback_owner_not_retired_after_capture_drain"); return; }
            }
        }
        if (!blueprintFence.Release(operations)) { BlueprintQuarantine("capture_drain_fence_release_uncertain"); return; }
        CompleteBlueprintRetirement(); return;
    }
    if (blueprint.Current==BlueprintPhase::Prepared || blueprint.Current==BlueprintPhase::C0)
    {
        if (blueprintTick<=blueprint.PreparedTick ||
            (blueprint.Current==BlueprintPhase::C0 && blueprintTick-blueprint.C0Tick<10)) return;
        if (!BlueprintActorState(true) || !CheckOwned(blueprint.Capture) || !CheckOwned(blueprint.Target))
        { BlueprintQuarantine("capture_submission_actor_or_resource_ownership_invalid"); return; }
        for (size_t i=0;i<blueprint.MetadataCount;++i)
            if (!CheckOwned(blueprint.Metadata[i])) { BlueprintQuarantine("capture_class_metadata_identity_invalid"); return; }
        if (blueprint.Current==BlueprintPhase::Prepared)
        {
            if (!MiniMapBuildingSprites::StartCapture(ownerLease))
            { BlueprintFail("capture admission","session capture reservation is no longer current"); return; }
            blueprint.CaptureUsed=true; // before irreversible void dispatch; no premature destruction
            if (!GuardedCaptureScene(reinterpret_cast<SDK::USceneCaptureComponent2D*>(blueprint.Capture.Object.Bits)))
            { BlueprintQuarantine("C0_CaptureScene_invocation_uncertain"); return; }
            blueprint.C0Tick=blueprintTick; blueprint.Current=BlueprintPhase::C0;
            return;
        }
        if (!MiniMapReadbackNative::MarkExternalCaptureCommitted(blueprint.Job))
        { BlueprintQuarantine("C10_readback_job_commit_uncertain"); return; }
        blueprint.Committed=true;
        if (!GuardedCaptureScene(reinterpret_cast<SDK::USceneCaptureComponent2D*>(blueprint.Capture.Object.Bits)))
        { BlueprintQuarantine("C10_CaptureScene_invocation_uncertain"); return; }
        ++totals.Captured;
        blueprint.Current=BlueprintPhase::Readback;
        MiniMapReadbackNative::Service(blueprint.Job,blueprintTick); return;
    }
    if (blueprint.Current==BlueprintPhase::Readback)
    {
        MiniMapReadbackNative::Service(blueprint.Job,blueprintTick);
        const auto report=MiniMapReadbackNative::Inspect(blueprint.Job);
        if (report.Quarantined)
        {
            if (MiniMapReadbackNative::ExternalRejectedWithoutReadback(blueprint.Job))
            { blueprint.Failed=true; blueprint.Failure="readback_rejected_before_native_enqueue"; ++totals.CaptureFailed; BlueprintLog("GPU_readback",false,blueprint.Failure); BeginBlueprintRetirement(); }
            else BlueprintQuarantine(report.Failure?report.Failure:"GPU_readback_ownership_uncertain");
            return;
        }
        if (report.Current!=MiniMapReadbackNative::Phase::CleanupComplete || report.Callables || report.Outstanding) return;
        if (report.Result!=MiniMapReadbackNative::Outcome::Verified)
            BlueprintLog("GPU_readback",false,report.Failure?report.Failure:"readback_failed");
        if (report.Result==MiniMapReadbackNative::Outcome::Verified)
        {
            try { ExportBlueprintReadback(); }
            catch (...) { blueprint.Failed=true; blueprint.Failure="CPU_image_processing_or_output_exception"; ++totals.CaptureFailed; BlueprintLog("image_validation",false,blueprint.Failure); }
        }
        else { blueprint.Failed=true; blueprint.Failure=report.Failure?report.Failure:"readback_failed"; ++totals.CaptureFailed; }
        BeginBlueprintRetirement();
    }
}
