// Included inside the synchronous representation Context. The optional R2.5b
// handoff independently retains assets; Context's own roots remain local.
bool AuditTransform(uintptr_t p,A::Transform& t,bool& absolute,bool& socket)
{
    const auto* scene=reinterpret_cast<const SDK::USceneComponent*>(p);
    const A::V rotation{scene->RelativeRotation.Pitch,scene->RelativeRotation.Yaw,scene->RelativeRotation.Roll};
    t.T={scene->RelativeLocation.X,scene->RelativeLocation.Y,scene->RelativeLocation.Z};
    t.S={scene->RelativeScale3D.X,scene->RelativeScale3D.Y,scene->RelativeScale3D.Z};
    absolute=scene->bAbsoluteLocation||scene->bAbsoluteRotation||scene->bAbsoluteScale;socket=!scene->AttachSocketName.IsNone();
    if(!A::Finite(rotation))return false;
    t.R=A::FromRotator(rotation);return A::Normalize(t.R)&&A::Valid(t);
}
template<class T> std::vector<A::Slot> AuditMaterials(const SDK::TArray<T>& values,bool& valid,std::string& json,bool detailed=false)
{
    std::vector<A::Slot> slots;C::JsonArray records;
    const C::ArrayHeader h{reinterpret_cast<uintptr_t>(values.GetDataPtr()),values.Num(),values.Max()};valid=false;
    if(!AuditSpend())return slots;
    const auto b=C::BoundArray(h,sizeof(T),alignof(T),C::MaterialLimit);
    if(!b.Valid||(b.Bytes&&!Readable(h.Data,b.Bytes,8)))return slots;
    if(b.Truncated)AuditTruncated=true;
    valid=!b.Truncated;
    for(size_t i=0;i<b.Count;++i)
    {
        if(!AuditSpend()){valid=false;break;}
        const auto& value=values[int32_t(i)];
        const auto material=reinterpret_cast<uintptr_t>([&]{if constexpr(std::is_pointer_v<T>)return value;else return value.MaterialInterface;}());
        bool objectValid=false,compatible=false,retained=false;
        const bool ok=detailed?
            ((objectValid=material&&CheckIdentity(material))&&
             (compatible=Derives(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(material)->Class),Material))&&(retained=Hold(material))):
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
    if(!AuditSpend()||!Is(p,Scene)||!Hold(p))
    {r.Status=AuditTruncated?"truncated":"invalid";r.Reason="native_parent_identity_or_type";return r;}
    if(!active.insert(p).second){r.Status="invalid";r.Reason="native_parent_cycle";return r;}
    struct Leave {std::set<uintptr_t>& S;uintptr_t P;~Leave(){S.erase(P);}} leave{active,p};
    A::Transform local;bool absolute=false,socket=false;
    const bool valid=AuditTransform(p,local,absolute,socket);
    const auto parent=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USceneComponent*>(p)->AttachParent);
    chain.Add(C::Object({{"component_id",C::Quote(Id(p))},{"component_identity",Ref(p,Scene)},
        {"relative_transform",valid?A::Json(local):"null"},{"absolute_transform",C::Bool(absolute)},
        {"socket",NameValue(reinterpret_cast<const SDK::USceneComponent*>(p)->AttachSocketName)},{"parent_id",parent?C::Quote(Id(parent)):"null"},
        {"is_assembly_root",C::Bool(p==root)}}));
    DiagnosticTruncated|=chain.Truncated;
    // Root space is defined at this component, excluding its own world-relative
    // transform and any external attachment. This is not an identity-parent guess.
    if(p==root){r.Status="verified";r.Reason="existing_cdo_root_reference_space";return r;}
    if(!valid){r.Status="invalid";r.Reason="unsupported_native_transform";return r;}
    if(absolute||socket){r.Status="unsupported";r.Reason=absolute?"native_absolute_transform":"native_socket_transform";return r;}
    if(!parent){r.Reason="native_chain_does_not_reach_root";return r;}
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
    if(!generation||!ClassObject(selectedClass)||!Derives(selectedClass,buildingBase)||!Hold(selectedClass))return false;
    const auto cdo=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UClass*>(selectedClass)->ClassDefaultObject);
    if(!Is(cdo,selectedClass)||!Hold(cdo)||reinterpret_cast<const SDK::UObject*>(cdo)->Class!=reinterpret_cast<const SDK::UClass*>(selectedClass)||
        !(uint32_t(reinterpret_cast<const SDK::UObject*>(cdo)->Flags)&0x10))return false;
    failure="native_main_component_or_ownership_unverified";
    const auto* actor=reinterpret_cast<const SDK::ACrBuildingActorBase*>(cdo);
    const auto root=reinterpret_cast<uintptr_t>(actor->RootComponent),main=reinterpret_cast<uintptr_t>(actor->MainMeshComponent);
    if(!Is(root,Scene)||!Hold(root)||!Is(main,StaticComponent)||!Hold(main))return false;
    const auto* component=reinterpret_cast<const SDK::UStaticMeshComponent*>(main);
    if(component->Class!=reinterpret_cast<const SDK::UClass*>(StaticComponent)||component->Outer!=reinterpret_cast<const SDK::UObject*>(cdo)||
        reinterpret_cast<const SDK::UObject*>(root)->Outer!=reinterpret_cast<const SDK::UObject*>(cdo))return false;
    SA::Recipe recipe;recipe.General=true;recipe.Generation=generation;recipe.ActorClass=selectedClass;
    recipe.RootVerified=true;recipe.HighDetail=highDetail;recipe.VisualIncomplete=true;recipe.Count=1;
    auto& input=recipe.Instances[0];const auto index=component->Index;
    failure="native_main_visibility_or_mesh_unverified";
    if(!component->bVisible||component->bHiddenInGame||index<0)return false;
    input.Mesh=reinterpret_cast<uintptr_t>(component->StaticMesh);
    if(!Is(input.Mesh,StaticMesh)||!Hold(input.Mesh))return false;
    input.ComponentIndex=uint32_t(index);input.Visible=true;input.Hidden=false;
    std::set<uintptr_t> active;C::JsonArray chain;
    const auto transform=AuditNativeChain(main,root,active,0,chain);
    failure="native_main_root_relative_transform_unverified";
    if(transform.Status!="verified")return false;
    input.Transform=transform.Effective;
    bool defaultsValid=false,overridesValid=false;std::string defaultsJson,overridesJson;
    const auto defaults=AuditMaterials(reinterpret_cast<const SDK::UStaticMesh*>(input.Mesh)->StaticMaterials,defaultsValid,defaultsJson);
    const auto overrides=AuditMaterials(reinterpret_cast<const SDK::UMeshComponent*>(main)->OverrideMaterials,overridesValid,overridesJson);
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
    const auto& sourceBounds=reinterpret_cast<const SDK::UStaticMesh*>(input.Mesh)->ExtendedBounds;
    input.LocalBounds.Origin={sourceBounds.Origin.X,sourceBounds.Origin.Y,sourceBounds.Origin.Z};
    input.LocalBounds.Extent={sourceBounds.BoxExtent.X,sourceBounds.BoxExtent.Y,sourceBounds.BoxExtent.Z};
    if(AuditTruncated||AuditCollectionTruncated||Truncated||Invalid||Work.Exhausted||Graph.Truncated)return false;
    input.Verified=true;recipe.Complete=true;SA::Footprint bounds;
    const auto check=SA::ValidateTyped(recipe,bounds);
    if(check.Failure!=SA::RecipeFailure::None){failure=SA::Label(check.Failure);return false;}
    output=recipe;failure=nullptr;return true;
}
