// Included inside PrivateCaptureWorld::Implementation. One in-flight candidate,
// serviced only by BuildingSpriteCapture's existing guarded game-thread tick.
// All render callbacks belong to ReadbackNativeAdapter and carry Job*, never Actor*.
using BN = SDK::UObject;
using Class = SDK::UClass;
using Function = SDK::UFunction;
// Runtime inventories are dynamically sized and bounded only to keep hostile or
// corrupted TArrays from causing unbounded reads/allocations.
static constexpr size_t ComponentLimit = 4096;
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
    std::vector<Reference> Components;
    size_t ComponentCount{}, DrawableCount{};
    std::vector<uintptr_t> Drawables;
    std::array<Reference, 96> Metadata{};
    size_t MetadataCount{};
    Function *Begin{}, *Finish{}, *Enumerate{}, *Bounds{},
        *ShowOnly{}, *ShowFlags{}, *Camera{}, *CaptureScene{};
    uintptr_t Statics{}, Kismet{}, SelectedClass{};
    uint64_t LevelHash{};
    int32_t LevelCount{};
    SDK::FVector Center{}, Extent{};
    MiniMapReadbackNative::Job* Job{};
    std::vector<uint8_t> Production, PrivateImage;
} blueprint;
uint64_t nextBlueprintId{}, blueprintTick{};
std::unordered_set<std::string> attemptedBlueprints; // session-owned, bounded; no retry queue
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
    blueprint.Current = BlueprintPhase::Quarantined;
    if (blueprint.Job) MiniMapReadbackNative::Cancel(blueprint.Job);
    Quarantine(reason); // prevents world cleanup and future access to dependent pointers
}
void ResetBlueprintSession()
{
    // Arm has excluded active/quarantined world ownership before reaching here.
    if (blueprint.Active || blueprint.Current == BlueprintPhase::Quarantined) return;
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
struct ParameterPart
{
    int Offset, Size;
    SDK::EClassCastFlags Kind;
    const wchar_t* Type = nullptr;
    bool Return = false;
};
Function* BlueprintFunction(const wchar_t* ownerPath, const wchar_t* name, size_t capacity,
    bool isStatic, std::initializer_list<ParameterPart> parts)
{
    auto* owner = BlueprintClass(ownerPath);
    auto* functionClass = BlueprintClass(L"/Script/CoreUObject.Function");
    if (!owner || !functionClass) return nullptr;
    auto* object = FindBlueprintObject(name, functionClass, owner, true);
    Identity id;
    if (!object || !Identify(reinterpret_cast<uintptr_t>(object), id, sizeof(Function))) return nullptr;
    auto* fn = static_cast<Function*>(object);
    uint8_t count{}; uint16_t frame{};
    if (fn->Class != functionClass || fn->Outer != owner || !fn->ExecFunction ||
        !(uint32_t(fn->FunctionFlags) & 0x400) || bool(uint32_t(fn->FunctionFlags) & 0x2000) != isStatic ||
        !Read(reinterpret_cast<uintptr_t>(fn) + 0xb4, count) || count != parts.size() ||
        !Read(reinterpret_cast<uintptr_t>(fn) + 0xb6, frame) || frame > capacity) return nullptr;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<void*>(fn->ExecFunction), &page, sizeof(page)) ||
        page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(page.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return nullptr;
    unsigned seen{}; size_t visited{};
    for (auto* field = fn->ChildProperties; field; field = field->Next)
    {
        if (++visited > parts.size() || !N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(field), sizeof(SDK::FProperty), 8)) return nullptr;
        auto* prop = reinterpret_cast<SDK::FProperty*>(field);
        if (!N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(prop->ClassPrivate), 0x18, 8) ||
            prop->ArrayDim != 1 || !(prop->PropertyFlags & 0x80)) return nullptr;
        uintptr_t owning{}; std::memcpy(&owning, &prop->Owner, 8);
        if ((owning & ~uintptr_t(1)) != reinterpret_cast<uintptr_t>(fn) || !(owning & 1)) return nullptr;
        bool matched = false; size_t index{};
        for (const auto& part : parts)
        {
            if (prop->Offset != part.Offset) { ++index; continue; }
            const auto kind = prop->ClassPrivate->CastFlags;
            const bool enumeration = part.Kind == SDK::EClassCastFlags::EnumProperty;
            if ((seen & (1u << index)) || prop->ElementSize != part.Size || part.Offset < 0 ||
                size_t(part.Offset + part.Size) > frame || bool(prop->PropertyFlags & 0x400) != part.Return ||
                (!(kind & uint64_t(part.Kind)) && !(enumeration && (kind & uint64_t(SDK::EClassCastFlags::ByteProperty))))) return nullptr;
            if (part.Type)
            {
                auto* expected = FindBlueprintObject(part.Type);
                uintptr_t actual{};
                if (!expected || !Read(reinterpret_cast<uintptr_t>(prop) + 0x70, actual)) return nullptr;
                if (part.Kind == SDK::EClassCastFlags::ArrayProperty)
                {
                    auto* inner=reinterpret_cast<SDK::FProperty*>(actual);
                    if (!N::IsMemoryRangeReadable(actual,sizeof(SDK::FProperty),8) ||
                        !N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(inner->ClassPrivate),0x18,8) || inner->ArrayDim!=1) return nullptr;
                    const bool componentArray=std::wcscmp(part.Type,L"/Script/Engine.ActorComponent")==0;
                    const auto innerKind=componentArray?SDK::EClassCastFlags::ObjectProperty:SDK::EClassCastFlags::StructProperty;
                    if (!(inner->ClassPrivate->CastFlags & uint64_t(innerKind)) || inner->ElementSize!=(componentArray?8:0x18) ||
                        !Read(actual+0x70,actual)) return nullptr;
                }
                // FClassProperty has PropertyClass at+70 and MetaClass at+78.
                if (part.Kind == SDK::EClassCastFlags::ClassProperty && !Read(reinterpret_cast<uintptr_t>(prop) + 0x78, actual)) return nullptr;
                if (enumeration && (kind & uint64_t(SDK::EClassCastFlags::EnumProperty)))
                {
                    if (!N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(prop), sizeof(SDK::FEnumProperty), 8)) return nullptr;
                    actual = reinterpret_cast<uintptr_t>(reinterpret_cast<SDK::FEnumProperty*>(prop)->Enum);
                }
                if (actual != reinterpret_cast<uintptr_t>(expected)) return nullptr;
            }
            if (part.Kind == SDK::EClassCastFlags::BoolProperty)
            {
                auto* b = reinterpret_cast<SDK::FBoolProperty*>(prop);
                if (!N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(b), sizeof(*b), 8) ||
                    b->FieldSize != 1 || b->ByteOffset || b->FieldMask != 0xff || !b->ByteMask) return nullptr;
            }
            seen |= 1u << index; matched = true; break;
        }
        if (!matched) return nullptr;
    }
    if (seen != ((1u << parts.size()) - 1)) return nullptr;
    return static_cast<Function*>(KeepBlueprintMetadata(fn));
}
// SEH boundary has no destructible automatic C++ objects. Access violations are
// reported separately because they do not prove that an invocation was harmless.
bool BlueprintCall(BN* receiver, Function* fn, void* params, bool* accessViolation = nullptr) noexcept
{
    if (accessViolation) *accessViolation=false;
    __try
    {
        auto* walker = GetHooks() ? GetHooks()->ObjectWalker : nullptr;
        return walker && walker->InvokeResolvedUFunction(receiver, fn, params);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { blueprint.NativeAccessViolation=true; if (accessViolation) *accessViolation=true; return false; }
}
struct PointerArray { uintptr_t Data{}; int32_t Count{}, Capacity{}; };
bool BlueprintArray(uintptr_t address, PointerArray& out, size_t limit = ComponentLimit)
{
    return Read(address, out) && out.Count >= 0 && out.Capacity >= out.Count && size_t(out.Capacity) <= limit &&
        (!out.Count || (out.Data && N::IsMemoryRangeReadable(out.Data, size_t(out.Count) * 8, 8)));
}
bool BlueprintLevel(uint64_t& hash, int32_t& count)
{
    PointerArray a;
    if (!CheckOwned(state.PersistentLevel) || !BlueprintArray(state.PersistentLevel.Object.Bits + 0xa0, a, 4096)) return false;
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
        !BlueprintArray(state.PersistentLevel.Object.Bits+0xa0,actors,4096)) return false;
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
bool ResolveBlueprintFunctions()
{
    using K = SDK::EClassCastFlags;
    static_assert(sizeof(SDK::Params::GameplayStatics_BeginDeferredActorSpawnFromClass) == 0x90);
    static_assert(sizeof(SDK::Params::GameplayStatics_FinishSpawningActor) == 0x80);
    static_assert(offsetof(SDK::Params::GameplayStatics_BeginDeferredActorSpawnFromClass, ReturnValue) == 0x88);
    static_assert(offsetof(SDK::Params::GameplayStatics_FinishSpawningActor, ReturnValue) == 0x78);
    static_assert(offsetof(SDK::ULevel, OwningWorld) == 0xc0);
    auto* statics = BlueprintClass(L"/Script/Engine.GameplayStatics");
    auto* kismet = BlueprintClass(L"/Script/Engine.KismetSystemLibrary");
    if (!statics || !kismet || !KeepBlueprintMetadata(statics->ClassDefaultObject) ||
        !KeepBlueprintMetadata(kismet->ClassDefaultObject) || statics->ClassDefaultObject->Class!=statics ||
        kismet->ClassDefaultObject->Class!=kismet) return false;
    blueprint.Statics = reinterpret_cast<uintptr_t>(statics->ClassDefaultObject);
    blueprint.Kismet = reinterpret_cast<uintptr_t>(kismet->ClassDefaultObject);
    blueprint.Begin = BlueprintFunction(L"/Script/Engine.GameplayStatics", L"BeginDeferredActorSpawnFromClass", 0x90, true, {
        {0,8,K::ObjectProperty,L"/Script/CoreUObject.Object"}, {8,8,K::ClassProperty,L"/Script/Engine.Actor"},
        {0x10,0x60,K::StructProperty,L"/Script/CoreUObject.Transform"},
        {0x70,1,K::EnumProperty,L"/Script/Engine.ESpawnActorCollisionHandlingMethod"},
        {0x78,8,K::ObjectProperty,L"/Script/Engine.Actor"}, {0x80,1,K::EnumProperty,L"/Script/Engine.ESpawnActorScaleMethod"},
        {0x88,8,K::ObjectProperty,L"/Script/Engine.Actor",true}});
    blueprint.Finish = BlueprintFunction(L"/Script/Engine.GameplayStatics", L"FinishSpawningActor", 0x80, true, {
        {0,8,K::ObjectProperty,L"/Script/Engine.Actor"}, {0x10,0x60,K::StructProperty,L"/Script/CoreUObject.Transform"},
        {0x70,1,K::EnumProperty,L"/Script/Engine.ESpawnActorScaleMethod"}, {0x78,8,K::ObjectProperty,L"/Script/Engine.Actor",true}});
    blueprint.Enumerate = BlueprintFunction(L"/Script/Engine.Actor", L"K2_GetComponentsByClass", 0x18, false, {
        {0,8,K::ClassProperty,L"/Script/Engine.ActorComponent"}, {8,0x10,K::ArrayProperty,L"/Script/Engine.ActorComponent",true}});
    blueprint.Bounds = BlueprintFunction(L"/Script/Engine.KismetSystemLibrary", L"GetComponentBounds", 0x40, true, {
        {0,8,K::ObjectProperty,L"/Script/Engine.SceneComponent"}, {8,0x18,K::StructProperty,L"/Script/CoreUObject.Vector"},
        {0x20,0x18,K::StructProperty,L"/Script/CoreUObject.Vector"}, {0x38,4,K::FloatProperty}});
    blueprint.ShowOnly = BlueprintFunction(L"/Script/Engine.SceneCaptureComponent", L"ShowOnlyComponent", 8, false, {
        {0,8,K::ObjectProperty,L"/Script/Engine.PrimitiveComponent"}});
    blueprint.ShowFlags = BlueprintFunction(L"/Script/Engine.SceneCaptureComponent", L"SetShowFlagSettings", 0x10, false, {
        {0,0x10,K::ArrayProperty,L"/Script/Engine.EngineShowFlagsSetting"}});
    blueprint.Camera = BlueprintFunction(L"/Script/Engine.SceneComponent", L"K2_SetWorldLocationAndRotation", 0x140, false, {
        {0,0x18,K::StructProperty,L"/Script/CoreUObject.Vector"}, {0x18,0x18,K::StructProperty,L"/Script/CoreUObject.Rotator"},
        {0x30,1,K::BoolProperty}, {0x38,0x100,K::StructProperty,L"/Script/Engine.HitResult"}, {0x138,1,K::BoolProperty}});
    blueprint.CaptureScene = BlueprintFunction(L"/Script/Engine.SceneCaptureComponent2D", L"CaptureScene", 0, false, {});
    return blueprint.Begin && blueprint.Finish && blueprint.Enumerate && blueprint.Bounds &&
        blueprint.ShowOnly && blueprint.ShowFlags && blueprint.Camera && blueprint.CaptureScene;
}
// Classify runtime components through Unreal's native superclass chain.
int BlueprintComponentKind(uintptr_t component)
{
    Identity id; uintptr_t type{};
    if (!Identify(component, id, sizeof(SDK::UActorComponent)) || !Read(component + 0x10, type)) return -1;
    const auto componentType=type;
    auto* sceneClass=BlueprintClass(L"/Script/Engine.SceneComponent");
    auto* primitiveClass=BlueprintClass(L"/Script/Engine.PrimitiveComponent");
    if (!sceneClass || !primitiveClass) return -1;
    bool scene=false, primitive=false;
    for (size_t depth=0; type && depth<128; ++depth)
    {
        if (!Identify(type,id,sizeof(Class))) return -1;
        scene |= type==reinterpret_cast<uintptr_t>(sceneClass);
        primitive |= type==reinterpret_cast<uintptr_t>(primitiveClass);
        if (scene && primitive) break;
        uintptr_t parent{};
        if (!Read(type+0x40,parent)) return -1;
        if (parent==type) break;
        if (!parent) break;
        type=parent;
    }
    auto* actual=reinterpret_cast<Class*>(componentType);
    if (!actual || actual->Size<int32_t(sizeof(SDK::UActorComponent)) || actual->Size>0x100000 ||
        !Identify(component,id,size_t(actual->Size))) return -1;
    return primitive && scene ? 2 : scene ? 1 : primitive ? -1 : 0;
}
bool AdmitBlueprint(Class* type)
{
    auto* generated=BlueprintClass(L"/Script/Engine.BlueprintGeneratedClass");
    auto* base=BlueprintClass(L"/Script/Chimera.CrBuildingActorBase");
    auto* drone=BlueprintClass(L"/Script/Chimera.CrDroneSupplySystemBuildingBase");
    auto* crafter=BlueprintClass(L"/Script/Chimera.CrCrafter");
    if (!generated || !base || !drone || !crafter ||
        base->Size!=sizeof(SDK::ACrBuildingActorBase) || drone->Size!=sizeof(SDK::ACrDroneSupplySystemBuildingBase) ||
        crafter->Size!=sizeof(SDK::ACrCrafter))
    { blueprint.Failure="native building class metadata is unavailable"; return false; }
    auto* current=type; bool nativeFound=false;
    for (size_t depth=0; current && depth<128; ++depth)
    {
        Identity id;
        if (!Identify(reinterpret_cast<uintptr_t>(current),id,sizeof(Class)) || !KeepBlueprintMetadata(current))
        { blueprint.Failure="Blueprint class identity could not be verified"; return false; }
        if (current->Class!=generated)
        {
            nativeFound=current==base || current==drone || current==crafter;
            break;
        }
        if (!Identify(reinterpret_cast<uintptr_t>(current),id,sizeof(SDK::UBlueprintGeneratedClass)))
        { blueprint.Failure="Blueprint class extent could not be verified"; return false; }
        current=static_cast<Class*>(current->SuperStruct);
    }
    if (!nativeFound || !current || !type || type->Size<current->Size || type->Size>65536)
    { blueprint.Failure="Blueprint does not derive from a supported native building class"; return false; }
    Identity cdoId;
    if (!type->ClassDefaultObject || type->ClassDefaultObject->Class!=type ||
        !Identify(reinterpret_cast<uintptr_t>(type->ClassDefaultObject),cdoId,size_t(type->Size)) ||
        !KeepBlueprintMetadata(type->ClassDefaultObject))
    { blueprint.Failure="Blueprint class default object identity could not be verified"; return false; }
    return true;
}
bool BlueprintBindings()
{
    if (!MiniMapReadbackNative::MatchesCaptureRetirementBuild()) return false;
    const auto& entry=actorEntries[0];
    std::array<uint8_t,16> bytes{}; MEMORY_BASIC_INFORMATION p{};
    return N::ReadMemory(state.Base+entry.Rva,bytes.data(),bytes.size()) && bytes==entry.Bytes &&
        VirtualQuery(reinterpret_cast<void*>(state.Base+entry.Rva),&p,sizeof(p)) && p.State==MEM_COMMIT &&
        !(p.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (p.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}

bool InspectBlueprint()
{
    auto* actor=reinterpret_cast<SDK::AActor*>(blueprint.Actor.Object.Bits);
    auto* componentClass=BlueprintClass(L"/Script/Engine.ActorComponent");
    auto freeFn=MiniMapReadbackNative::GetBindings().Free;
    if (!componentClass || !freeFn) { blueprint.Failure="component enumeration support is unavailable"; return false; }
    SDK::Params::Actor_K2_GetComponentsByClass params{};
    std::memcpy(&params.ComponentClass,&componentClass,8);
    if (!BlueprintCall(actor,blueprint.Enumerate,&params)) { blueprint.Failure="Unreal component enumeration failed"; return false; }
    PointerArray list;
    if (!BlueprintArray(reinterpret_cast<uintptr_t>(&params.ReturnValue),list))
    { blueprint.Failure="runtime component list is unreadable"; return false; }
    bool valid=true;
    for (int32_t i=0;i<list.Count;++i)
    {
        uintptr_t component{};
        if (!Read(list.Data+size_t(i)*8,component) || !component || blueprint.Components.size()>=ComponentLimit)
        { valid=false; break; }
        for (const auto& existing:blueprint.Components) if (existing.Object.Bits==component) valid=false;
        if (!valid) break;
        Reference reference;
        if (!Hold(component,reference)) { valid=false; break; }
        blueprint.Components.push_back(reference);
    }
    if (list.Data) reinterpret_cast<void(*)(void*)>(freeFn)(reinterpret_cast<void*>(list.Data));
    if (!valid) { blueprint.Failure="runtime component identity or ownership could not be established"; return false; }
    blueprint.ComponentCount=blueprint.Components.size();

    bool haveBounds=false;
    SDK::FVector minimum{},maximum{};
    size_t renderable{}, skipped{};
    for (size_t i=0;i<blueprint.Components.size();++i)
    {
        auto& reference=blueprint.Components[i];
        const auto bits=reference.Object.Bits;
        uintptr_t outer{},world{}; uint8_t registered{};
        const int kind=BlueprintComponentKind(bits);
        if (!CheckOwned(reference) || kind<0 || !Read(bits+0x20,outer) || outer!=blueprint.Actor.Object.Bits ||
            !Read(bits+0xa8,world) || (world && world!=state.Private.Object.Bits) || !Read(bits+0x94,registered))
        { blueprint.Failure="runtime component identity, owner, or world could not be verified"; return false; }
        if (kind==0)
        {
            ++skipped;
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component inspection outcome=skipped component=%d reason=component is not a scene component",
                (unsigned long long)blueprint.Id,reference.Object.Index);
            continue;
        }
        auto* scene=reinterpret_cast<SDK::USceneComponent*>(bits);
        if (kind==1)
        {
            ++skipped;
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component inspection outcome=skipped component=%d reason=scene component is not renderable",
                (unsigned long long)blueprint.Id,reference.Object.Index);
            continue;
        }
        if (!scene->bVisible || scene->bHiddenInGame || (registered&3)!=3 || world!=state.Private.Object.Bits)
        {
            ++skipped; blueprint.VisualIncomplete=true;
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component inspection outcome=skipped component=%d reason=primitive component is hidden, unregistered, or outside the private world",
                (unsigned long long)blueprint.Id,reference.Object.Index);
            continue;
        }
        SDK::Params::KismetSystemLibrary_GetComponentBounds bounds{};
        bounds.Component=reinterpret_cast<SDK::USceneComponent*>(bits);
        bool accessViolation=false;
        if (!BlueprintCall(reinterpret_cast<BN*>(blueprint.Kismet),blueprint.Bounds,&bounds,&accessViolation))
        {
            if (accessViolation) { blueprint.Failure="access violation while querying primitive bounds"; return false; }
            ++skipped; blueprint.VisualIncomplete=true;
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component bounds outcome=skipped component=%d reason=Unreal could not provide primitive bounds",
                (unsigned long long)blueprint.Id,reference.Object.Index);
            continue;
        }
        const auto& origin=bounds.Origin; const auto& extent=bounds.BoxExtent;
        if (!std::isfinite(origin.X) || !std::isfinite(origin.Y) || !std::isfinite(origin.Z) ||
            !std::isfinite(extent.X) || !std::isfinite(extent.Y) || !std::isfinite(extent.Z) ||
            extent.X<0 || extent.Y<0 || extent.Z<0)
        {
            ++skipped; blueprint.VisualIncomplete=true;
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component bounds outcome=skipped component=%d reason=primitive returned unusable bounds",
                (unsigned long long)blueprint.Id,reference.Object.Index);
            continue;
        }
        if (extent.X<=0 || extent.Y<=0)
        {
            ++skipped; blueprint.VisualIncomplete=true;
            LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component bounds outcome=skipped component=%d reason=primitive has no two-dimensional renderable bounds",
                (unsigned long long)blueprint.Id,reference.Object.Index);
            continue;
        }
        const SDK::FVector lo(origin.X-extent.X,origin.Y-extent.Y,origin.Z-extent.Z);
        const SDK::FVector hi(origin.X+extent.X,origin.Y+extent.Y,origin.Z+extent.Z);
        if (!haveBounds) { minimum=lo; maximum=hi; haveBounds=true; }
        else
        {
            minimum.X=(std::min)(minimum.X,lo.X); minimum.Y=(std::min)(minimum.Y,lo.Y); minimum.Z=(std::min)(minimum.Z,lo.Z);
            maximum.X=(std::max)(maximum.X,hi.X); maximum.Y=(std::max)(maximum.Y,hi.Y); maximum.Z=(std::max)(maximum.Z,hi.Z);
        }
        blueprint.Drawables.push_back(bits); ++blueprint.DrawableCount; ++renderable;
    }
    LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=component inventory outcome=observed total=%zu renderable=%zu skipped=%zu bounds=actual primitive bounds",
        (unsigned long long)blueprint.Id,blueprint.Components.size(),renderable,skipped);
    if (!haveBounds || !blueprint.DrawableCount)
    { blueprint.Failure="no visible primitive component provided renderable bounds"; return true; }
    blueprint.Center=SDK::FVector((minimum.X+maximum.X)*0.5,(minimum.Y+maximum.Y)*0.5,(minimum.Z+maximum.Z)*0.5);
    blueprint.Extent=SDK::FVector((maximum.X-minimum.X)*0.5,(maximum.Y-minimum.Y)*0.5,(maximum.Z-minimum.Z)*0.5);
    return BlueprintActorState(true);
}// All opaque native factory/registration calls are isolated here. The bindings
// are already verified by the production worker; no new executable patch/scan.
bool BlueprintNative(unsigned operation, uintptr_t object, uintptr_t argument = 0, uintptr_t* result = nullptr) noexcept
{
    __try
    {
        const auto& b = MiniMapSmelterCapture::GetBindings();
        switch (operation)
        {
        case 0: {
            const Class* type = reinterpret_cast<Class*>(argument);
            struct Subclass { const Class* Type; } value{type};
            *result = reinterpret_cast<uintptr_t(*)(const Subclass*,uintptr_t)>(b.Spawn)(&value,object);
            break;
        }
        case 1: reinterpret_cast<void(*)(uintptr_t,uint32_t)>(b.Flags)(object,0x40); break;
        case 2: reinterpret_cast<void(*)(uintptr_t,uintptr_t,void*)>(b.Register)(object,argument,nullptr); break;
        case 3: reinterpret_cast<void(*)(uintptr_t)>(b.Unregister)(object); break;
        case 4: *result = reinterpret_cast<bool(*)(uintptr_t,bool,bool)>(state.Base + actorEntries[0].Rva)(object,false,false); break;
        case 5: reinterpret_cast<void(*)(uintptr_t,bool)>(b.DestroyComponent)(object,false); break;
        case 6: reinterpret_cast<void(*)(uintptr_t,uint32_t,uint32_t,uint8_t,bool)>(b.InitTarget)(object,256,256,10,true); break;
        case 7: *result = reinterpret_cast<uintptr_t(*)(uintptr_t)>(b.TargetResource)(object); break;
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
    SDK::Params::SceneCaptureComponent_SetShowFlagSettings show{};
    show.InShowFlagSettings=SDK::TArray<SDK::FEngineShowFlagsSetting>(flags,11,11);
    if (!BlueprintCall(capture,blueprint.ShowFlags,&show)) return false;
    target->bAutoGenerateMips=false; target->bSupportsUAV=false;
    uint32_t targetFlags{};
    if (!Read(blueprint.Target.Object.Bits+0x144,targetFlags)) return false;
    targetFlags &= ~uint32_t(1);
    std::memcpy(reinterpret_cast<uint8_t*>(target)+0x144,&targetFlags,4);
    target->ClearColor=SDK::FLinearColor(0,0,0,1);
    if (!BlueprintNative(6,blueprint.Target.Object.Bits)) return false;
    capture->TextureTarget=target;
    SDK::Params::SceneComponent_K2_SetWorldLocationAndRotation camera{};
    camera.NewLocation=SDK::FVector(blueprint.Center.X,blueprint.Center.Y,blueprint.Center.Z+blueprint.Extent.Z+1000.0);
    camera.NewRotation=SDK::FRotator(-90,-90,0); camera.bTeleport=true;
    if (!BlueprintCall(capture,blueprint.Camera,&camera) || !BlueprintNative(2,blueprint.Capture.Object.Bits,state.Private.Object.Bits)) return false;
    uint8_t registered{}; uintptr_t world{};
    if (!Read(blueprint.Capture.Object.Bits+0x94,registered) || !(registered&1) ||
        !Read(blueprint.Capture.Object.Bits+0xa8,world) || world!=state.Private.Object.Bits) return false;
    for (size_t i=0;i<blueprint.DrawableCount;++i)
    {
        SDK::Params::SceneCaptureComponent_ShowOnlyComponent member{};
        member.InComponent=reinterpret_cast<SDK::UPrimitiveComponent*>(blueprint.Drawables[i]);
        if (!BlueprintCall(capture,blueprint.ShowOnly,&member)) return false;
    }
    if (!MiniMapBuildingRepresentation::ValidateCaptureMembership(blueprint.Capture.Object.Bits,blueprint.Drawables.data(),blueprint.DrawableCount)) return false;
    uintptr_t resource{};
    if (!BlueprintNative(7,blueprint.Target.Object.Bits,0,&resource) || !resource) return false;
    blueprint.Job=MiniMapReadbackNative::CreateExternalJob(reinterpret_cast<void*>(resource),256,256);
    if (!blueprint.Job) return false;
    BlueprintLog("renderer_scene",true,"private_scene_registered_components_show_only_256_RGBA16F_unlit");
    return true;
}

struct BlueprintRetirementOperations
{
    Implementation* Owner;
    bool Available() const
    {
        const auto& b=MiniMapSmelterCapture::GetBindings();
        return Owner->state.GameThread && Owner->state.GameThread() && MiniMapReadbackNative::CaptureRetirementRendererAvailable() &&
            b.RetirementConstruct && b.RetirementDestroy && b.RetirementBegin && b.RetirementPoll;
    }
    bool Construct(void* storage) const { reinterpret_cast<void(*)(void*)>(MiniMapSmelterCapture::GetBindings().RetirementConstruct)(storage); return true; }
    bool Begin(void* storage) const { reinterpret_cast<void(*)(void*,int32_t)>(MiniMapSmelterCapture::GetBindings().RetirementBegin)(storage,1); return true; }
    bool Poll(void* storage) const { return reinterpret_cast<bool(*)(const void*)>(MiniMapSmelterCapture::GetBindings().RetirementPoll)(storage); }
    void Destroy(void* storage) const { reinterpret_cast<void(*)(void*)>(MiniMapSmelterCapture::GetBindings().RetirementDestroy)(storage); }
};
// Only callable contiguously after THIS owner's admitted engine destruction.
// It deliberately does not call Identify or modify its live-object policy.
bool ReleaseRetiringOwned(Reference& ref)
{
    if (!ref.Owned || ref.Uncertain || !QuietGC()) return false;
    DefinitionSlots::Slot slot; SlotReader reader; int32_t index{}, count{}; uint32_t flags{};
    const auto bits=ref.Object.Bits;
    if (DefinitionSlots::ReadSlot(reader,state.Array,ref.Object.Index,slot)!=DefinitionSlots::Status::Success || slot.Object!=bits ||
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
    for (size_t i=0;i<blueprint.ComponentCount;++i)
        if (!CheckOwned(blueprint.Components[i])) return false;
    if (blueprint.Actor.Owned)
    {
        // Finished can be false for a deferred Actor if construction wasn't entered.
        uint8_t finished{};
        if (!Read(blueprint.Actor.Object.Bits+0x5c,finished) || !BlueprintActorState(bool(finished&1))) return false;
        uintptr_t destroyed{};
        BlueprintLog("actor_destruction_admission",true,"live_private_uninitialized_actor_no_active_capture");
        if (!BlueprintNative(4,blueprint.Actor.Object.Bits,0,&destroyed) || !destroyed) return false;
        uint8_t a{}, b{};
        if (!Read(blueprint.Actor.Object.Bits+0x5c,a) || (a&2) ||
            !Read(blueprint.Actor.Object.Bits+0x5d,b) || !(b&2) || (b&0x18) ||
            !BlueprintLevelContains(blueprint.Actor.Object.Bits,false)) return false;
        for (size_t i=0;i<blueprint.ComponentCount;++i)
        {
            uint8_t flags{}, init{};
            if (!Read(blueprint.Components[i].Object.Bits+0x94,flags) || (flags&3) ||
                !Read(blueprint.Components[i].Object.Bits+0x97,init) || (init&0x28)) return false;
        }
        BlueprintLog("actor_destruction",true,"normal_return_and_level_removal_component_unregistration_verified_GC_unobserved");
        // No return/yield/callback servicing between destruction and acquired-reference release.
        for (size_t i=blueprint.ComponentCount;i>0;--i)
            if (!ReleaseRetiringOwned(blueprint.Components[i-1])) return false;
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
    BlueprintLog("reference_release",true,"exact_acquired_actor_component_capture_target_metadata_references_released");
    return Unchanged() && PrivateState(true) && QuietGC();
}
void CompleteBlueprintRetirement()
{
    if (!RetireBlueprintObjects()) { BlueprintQuarantine("actor_or_resource_retirement_postcondition_or_release_uncertain"); return; }
    ++totals.Retired;
    // Remove callable/borrowed native pointers after the exact reference ledger
    // has been consumed. Only copied CPU images/identity remain for comparison.
    blueprint.Actor={}; blueprint.Capture={}; blueprint.Target={};
    blueprint.Components={}; blueprint.Metadata={}; blueprint.Drawables={};
    blueprint.ComponentCount=blueprint.MetadataCount=blueprint.DrawableCount=0;
    blueprint.Begin=blueprint.Finish=blueprint.Enumerate=blueprint.Bounds=nullptr;
    blueprint.ShowOnly=blueprint.ShowFlags=blueprint.Camera=blueprint.CaptureScene=nullptr;
    blueprint.Statics=blueprint.Kismet=blueprint.SelectedClass=0;
    blueprint.ResourceCreationUncertain=blueprint.NativeAccessViolation=blueprint.ActorSpawnAttempted=false;
    blueprint.Active=false; blueprint.Current=BlueprintPhase::Idle;
    BlueprintLog("final",!blueprint.Failed,blueprint.Failed?blueprint.Failure:blueprint.VisualIncomplete?"rendered_partial_visual_dependencies_and_logically_retired":"rendered_and_logically_retired_UObject_GC_and_renderer_retirement_unobserved");
    BlueprintTotals();
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
    BlueprintLog("capture_drain",true,"ordered_RHI_fence_inserted_nonblocking");
}
void BlueprintFail(const char* stage, const char* reason, bool unsupported = false)
{
    if (!blueprint.Failed)
    {
        blueprint.Failed=true; blueprint.Failure=reason;
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
    blueprint.Active=false;
    blueprint.Current=BlueprintPhase::Idle;
    BlueprintTotals();
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
    static const char moduleAnchor{};
    HMODULE module{}; wchar_t path[32768]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleAnchor),&module) || !GetModuleFileNameW(module,path,32768)) return {};
    const auto directory=std::filesystem::path(path).parent_path()/L"MiniMap"/L"Development"/L"PrivateWorld";
    std::error_code error; std::filesystem::create_directories(directory,error);
    if (error) return {};
    const auto name=L"run_"+std::to_wstring(outputRun)+L"_pid_"+std::to_wstring(GetCurrentProcessId())+
        L"_g_"+std::to_wstring(state.Generation)+L"_candidate_"+std::to_wstring(blueprint.Id)+suffix;
    return directory/name;
}
uint64_t outputRun=GetTickCount64();
bool WriteBlueprintImage(const std::vector<uint8_t>& rgba, const wchar_t* suffix)
{
    const auto path=BlueprintOutputPath(suffix);
    if (path.empty()) { BlueprintLog("image_output",false,"module_path_or_directory_unavailable"); return false; }
    const auto result=MiniMapCapturePng::Write(path,256,256,rgba,false);
    const auto readableStage=BlueprintText(result.Stage);
    LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=image output outcome=%s path=%ls reason=%s HRESULT=%08lx production publication=0",
        (unsigned long long)blueprint.Id,result.Ok?"success":"failure",path.c_str(),readableStage.c_str(),(unsigned long)result.Error);
    return result.Ok;
}
void CompareBlueprintImages()
{
    if (blueprint.Production.size()!=256*256*4 || blueprint.PrivateImage.size()!=256*256*4) return;
    uint64_t differing{}, alphaDifference{}, colorDifference{};
    for (size_t i=0;i<blueprint.Production.size();i+=4)
    {
        bool different=false;
        for (size_t c=0;c<4;++c)
        {
            const auto d=std::abs(int(blueprint.Production[i+c])-int(blueprint.PrivateImage[i+c]));
            different |= d!=0;
            if (c==3) alphaDifference+=d; else colorDifference+=d;
        }
        differing+=different;
    }
    LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=comparison outcome=observed differing pixels=%llu mean absolute alpha=%.5f mean absolute RGB=%.5f framing=independent actual bounds lighting=private unlit versus gameplay equivalence=not established",
        (unsigned long long)blueprint.Id,(unsigned long long)differing,double(alphaDifference)/65536.0,double(colorDifference)/(65536.0*3));
    WriteBlueprintImage(blueprint.Production,L"_component_copy.png");
    blueprint.Production.clear();
}
void CompareProduction(const Candidate& candidate, const unsigned char* rgba, size_t bytes)
{
    // CPU-only, bounded single candidate pair. No cache/atlas/publication calls.
    if (!rgba || bytes!=256*256*4 || blueprint.Key!=SessionVisualKey(candidate)) return;
    blueprint.Production.assign(rgba,rgba+bytes);
    CompareBlueprintImages();
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
    BlueprintLog("image_validation",true,blueprint.VisualIncomplete?"valid_pixels_with_incomplete_visual_dependencies":"256x256_finite_foreground_and_background_not_visual_equivalence");
    blueprint.PrivateImage=std::move(image.Rgba);
    if (!WriteBlueprintImage(blueprint.PrivateImage,L"_blueprint.png"))
    { blueprint.Failed=true; blueprint.Failure="development_PNG_write_failed"; }
    CompareBlueprintImages();
}

void OfferBlueprint(const Candidate& candidate, uintptr_t selectedClass, uint32_t building, void* gameplay)
{
    ++totals.Encountered;
    const auto key=SessionVisualKey(candidate);
    if (attemptedBlueprints.contains(key)) return;
    const auto id=++nextBlueprintId;
    if (blueprint.Active || blueprint.Current==BlueprintPhase::Quarantined || state.Current!=Phase::Ready)
    {
        LOG_INFO("MiniMap: private_blueprint_actor id=%llu generation=%llu building=%u candidate=%s step=owner readiness outcome=skipped reason=%s production unchanged=1",
            (unsigned long long)id,(unsigned long long)candidate.Identity.Generation,building,key.c_str(),
            blueprint.Active?"private capture owner is busy":state.Current==Phase::Quarantined?"private capture session is quarantined":"private capture world is not ready");
        return;
    }
    if (attemptedBlueprints.size()>=4096)
    { LOG_WARN("MiniMap: private_blueprint_actor id=%llu step=admission outcome=skipped reason=session candidate inventory reached its 4096 entry memory bound",(unsigned long long)id); return; }
    attemptedBlueprints.insert(key); // terminal for this visual identity in this session
    blueprint={}; blueprint.Active=true; blueprint.Id=id; blueprint.Building=building; blueprint.Key=key;
    blueprint.Failure="Blueprint class identity is incomplete";
    if (!state.GameThread || !state.GameThread())
    { BlueprintSkipBeforeConstruction("candidate was not offered on the Unreal game thread"); return; }
    if (gameplay!=state.SessionWorld)
    { BlueprintQuarantine("candidate gameplay world does not match the private capture session"); return; }
    QuietGCDetail gcDetail;
    if (!QuietGC(&gcDetail))
    { BlueprintSkipBeforeConstruction(gcDetail.Step); return; }
    if (!Unchanged())
    { BlueprintQuarantine("gameplay world identity or baseline changed"); return; }
    if (!PrivateState(true))
    { BlueprintQuarantine("private capture world lifecycle validation failed"); return; }
    BlueprintLog("owner readiness",true,"candidate uses the existing game thread and current session");
    if (candidate.ClassificationObserved && candidate.Classification.ExcludeFromMiniMap!=MiniMapSpriteClassification::Exclusion::Include)
        LOG_INFO("MiniMap: private_blueprint_actor id=%llu generation=%llu building=%u step=candidate policy outcome=info reason=production classification excludes this building; development construction remains eligible",
            (unsigned long long)id,(unsigned long long)candidate.Identity.Generation,building);
    if (candidate.Identity.Spline || candidate.Identity.TintBits!=MiniMapSpriteDiscoveryCore::Key{}.TintBits)
    {
        blueprint.VisualIncomplete=true;
        BlueprintLog("visual customization","observed","candidate has spline or tint customization that this diagnostic capture does not reproduce");
    }
    if (!MiniMapReadbackNative::Preflight()) { BlueprintFail("native ABI","shared GPU readback backend is unavailable",true); return; }
    if (!BlueprintBindings()) { BlueprintFail("native ABI", "native Actor destruction entry or matching module is unavailable",true); return; }
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
    if (!found || reinterpret_cast<uintptr_t>(found)!=selectedClass ||
        !Identify(selectedClass,identity,sizeof(SDK::UBlueprintGeneratedClass)) || found->Class!=generated)
    { BlueprintFail("class resolution","resident class is absent or differs from the selected visual class",true); return; }
    auto* type=static_cast<Class*>(found);
    if (type->Size<int32_t(sizeof(SDK::ACrBuildingActorBase)) || type->Size>65536 || !KeepBlueprintMetadata(type))
    { BlueprintFail("class resolution","Blueprint class extent or retained identity is invalid",true); return; }
    blueprint.SelectedClass=selectedClass;
    LOG_INFO("MiniMap: private_blueprint_actor id=%llu candidate=%s blueprint=%s class index=%d step=class resolution outcome=success source=authored definition and selected visual class",
        (unsigned long long)id,key.c_str(),blueprint.ClassPath.c_str(),identity.Index);
    if (!ResolveBlueprintFunctions()) { BlueprintFail("native ABI","required Unreal reflected functions do not match the native call contract",true); return; }
    if (!AdmitBlueprint(type)) { BlueprintFail("construction admission",blueprint.Failure,true); return; }
    BlueprintLog("native ABI",true,"shipping Actor destruction entry and reflected parameter contracts match");
    if (!BlueprintLevel(blueprint.LevelHash,blueprint.LevelCount))
    { BlueprintQuarantine("private persistent level could not be inspected before spawn"); return; }
    if (!QuietGC()) { BlueprintQuarantine("garbage collection became active before spawn"); return; }
    if (!PrivateState(true)) { BlueprintQuarantine("private capture world failed validation before spawn"); return; }
    if (!Unchanged()) { BlueprintQuarantine("gameplay world baseline changed before spawn"); return; }
    ++totals.Eligible;
    BlueprintLog("construction admission",true,"native building ancestry and bounded object identity verified; Unreal chooses the Blueprint construction path");
    SDK::Params::GameplayStatics_BeginDeferredActorSpawnFromClass begin{};
    begin.WorldContextObject=reinterpret_cast<BN*>(state.Private.Object.Bits);
    std::memcpy(&begin.ActorClass,&type,8);
    begin.SpawnTransform=BlueprintIdentityTransform();
    begin.CollisionHandlingOverride=SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    begin.TransformScaleMethod=SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
    blueprint.ActorSpawnAttempted=true;
    bool spawnAccessViolation=false;
    if (!BlueprintCall(reinterpret_cast<BN*>(blueprint.Statics),blueprint.Begin,&begin,&spawnAccessViolation))
    {
        uint64_t hash{}; int32_t count{};
        if (!spawnAccessViolation && Unchanged() && PrivateState(true) && BlueprintLevel(hash,count) && hash==blueprint.LevelHash && count==blueprint.LevelCount)
        {
            blueprint.ActorSpawnAttempted=false;
            BlueprintFail("deferred spawn","Unreal rejected deferred spawn before creating an Actor");
        }
        else BlueprintQuarantine("deferred spawn raised an access violation or private world ownership cannot be confirmed");
        return;
    }
    if (!begin.ReturnValue)
    {
        uint64_t hash{}; int32_t count{};
        if (!Unchanged() || !PrivateState(true) || !BlueprintLevel(hash,count) || hash!=blueprint.LevelHash || count!=blueprint.LevelCount)
        { BlueprintQuarantine("Unreal returned no Actor and the private or gameplay world baseline changed"); return; }
        blueprint.ActorSpawnAttempted=false;
        BlueprintFail("deferred spawn","Unreal returned no Actor"); return;
    }
    if (!Hold(reinterpret_cast<uintptr_t>(begin.ReturnValue),blueprint.Actor) || !BlueprintActorState(false))
    { ++totals.ConstructionFailed; BlueprintQuarantine("deferred_actor_retention_level_or_initialization_invalid"); return; }
    BlueprintLog("deferred spawn",true,"retained Actor belongs to the private persistent level and has no owner or instigator");
    SDK::Params::GameplayStatics_FinishSpawningActor finish{};
    finish.Actor=begin.ReturnValue; finish.SpawnTransform=begin.SpawnTransform; finish.TransformScaleMethod=begin.TransformScaleMethod;
    if (!BlueprintCall(reinterpret_cast<BN*>(blueprint.Statics),blueprint.Finish,&finish) ||
        finish.ReturnValue!=begin.ReturnValue || !BlueprintActorState(true))
    { ++totals.ConstructionFailed; BlueprintQuarantine("FinishSpawning_return_or_ownership_isolation_uncertain"); return; }
    ++totals.Constructed;
    BlueprintLog("finish spawning",true,"Unreal finished the same Actor in the private world");
    if (!InspectBlueprint())
    {
        BlueprintQuarantine("runtime component ownership could not be established after Actor construction");
        return;
    }
    if (!blueprint.DrawableCount)
    { BlueprintFail("component inspection",blueprint.Failure,true); return; }
    BlueprintLog("isolation",true,"contexts_GameInstance_world_flags_private_level_and_component_ownership_unchanged_identified_gameplay_paths_excluded_not_save_byte_audit");
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
    BlueprintLog("capture_prepared",true,"next_engine_tick_C0_then_C10_no_production_publication");
}
void ServiceBlueprint()
{
    if (!state.GameThread || !state.GameThread()) { BlueprintQuarantine("service_off_game_thread"); return; }
    ++blueprintTick;
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
        BlueprintLog("capture_drain",true,"ordered_RHI_fence_completed_not_independent_renderer_resource_retirement");
        CompleteBlueprintRetirement(); return;
    }
    if (blueprint.Current==BlueprintPhase::Prepared || blueprint.Current==BlueprintPhase::C0)
    {
        if (blueprintTick<=blueprint.PreparedTick ||
            (blueprint.Current==BlueprintPhase::C0 && blueprintTick-blueprint.C0Tick<10)) return;
        if (!BlueprintActorState(true) || !CheckOwned(blueprint.Capture) || !CheckOwned(blueprint.Target) ||
            !MiniMapBuildingRepresentation::ValidateCaptureMembership(blueprint.Capture.Object.Bits,blueprint.Drawables.data(),blueprint.DrawableCount))
        { BlueprintQuarantine("capture_submission_actor_resource_or_membership_invalid"); return; }
        for (size_t i=0;i<blueprint.ComponentCount;++i)
            if (!CheckOwned(blueprint.Components[i])) { BlueprintQuarantine("capture_component_identity_invalid"); return; }
        for (size_t i=0;i<blueprint.MetadataCount;++i)
            if (!CheckOwned(blueprint.Metadata[i])) { BlueprintQuarantine("capture_reflected_metadata_identity_invalid"); return; }
        uint8_t empty{};
        if (blueprint.Current==BlueprintPhase::Prepared)
        {
            blueprint.CaptureUsed=true; // before irreversible void dispatch; no premature destruction
            if (!BlueprintCall(reinterpret_cast<BN*>(blueprint.Capture.Object.Bits),blueprint.CaptureScene,&empty))
            { BlueprintQuarantine("C0_CaptureScene_invocation_uncertain"); return; }
            blueprint.C0Tick=blueprintTick; blueprint.Current=BlueprintPhase::C0;
            BlueprintLog("capture_C0",true,"CaptureScene_normal_return_no_readback_or_render_proof"); return;
        }
        if (!MiniMapReadbackNative::MarkExternalCaptureCommitted(blueprint.Job))
        { BlueprintQuarantine("C10_readback_job_commit_uncertain"); return; }
        blueprint.Committed=true;
        if (!BlueprintCall(reinterpret_cast<BN*>(blueprint.Capture.Object.Bits),blueprint.CaptureScene,&empty))
        { BlueprintQuarantine("C10_CaptureScene_invocation_uncertain"); return; }
        ++totals.Captured;
        blueprint.Current=BlueprintPhase::Readback;
        LOG_INFO("MiniMap: private_blueprint_actor id=%llu step=capture submission outcome=success C0=%llu C10=%llu interval=%llu evidence=normal native return only publication=0",
            (unsigned long long)blueprint.Id,(unsigned long long)blueprint.C0Tick,(unsigned long long)blueprintTick,
            (unsigned long long)(blueprintTick-blueprint.C0Tick));
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
        BlueprintLog("GPU_readback",report.Result==MiniMapReadbackNative::Outcome::Verified,
            report.Failure?report.Failure:"callbacks_native_readback_and_texture_ownership_retired");
        if (report.Result==MiniMapReadbackNative::Outcome::Verified)
        {
            try { ExportBlueprintReadback(); }
            catch (...) { blueprint.Failed=true; blueprint.Failure="CPU_image_processing_or_output_exception"; ++totals.CaptureFailed; BlueprintLog("image_validation",false,blueprint.Failure); }
        }
        else { blueprint.Failed=true; blueprint.Failure=report.Failure?report.Failure:"readback_failed"; ++totals.CaptureFailed; }
        BeginBlueprintRetirement();
    }
}
