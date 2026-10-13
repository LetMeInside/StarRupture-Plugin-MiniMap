// Context-local optional CPU metadata; independent roots/budget, no readiness policy.
SDK::UFunction* MetadataFunction(const wchar_t* ownerPath,const wchar_t* name,bool indexed,bool bounds)
{
    const auto functionClass=NativeClass(L"/Script/CoreUObject.Function"),owner=NativeClass(ownerPath);
    if(!functionClass||!owner||!engine->GetStaticFindObjectByNameAddress())return nullptr;
    using Find=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
    const auto address=reinterpret_cast<uintptr_t>(reinterpret_cast<Find>(engine->GetStaticFindObjectByNameAddress())(
        reinterpret_cast<SDK::UClass*>(functionClass),reinterpret_cast<SDK::UObject*>(owner),name,true));
    if(!Is(address,functionClass,sizeof(SDK::UFunction))||!Hold(address,sizeof(SDK::UFunction)))return nullptr;
    auto* fn=reinterpret_cast<SDK::UFunction*>(address);
    const uint16_t frame=bounds?0x38:indexed?8:4;
    if(fn->Outer!=reinterpret_cast<SDK::UObject*>(owner)||!fn->ExecFunction||
        !(uint32_t(fn->FunctionFlags)&0x400)||(uint32_t(fn->FunctionFlags)&0x2000)||
        Value<uint8_t>(address,0xb4)!=(indexed?2:1)||Value<uint16_t>(address,0xb6)!=frame)return nullptr;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(reinterpret_cast<const void*>(fn->ExecFunction),&memory,sizeof(memory))||memory.State!=MEM_COMMIT||
        (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
        !(memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return nullptr;
    // Installed reflected layouts already used by the capture's SkeletalFunctions.
    unsigned seen=0;size_t visits=0;
    for(auto* field=fn->ChildProperties;field;field=field->Next)
    {
        if(++visits>(indexed?2u:1u)||!Readable(reinterpret_cast<uintptr_t>(field),sizeof(SDK::FProperty),8)||
            !Readable(reinterpret_cast<uintptr_t>(field->ClassPrivate),0x18,8))return nullptr;
        const auto* p=reinterpret_cast<const SDK::FProperty*>(field);
        uintptr_t owning=0;std::memcpy(&owning,&p->Owner,8);
        const bool returned=(p->PropertyFlags&0x400)!=0;const unsigned bit=returned?1:2;
        if((owning&~uintptr_t(1))!=address||!(owning&1)||!(p->PropertyFlags&0x80)||p->ArrayDim!=1||
            p->Offset!=(returned&&indexed?4:0)||p->ElementSize!=(bounds?0x38:4)||(seen&bit)||
            (!returned&&(!indexed||(p->PropertyFlags&(0x100|0x8000000)))))return nullptr;
        const auto kind=bounds?SDK::EClassCastFlags::StructProperty:SDK::EClassCastFlags::IntProperty;
        if(!(p->ClassPrivate->CastFlags&uint64_t(kind)))return nullptr;
        if(bounds)
        {
            if(!Readable(reinterpret_cast<uintptr_t>(p),0x78,8))return nullptr;
            uintptr_t type=0;std::memcpy(&type,reinterpret_cast<const uint8_t*>(p)+0x70,8);
            const auto expected=AcquireResidentMetadata(L"/Script/CoreUObject.BoxSphereBounds",true,0x38);
            if(!expected)return nullptr;
            const bool equal=type==expected;ReleaseResidentMetadata(expected);if(!equal)return nullptr;
        }
        seen|=bit;
    }
    return seen==(indexed?3u:1u)?fn:nullptr;
}
template<class Params> bool MetadataCall(uintptr_t receiver,SDK::UFunction* fn,Params& params)
{
    auto* hooks=GetHooks();const auto gt=MiniMapSmelterCapture::GetBindings().GameThread;
    return fn&&Held.contains(receiver)&&gt&&reinterpret_cast<bool(*)()>(gt)()&&
        hooks&&hooks->ObjectWalker&&hooks->ObjectWalker->InvokeResolvedUFunction&&
        hooks->ObjectWalker->InvokeResolvedUFunction(reinterpret_cast<void*>(receiver),fn,&params);
}
std::string MetadataLoadFlags(uintptr_t object)
{
    if(!Held.contains(object))return "{\"status\":\"unavailable\"}";
    const auto flags=uint32_t(reinterpret_cast<const SDK::UObject*>(object)->Flags);
    return C::Object({{"status",C::Quote("observed")},
        {"need_load",C::Bool((flags&uint32_t(SDK::EObjectFlags::NeedLoad))!=0)},
        {"need_post_load",C::Bool((flags&uint32_t(SDK::EObjectFlags::NeedPostLoad))!=0)},
        {"need_post_load_subobjects",C::Bool((flags&uint32_t(SDK::EObjectFlags::NeedPostLoadSubobjects))!=0)},
        {"milestone",C::Quote("source_validation; no_load_completion_event_observed")}});
}
static std::string MetadataBounds(const SDK::FBoxSphereBounds& b)
{
    if(!std::isfinite(b.Origin.X)||!std::isfinite(b.Origin.Y)||!std::isfinite(b.Origin.Z)||
        !std::isfinite(b.BoxExtent.X)||!std::isfinite(b.BoxExtent.Y)||!std::isfinite(b.BoxExtent.Z)||
        b.BoxExtent.X<0||b.BoxExtent.Y<0||b.BoxExtent.Z<0)return "{\"status\":\"invalid_bounds\"}";
    return C::Object({{"status",C::Quote("observed")},
        {"origin","["+C::Number(b.Origin.X)+","+C::Number(b.Origin.Y)+","+C::Number(b.Origin.Z)+"]"},
        {"extent","["+C::Number(b.BoxExtent.X)+","+C::Number(b.BoxExtent.Y)+","+C::Number(b.BoxExtent.Z)+"]"}});
}
template<class Sources> void SourceMetadata(const MiniMapStaticAssembly::Recipe& recipe,const Sources& skeletal,
    size_t skeletalCount,bool skeletalTruncated,MiniMapSpriteValidation::Result& out)
{
    static_assert(sizeof(SDK::Params::StaticMesh_GetNumLODs)==4);
    static_assert(sizeof(SDK::Params::StaticMesh_GetNumSections)==8&&offsetof(SDK::Params::StaticMesh_GetNumSections,ReturnValue)==4);
    static_assert(sizeof(SDK::Params::StaticMesh_GetNumTriangles)==8&&offsetof(SDK::Params::StaticMesh_GetNumTriangles,ReturnValue)==4);
    static_assert(sizeof(SDK::Params::StaticMesh_GetNumVertices)==8&&offsetof(SDK::Params::StaticMesh_GetNumVertices,ReturnValue)==4);
    static_assert(sizeof(SDK::Params::SkeletalMesh_GetImportedBounds)==0x38);
    constexpr size_t LodLimit=8;
    C::JsonArray geometry,sources;geometry.Limit=12*1024;sources.Limit=24*1024;
    std::set<uintptr_t> measured;std::array<SDK::UFunction*,4> functions{};bool resolved=false;
    for(size_t i=0;i<recipe.Count;++i)
    {
        const auto mesh=recipe.Instances[i].Mesh;if(!mesh||!measured.insert(mesh).second)continue;
        if(!resolved)
        {
            resolved=true;const wchar_t* names[]={L"GetNumLODs",L"GetNumSections",L"GetNumTriangles",L"GetNumVertices"};
            for(size_t f=0;f<functions.size();++f)functions[f]=MetadataFunction(L"/Script/Engine.StaticMesh",names[f],f!=0,false);
        }
        SDK::Params::StaticMesh_GetNumLODs count{};C::JsonArray lods;lods.Limit=2048;
        const bool got=Is(mesh,StaticMesh,sizeof(SDK::UStaticMesh))&&Hold(mesh,sizeof(SDK::UStaticMesh))&&
            MetadataCall(mesh,functions[0],count)&&count.ReturnValue>=0;
        bool ok=got;const bool truncated=got&&size_t(count.ReturnValue)>LodLimit;
        if(got)for(int32_t lod=0;lod<(std::min)(count.ReturnValue,int32_t(LodLimit));++lod)
        {
            SDK::Params::StaticMesh_GetNumSections sections{};sections.InLOD=lod;
            SDK::Params::StaticMesh_GetNumTriangles triangles{};triangles.LODIndex=lod;
            SDK::Params::StaticMesh_GetNumVertices vertices{};vertices.LODIndex=lod;
            const bool success=MetadataCall(mesh,functions[1],sections)&&MetadataCall(mesh,functions[2],triangles)&&MetadataCall(mesh,functions[3],vertices)&&
                sections.ReturnValue>=0&&triangles.ReturnValue>=0&&vertices.ReturnValue>=0;
            ok&=success;
            lods.Add(C::Object({{"lod",std::to_string(lod)},{"success",C::Bool(success)},
                {"sections",success?std::to_string(sections.ReturnValue):"null"},
                {"triangles",success?std::to_string(triangles.ReturnValue):"null"},
                {"vertices",success?std::to_string(vertices.ReturnValue):"null"}}));
        }
        geometry.Add(C::Object({{"mesh_id",C::Quote(Id(mesh))},{"attempted","true"},{"success",C::Bool(ok)},
            {"status",C::Quote(ok?"observed_conventional_cpu_metadata":"unavailable_or_getter_contract_failed")},
            {"lod_count",got?std::to_string(count.ReturnValue):"null"},{"lods",lods.String()},
            {"truncated",C::Bool(truncated||lods.Truncated)}}));
    }
    auto* bounds=skeletalCount?MetadataFunction(L"/Script/Engine.SkeletalMesh",L"GetImportedBounds",false,true):nullptr;
    for(size_t i=0;i<skeletalCount;++i)
    {
        const auto& s=skeletal[i];const bool component=Hold(s.Component,sizeof(SDK::UMeshComponent));
        const bool valid=s.Valid&&Is(s.Mesh,SkeletalMesh,sizeof(SDK::USkeletalMesh))&&Hold(s.Mesh,sizeof(SDK::USkeletalMesh));
        SDK::Params::SkeletalMesh_GetImportedBounds imported{};const bool got=valid&&MetadataCall(s.Mesh,bounds,imported);
        bool defaultsValid=false,overridesValid=false;std::string defaults="[]",overrides="[]";
        const auto def=valid?AuditMaterials(s.Mesh+offsetof(SDK::USkeletalMesh,Materials),sizeof(SDK::FSkeletalMaterial),defaultsValid,defaults,true):std::vector<A::Slot>{};
        const auto over=component?AuditMaterials(s.Component+offsetof(SDK::UMeshComponent,OverrideMaterials),8,overridesValid,overrides,true):std::vector<A::Slot>{};
        C::JsonArray effective;for(const auto& slot:A::ProposedMaterials(def,over))
            effective.Add(C::Object({{"slot",std::to_string(slot.Index)},{"valid",C::Bool(slot.Valid)},
                {"material_id",slot.Valid?C::Quote(slot.Material):"null"}}));
        sources.Add(C::Object({{"component_id",C::Quote(Id(s.Component))},{"mesh_id",s.Mesh?C::Quote(Id(s.Mesh)):"null"},
            {"asset_validation_succeeded",C::Bool(s.Valid)},{"metadata_asset_available",C::Bool(valid)},
            {"imported_bounds",got?MetadataBounds(imported.ReturnValue):"{\"status\":\"unavailable_or_getter_contract_failed\"}"},
            {"extended_bounds",valid?MetadataBounds(reinterpret_cast<const SDK::USkeletalMesh*>(s.Mesh)->ExtendedBounds):"{\"status\":\"unavailable\"}"},
            {"mesh_load_flags",MetadataLoadFlags(s.Mesh)},{"component_load_flags",MetadataLoadFlags(s.Component)},
            {"material_slot_count",valid?std::to_string(reinterpret_cast<const SDK::USkeletalMesh*>(s.Mesh)->Materials.Num()):"null"},
            {"override_count",component?std::to_string(reinterpret_cast<const SDK::UMeshComponent*>(s.Component)->OverrideMaterials.Num()):"null"},
            {"default_materials",defaults},{"component_overrides",overrides},{"effective_materials",effective.String()},
            {"material_arrays_valid",C::Bool(defaultsValid&&overridesValid)},{"truncated",C::Bool(AuditTruncated||effective.Truncated)}}));
    }
    out.CookedGeometry=C::Object({{"scope",C::Quote("conventional_cpu_metadata_only; not_render_readiness")},
        {"lod_limit",std::to_string(LodLimit)},{"meshes",geometry.String()},{"truncated",C::Bool(geometry.Truncated)}});
    out.SkeletalMetadata=C::Object({{"scope",C::Quote("source_snapshot_independent_of_material_admission; not_pose_or_gpu_readiness")},
        {"slot_limit",std::to_string(C::MaterialLimit)},{"components",sources.String()},{"truncated",C::Bool(skeletalTruncated||sources.Truncated)}});
}
