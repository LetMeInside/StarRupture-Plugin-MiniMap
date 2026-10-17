// Representation metadata inspection only; never prepares rendering components.
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
