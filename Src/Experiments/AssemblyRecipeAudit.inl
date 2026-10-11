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
std::vector<A::Slot> AuditMaterials(uintptr_t header,size_t stride,bool& valid,std::string& json)
{
    std::vector<A::Slot> slots;C::JsonArray records;
    C::ArrayHeader h{};valid=false;
    if(!AuditSpend()||!Read(header,0,h))return slots;
    const auto b=C::BoundArray(h,stride,8,C::MaterialLimit);
    if(!b.Valid||(b.Bytes&&!Readable(h.Data,b.Bytes,8)))return slots;
    if(b.Truncated)AuditTruncated=true;
    valid=!b.Truncated;
    for(size_t i=0;i<b.Count;++i)
    {
        if(!AuditSpend()){valid=false;break;}
        uintptr_t material=0;
        if(!Read(h.Data+i*stride,0,material)){valid=false;break;}
        const bool ok=material&&Is(material,Material)&&Hold(material);
        if(ok)AuditMaterialPointers[Id(material)]=material;
        if(material&&!ok)valid=false;
        slots.push_back({i,ok?Id(material):"",ok});
        records.Add(C::Object({{"slot",std::to_string(i)},{"material_id",ok?C::Quote(Id(material)):"null"},
            {"status",C::Quote(!material?"absent":ok?"verified":"invalid_or_truncated")}}));
    }
    json=records.String();return slots;
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
