// Included inside the synchronous representation Context. The optional R2.5b
// handoff independently retains assets; Context's own roots remain local.
bool AuditTransform(uintptr_t p,A::Transform& t,bool& absolute,bool& socket)
{
    A::V rotation{};Name name{};uint8_t flags=0;
    if(!Read(p,0x140,t.T)||!Read(p,0x158,rotation)||!Read(p,0x170,t.S)||
        !Read(p,0x1a0,flags)||!Read(p,0xd0,name))return false;
    absolute=(flags&28)!=0;socket=name.Index!=0||name.Number!=0;
    if(!A::Finite(rotation))return false;
    t.R=A::FromRotator(rotation);return A::Normalize(t.R)&&A::Valid(t);
}
std::vector<A::Slot> AuditMaterials(uintptr_t header,size_t stride,bool& valid,std::string& json,bool detailed=false)
{
    std::vector<A::Slot> slots;C::JsonArray records;
    C::ArrayHeader h{};valid=false;
    if(!AuditSpend()||!Read(header,0,h))return slots;
    const auto b=C::BoundArray(h,stride,8,C::MaterialLimit);
    if(!b.Valid||(b.Bytes&&!Readable(h.Data,b.Bytes,8)))return slots;
    StorageScope storage(*this,header,h.Data,b.Bytes);
    if(b.Truncated)AuditTruncated=true;
    valid=!b.Truncated;
    for(size_t i=0;i<b.Count;++i)
    {
        if(!AuditSpend()){valid=false;break;}
        uintptr_t material=0;
        if(!Read(h.Data+i*stride,0,material)){valid=false;break;}
        bool objectValid=false,compatible=false,retained=false;
        const bool ok=detailed?
            ((objectValid=material&&CheckIdentity(material))&&
             (compatible=Derives(Value<uintptr_t>(material,0x10),Material))&&(retained=Hold(material))):
            material&&Is(material,Material)&&Hold(material);
        if(ok)AuditMaterialPointers[Id(material)]=material;
        if(material&&!ok)valid=false;
        slots.push_back({i,ok?Id(material):"",ok});
        if(detailed)
            records.Add(C::Object({{"slot",std::to_string(i)},{"material_id",ok?C::Quote(Id(material)):"null"},
                {"status",C::Quote(!material?"absent":ok?"verified":"invalid_or_truncated")},{"nonNull",C::Bool(material!=0)},
                {"objectValid",C::Bool(objectValid)},{"materialInterfaceCompatible",C::Bool(compatible)},
                {"retained",C::Bool(retained)},{"pointerMapped",C::Bool(ok&&AuditMaterialPointers.contains(Id(material)))},
                {"failure",C::Quote(!material?"null_slot":!objectValid?"object_identity_or_budget":
                    !compatible?"material_interface_type_or_budget":!retained?"retention_validation_or_budget":"none")}}));
        else
            records.Add(C::Object({{"slot",std::to_string(i)},{"material_id",ok?C::Quote(Id(material)):"null"},
                {"status",C::Quote(!material?"absent":ok?"verified":"invalid_or_truncated")}}));
    }
    DiagnosticTruncated|=records.Truncated;json=records.String();return slots;
}
A::Result AuditNativeChain(uintptr_t p,uintptr_t root,std::set<uintptr_t>& active,size_t depth,C::JsonArray& chain)
{
    A::Result r;
    if(depth>=A::DepthLimit){AuditTruncated=true;r.Status="truncated";r.Reason="native_parent_depth_limit";return r;}
    if(!AuditSpend()||!Is(p,Scene,0x250)||!Hold(p,0x250))
    {r.Status=AuditTruncated?"truncated":"invalid";r.Reason="native_parent_identity_or_type";return r;}
    if(!active.insert(p).second){r.Status="invalid";r.Reason="native_parent_cycle";return r;}
    struct Leave {std::set<uintptr_t>& S;uintptr_t P;~Leave(){S.erase(P);}} leave{active,p};
    A::Transform local;bool absolute=false,socket=false;
    const bool valid=AuditTransform(p,local,absolute,socket);
    uintptr_t parent=0;const bool parentRead=Read(p,0xc8,parent);
    chain.Add(C::Object({{"component_id",C::Quote(Id(p))},{"component_identity",Ref(p,Scene)},
        {"relative_transform",valid?A::Json(local):"null"},{"absolute_transform",C::Bool(absolute)},
        {"socket",NameValue(p,0xd0)},{"parent_id",parentRead&&parent?C::Quote(Id(parent)):"null"},
        {"is_assembly_root",C::Bool(p==root)}}));
    DiagnosticTruncated|=chain.Truncated;
    // Root space is defined at this component, excluding its own world-relative
    // transform and any external attachment. This is not an identity-parent guess.
    if(p==root){r.Status="verified";r.Reason="existing_cdo_root_reference_space";return r;}
    if(!valid){r.Status="invalid";r.Reason="unsupported_native_transform";return r;}
    if(absolute||socket){r.Status="unsupported";r.Reason=absolute?"native_absolute_transform":"native_socket_transform";return r;}
    if(!parentRead||!parent){r.Reason="native_chain_does_not_reach_root";return r;}
    auto result=AuditNativeChain(parent,root,active,depth+1,chain);
    if(result.Status=="verified"&&!A::Compose(local,result.Effective,result.Effective))
    {result.Status="invalid";result.Reason="native_composition_failed";}
    return result;
}
// Called in a separately bounded/rooted Context. Incomplete SCS traversal is
// visual uncertainty, never permission to skip a check on this native input.
bool AuditIndependentNativeMain(uintptr_t selectedClass,uint64_t generation,bool highDetail,
    MiniMapStaticAssembly::Recipe& output,const char*& failure)
{
    namespace SA=MiniMapStaticAssembly;
    failure="native_main_class_or_cdo_unverified";
    const auto buildingBase=NativeClass(L"/Script/Chimera.CrBuildingActorBase");
    if(!generation||!ClassObject(selectedClass)||!Derives(selectedClass,buildingBase)||!Hold(selectedClass,0x200))return false;
    uintptr_t cdo=0,cdoClass=0,root=0,main=0;uint32_t flags=0;
    if(!Read(selectedClass,0x110,cdo)||!Is(cdo,selectedClass,0x2e8)||!Hold(cdo,0x2e8)||
        !Read(cdo,0x10,cdoClass)||cdoClass!=selectedClass||!Read(cdo,8,flags)||!(flags&0x10))return false;
    failure="native_main_component_or_ownership_unverified";
    if(!Read(cdo,0x1b8,root)||!Read(cdo,0x2e0,main)||!Is(root,Scene,0x250)||!Hold(root,0x250)||
        !Is(main,StaticComponent,0x590)||!Hold(main,0x590))return false;
    uintptr_t type=0,owner=0,rootOwner=0;
    if(!Read(main,0x10,type)||type!=StaticComponent||!Read(main,0x20,owner)||owner!=cdo||
        !Read(root,0x20,rootOwner)||rootOwner!=cdo)return false;
    SA::Recipe recipe;recipe.General=true;recipe.Generation=generation;recipe.ActorClass=selectedClass;
    recipe.RootVerified=true;recipe.HighDetail=highDetail;recipe.VisualIncomplete=true;recipe.Count=1;
    auto& input=recipe.Instances[0];uint8_t visible=0,hidden=0;int32_t index=-1;
    failure="native_main_visibility_or_mesh_unverified";
    if(!Read(main,0x1a0,visible)||!Read(main,0x1a1,hidden)||!(visible&32)||(hidden&8)||
        !Read(main,0xc,index)||index<0||!Read(main,0x588,input.Mesh)||
        !Is(input.Mesh,StaticMesh,0x278)||!Hold(input.Mesh,0x278))return false;
    input.ComponentIndex=uint32_t(index);input.Visible=true;input.Hidden=false;
    std::set<uintptr_t> active;C::JsonArray chain;
    const auto transform=AuditNativeChain(main,root,active,0,chain);
    failure="native_main_root_relative_transform_unverified";
    if(transform.Status!="verified")return false;
    input.Transform=transform.Effective;
    bool defaultsValid=false,overridesValid=false;std::string defaultsJson,overridesJson;
    const auto defaults=AuditMaterials(input.Mesh+0x160,0x38,defaultsValid,defaultsJson);
    const auto overrides=AuditMaterials(main+0x530,8,overridesValid,overridesJson);
    const auto effective=A::ProposedMaterials(defaults,overrides);
    failure="native_main_effective_materials_unverified";
    if(!defaultsValid||!overridesValid||effective.empty()||effective.size()>SA::Slots)return false;
    input.MaterialCount=effective.size();
    for(const auto& slot:effective)
    {
        const auto material=AuditMaterialPointers.find(slot.Material);
        if(slot.Index>=input.MaterialCount||!slot.Valid||material==AuditMaterialPointers.end())return false;
        input.Materials[slot.Index]=material->second;
        for(const auto& over:overrides)if(over.Index==slot.Index&&over.Valid&&!over.Material.empty())input.Overrides[slot.Index]=true;
    }
    failure="native_main_bounds_or_independent_evidence_incomplete";
    if(!Read(input.Mesh,0x240,input.LocalBounds.Origin)||!Read(input.Mesh,0x258,input.LocalBounds.Extent)||
        AuditTruncated||AuditCollectionTruncated||Truncated||Invalid||Work.Exhausted||Graph.Truncated)return false;
    input.Verified=true;recipe.Complete=true;SA::Footprint bounds;
    const auto check=SA::ValidateTyped(recipe,bounds);
    if(check.Failure!=SA::RecipeFailure::None){failure=SA::Label(check.Failure);return false;}
    output=recipe;failure=nullptr;return true;
}
