// Included inside the synchronous representation Context. No persistent objects.
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
std::string AssemblyDocument(SDK::UWorld* world)
{
    if(!Decision.HasSelection()||Decision.Selected.Building!=336)
        return C::Object({{"schema_version","1"},{"stage",C::Quote("R2.5a")},
            {"status",C::Quote("not_attempted_target_not_selected")},
            {"readiness",C::Object({{"static_only_diagnostic_capture","false"},{"complete_visual_building","false"}})}});
    const auto initialSnapshot=MiniMapBuildingCollector::GetSnapshot();
    if(world!=MiniMapMap::GetWorld()||!initialSnapshot||initialSnapshot->Generation!=Record.Generation)
        return C::Object({{"schema_version","1"},{"stage",C::Quote("R2.5a")},{"status",C::Quote("not_attempted_world_generation_changed")},
            {"readiness",C::Object({{"static_only_diagnostic_capture","false"},{"complete_visual_building","false"}})}});
    AuditActive=true;
    struct EndAudit {bool& Active;~EndAudit(){Active=false;}} endAudit{AuditActive};
    const bool representationIncomplete=Truncated||Invalid||Graph.Truncated||Work.Exhausted||Components.Truncated||Relationships.Truncated;
    bool resident=true,materialsValid=true,transformsValid=true;
    std::map<std::string,A::Node> nodes;
    std::map<std::string,A::Result> native;
    C::JsonArray candidates,chains,hierarchy,staticRecords,skeletalRecords,comparisons,blockers;
    hierarchy.Limit=32*1024;
    staticRecords.Limit=128*1024;skeletalRecords.Limit=32*1024;chains.Limit=32*1024;
    std::vector<A::NamedCandidate> names;
    std::map<std::string,uintptr_t> nativePointers;
    uintptr_t cdo=0,root=0;
    const auto buildingBase=NativeClass(L"/Script/Chimera.CrBuildingActorBase");
    if(AuditClass&&buildingBase&&ClassObject(AuditClass)&&Derives(AuditClass,buildingBase))
    {
        cdo=Value<uintptr_t>(AuditClass,0x110);
        if(!Is(cdo,AuditClass,0x2e8)||!(Value<uint32_t>(cdo,8)&0x10)||!Hold(cdo,0x2e8))cdo=0;
    }
    if(cdo)
    {
        root=Value<uintptr_t>(cdo,0x1b8);
        if(!Is(root,Scene,0x250)||!Hold(root,0x250))root=0;
        for(const auto& field:{std::pair{size_t(0x1b8),"RootComponent"},std::pair{size_t(0x2d8),"MainComponent"},std::pair{size_t(0x2e0),"MainMeshComponent"}})
        {
            if(!AuditSpend())break;
            const auto p=Value<uintptr_t>(cdo,field.first);
            const auto expected=field.first==0x2e0?StaticComponent:Scene;
            const bool valid=p&&Is(p,expected,0x250)&&Hold(p,0x250);
            Name name{};if(valid)Read(p,0x18,name);
            candidates.Add(C::Object({{"field",C::Quote(field.second)},{"status",C::Quote(!p?"absent":valid?"verified":"invalid_or_truncated")},
                {"component",valid?Ref(p,expected):"null"},{"fname_comparison_index",valid?std::to_string(name.Index):"null"},
                {"fname_number",valid?std::to_string(name.Number):"null"}}));
            if(valid){const auto id=Id(p);names.push_back({id,{name.Index,name.Number}});nativePointers[id]=p;}
        }
    }
    else blockers.Add(C::Quote("existing_cdo_unavailable_or_invalid"));
    for(const auto pointer:AuditNodes)
    {
        if(!AuditSpend()||!Is(pointer,Node,0xd8)||!Hold(pointer,0xd8)){resident=false;break;}
        A::Node n;n.Id=Id(pointer);
        const auto component=Value<uintptr_t>(pointer,0x30),declared=Value<uintptr_t>(pointer,0x28);
        if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared,0xb8)||!Hold(component,0xb8))
        {resident=false;continue;}
        n.Component=Id(component);n.ParentConflict=AuditConflictingParents.contains(n.Id);
        if(const auto parent=AuditParents.find(n.Id);parent!=AuditParents.end())n.Parent=parent->second;
        const auto attach=Value<Name>(pointer,0x80);n.Socket=attach.Index!=0||attach.Number!=0;
        if(Value<uint8_t>(pointer,0x98)!=0)
        {
            if(!n.Parent.empty())n.ParentConflict=true;
            const auto parentName=Value<Name>(pointer,0x88);bool ambiguous=false;
            n.NativeParent=A::Match({parentName.Index,parentName.Number},names,ambiguous);
            C::JsonArray chain;
            A::Result result;
            if(ambiguous){result.Status="invalid";result.Reason="ambiguous_native_parent";n.ParentConflict=true;}
            else if(!n.NativeParent.empty()&&root)
            {std::set<uintptr_t> active;result=AuditNativeChain(nativePointers.at(n.NativeParent),root,active,0,chain);native[n.NativeParent]=result;}
            else result.Reason="native_parent_unresolved";
            chains.Add(C::Object({{"scs_node_id",C::Quote(n.Id)},{"declared_parent",NameValue(pointer,0x88)},
                {"matched_component_id",n.NativeParent.empty()?"null":C::Quote(n.NativeParent)},
                {"status",C::Quote(result.Status)},{"reason",C::Quote(result.Reason)},{"chain",chain.String()}}));
        }
        n.Scene=Is(component,Scene,0x250);
        bool componentSocket=false;
        if(n.Scene)n.InputsValid=AuditTransform(component,n.Local,n.Absolute,componentSocket);
        if(n.Scene)n.TemplateAttachment=Value<uintptr_t>(component,0xc8)!=0;
        n.Socket|=componentSocket;
        std::string visibility="null";
        if(n.Scene)
        {visibility=C::Object({{"visible",C::Bool(Value<uint8_t>(component,0x1a0)&32)},
            {"hidden_in_game",C::Bool(Value<uint8_t>(component,0x1a1)&8)},{"absolute_flags_present",C::Bool(n.Absolute)}});}
        uintptr_t mesh=0;
        const auto assignment=AuditAssignments.find(component);
        if(assignment!=AuditAssignments.end())mesh=assignment->second;
        const bool isStatic=Is(component,StaticComponent,0x590),isSkeletal=!isStatic&&Is(component,SkeletalComponent,0x598);
        n.Kind=isStatic?"static":isSkeletal?"skeletal":"none";
        const auto expected=isStatic?StaticMesh:SkeletalMesh;
        bool meshValid=(isStatic||isSkeletal)&&mesh&&Is(mesh,expected,isStatic?0x170:0x1b0)&&Hold(mesh);
        if(isStatic||isSkeletal)
        {
            if(!meshValid)resident=false;
            n.Mesh=meshValid?Id(mesh):"";
            bool defaultsValid=false,overridesValid=false;
            std::string defaults="[]",overrides="[]";
            std::vector<A::Slot> authored;
            if(meshValid)authored=AuditMaterials(mesh+(isStatic?0x160:0x1a0),isStatic?0x38:0x30,defaultsValid,defaults);
            const auto overridden=AuditMaterials(component+0x530,8,overridesValid,overrides);
            const auto proposed=A::ProposedMaterials(authored,overridden);C::JsonArray mapping;
            bool allPresent=!proposed.empty();
            for(const auto& slot:proposed)
            {allPresent&=slot.Valid;mapping.Add(C::Object({{"slot",std::to_string(slot.Index)},
                {"material_id",slot.Valid?C::Quote(slot.Material):"null"}}));}
            if(isStatic)materialsValid&=defaultsValid&&overridesValid&&allPresent;
            n.Metadata=C::Object({{"component_identity",Ref(component,declared)},{"mesh_identity",meshValid?Ref(mesh,expected):"null"},
                {"source",C::Quote("selected_actor_class_scs_template")},{"visibility",visibility},
                {"raw_relative_location",n.Scene?Vector(component,0x140,3):"null"},
                {"raw_relative_rotator_pitch_yaw_roll",n.Scene?Vector(component,0x158,3):"null"},
                {"raw_relative_scale",n.Scene?Vector(component,0x170,3):"null"},
                {"template_attach_parent_status",C::Quote(n.TemplateAttachment?"present_unreconciled":"absent")},
                {"authored_mesh_materials",defaults},{"component_overrides",overrides},
                {"proposed_material_slots",mapping.String()},
                {"material_resolution_status",C::Quote(defaultsValid&&overridesValid&&allPresent?"conditional_stored_override_precedence":"unresolved_material_inputs")},
                {"animation_mode",C::Object({{"status",C::Quote("not_attempted")},{"reason",C::Quote("complete enum read contract not verified; no animation evaluated")}})},
                {"animation_class",C::Object({{"status",C::Quote("not_attempted")},{"reason",C::Quote("optional subclass read contract not verified")}})},
                {"bounds",C::Object({{"status",C::Quote("not_attempted")},{"reason",C::Quote("no new render-data or bounds synchronization contract")}})},
                {"reference_pose_supported","null"},{"runtime_adjustments",C::Quote("runtime_adjustments_unverified")},
                {"cooked_instancing",C::Quote("not_evaluated")},{"effective_inherited_override_selection",C::Quote("not_evaluated")}});
        }
        hierarchy.Add(C::Object({{"scs_node_id",C::Quote(n.Id)},{"component_id",C::Quote(n.Component)},
            {"mesh_kind",C::Quote(n.Kind)},{"scene_component",C::Bool(n.Scene)},
            {"parent_scs_node_id",n.Parent.empty()?"null":C::Quote(n.Parent)},
            {"native_parent_id",n.NativeParent.empty()?"null":C::Quote(n.NativeParent)},
            {"local_transform",n.InputsValid?A::Json(n.Local):"null"},
            {"local_inputs_status",C::Quote(!n.Scene?"not_applicable":n.InputsValid?"verified":"invalid_or_unsupported")},
            {"raw_relative_location",n.Scene?Vector(component,0x140,3):"null"},
            {"raw_relative_rotator_pitch_yaw_roll",n.Scene?Vector(component,0x158,3):"null"},
            {"raw_relative_scale",n.Scene?Vector(component,0x170,3):"null"},
            {"absolute_transform",C::Bool(n.Absolute)},{"socket_present",C::Bool(n.Socket)},
            {"template_attachment_present",C::Bool(n.TemplateAttachment)},{"parent_conflict",C::Bool(n.ParentConflict)}}));
        nodes.emplace(n.Id,std::move(n));
    }
    size_t staticCount=0,skeletalCount=0;std::set<std::string> meshes;
    for(const auto& [id,n]:nodes)
    {
        if(n.Kind=="none")continue;
        auto& count=n.Kind=="static"?staticCount:skeletalCount;
        const size_t limit=n.Kind=="static"?A::StaticLimit:A::SkeletalLimit;
        if(count>=limit){AuditTruncated=true;continue;}++count;
        std::set<std::string> active;size_t remaining=A::VisitLimit-AuditVisits;
        const auto result=A::Resolve(id,nodes,native,active,0,&remaining);AuditVisits=A::VisitLimit-remaining;
        if(result.Status=="truncated")AuditTruncated=true;
        if(n.Kind=="static"){transformsValid&=result.Status=="verified";if(!n.Mesh.empty())meshes.insert(n.Mesh);}
        const auto record=C::Object({{"component_id",C::Quote(n.Component)},{"scs_node_id",C::Quote(id)},
            {"parent_scs_node_id",n.Parent.empty()?"null":C::Quote(n.Parent)},
            {"native_parent_id",n.NativeParent.empty()?"null":C::Quote(n.NativeParent)},
            {"local_transform",n.InputsValid?A::Json(n.Local):"null"},
            {"effective_root_relative_transform",result.Status=="verified"?A::Json(result.Effective):"null"},
            {"transform_status",C::Quote(result.Status)},{"transform_reason",C::Quote(result.Reason)},
            {"native_ancestry_resolved",C::Bool(result.Status=="verified")},{"inputs",n.Metadata}});
        (n.Kind=="static"?staticRecords:skeletalRecords).Add(record);
        if(n.Kind=="static")
        {
            C::JsonArray matches;matches.Limit=32*1024;
            for(const auto& mass:AuditMassRecords)if(!n.Mesh.empty()&&n.Mesh==mass.Mesh)
            {
                const auto offset=AuditMassOffsets.find({mass.Descriptor,mass.Ordinal});
                matches.Add(C::Object({{"descriptor_id",C::Quote(mass.Descriptor)},{"ordinal",std::to_string(mass.Ordinal)},
                    {"mesh_identity_equal","true"},{"local_transform",mass.Local},{"raw_transform_offset",offset==AuditMassOffsets.end()?"null":offset->second},
                    {"offset_status",C::Quote(offset==AuditMassOffsets.end()?"unavailable":"recorded_raw")},
                    {"mass_material_overrides",mass.Materials},{"min_lod_significance",mass.Min},{"max_lod_significance",mass.Max},
                    {"material_equivalence",C::Quote("unresolved; compare stored actor inputs separately; no material getters")}}));
            }
            AuditTruncated|=matches.Truncated;
            comparisons.Add(C::Object({{"component_id",C::Quote(n.Component)},{"mesh_id",n.Mesh.empty()?"null":C::Quote(n.Mesh)},
                {"matching_mass_references",matches.String()},
                {"actor_transform_status",C::Quote(result.Status)},{"equivalence",C::Quote("unresolved")},
                {"reason",C::Quote("match snapshot mesh IDs against separate ordered Mass records; raw offsets not composed, repeated instances not paired by list order")}}));
        }
    }
    if(!staticCount)blockers.Add(C::Quote("no_validated_static_instances"));
    if(!transformsValid)blockers.Add(C::Quote("static_hierarchy_or_native_parent_unresolved"));
    if(!materialsValid)blockers.Add(C::Quote("static_material_inputs_unresolved"));
    if(!resident)blockers.Add(C::Quote("source_identity_validation_incomplete"));
    blockers.Add(C::Quote("runtime_adjustments_unverified"));
    blockers.Add(C::Quote("cooked_instancing_and_effective_inherited_selection_unverified"));
    blockers.Add(C::Quote("future_multi_component_cleanup_not_implemented"));
    const auto currentSnapshot=MiniMapBuildingCollector::GetSnapshot();
    const bool worldValid=world==MiniMapMap::GetWorld()&&currentSnapshot&&currentSnapshot->Generation==Record.Generation;
    if(!worldValid)blockers.Add(C::Quote("world_or_generation_changed"));
    AuditTruncated|=AuditCollectionTruncated||hierarchy.Truncated||staticRecords.Truncated||skeletalRecords.Truncated||chains.Truncated||comparisons.Truncated||Work.Exhausted;
    if(AuditTruncated||representationIncomplete)blockers.Add(C::Quote("incomplete_or_truncated_input"));
    return A::Bound(C::Object({{"schema_version","1"},{"stage",C::Quote("R2.5a")},{"game_build",C::Quote("5.6.1-127004")},
        {"building_id","336"},{"status",C::Quote(AuditTruncated?"truncated":!staticCount||!resident||!transformsValid?"unresolved":"conditional")},
        {"representation_source",C::Quote(SelectedTier+"_scs")},
        {"reference_space",C::Quote("existing selected-class CDO RootComponent; its own placement/external attachment excluded")},
        {"building_placement_applied","false"},{"camera_transform_applied","false"},
        {"identity_bridge",C::Object({{"status",C::Quote("identity_bridge_unavailable")},
            {"reason",C::Quote("retained capture asset is not exposed in this context; no verified synchronous overlap or copied generation-token bridge; no relookup or lifetime extension")}})},
        {"native_parent",C::Object({{"cdo",cdo?C::Quote(Id(cdo)):"null"},{"assembly_root_id",root?C::Quote(Id(root)):"null"},
            {"direct_candidates",candidates.String()},{"scs_native_parent_resolutions",chains.String()}})},
        {"static_instance_count",std::to_string(staticCount)},{"unique_static_mesh_count",std::to_string(meshes.size())},
        {"hierarchy_nodes",hierarchy.String()},{"static_instances",staticRecords.String()},{"skeletal_components",skeletalRecords.String()},
        {"material_resolution",C::Quote("proposed stored non-null component override by slot, else authored mesh input; Nanite/virtual/live resolution not evaluated; no getters invoked")},
        {"mass_comparison",C::Object({{"actor_instances",comparisons.String()},{"mass_alternative",Mass.String()},
            {"equivalence",C::Quote("unresolved")},{"renderer_equivalent_transforms","false"}})},
        {"alpha_summary",C::Object({{"status",C::Quote("unsupported")},{"reason",C::Quote("immutable readback sharing not established; existing capture ownership and export untouched")}})},
        {"readiness",C::Object({{"static_only_diagnostic_capture",C::Bool(A::CaptureReady(staticCount>0&&transformsValid,materialsValid,resident&&worldValid,false,false,AuditTruncated||representationIncomplete))},
            {"complete_visual_building","false"},{"blocking_reasons",blockers.String()}})},
        {"truncation",C::Object({{"audit_limits",C::Bool(AuditTruncated)},{"upstream_representation_incomplete",C::Bool(representationIncomplete)},
            {"identity_graph_visits",std::to_string(AuditVisits)},{"visit_limit",std::to_string(A::VisitLimit)},
            {"output_limit",std::to_string(A::OutputLimit)}})},
        {"limitations",C::Quote("Authored recipe only; no live actor, construction scripts, pose evaluation, source loading, native enumeration or rendering. Skeletal contribution prevents completeness. Positive scales use UE QST composition; absolute/socket/negative/degenerate paths fail closed.")}}));
}
