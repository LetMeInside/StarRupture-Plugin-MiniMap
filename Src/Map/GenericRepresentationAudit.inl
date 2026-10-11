// Bounded Context-local audit; capture handoff, when requested, acquires its own roots.
MiniMapSpriteValidation::Result GenericAudit()
{
#if defined(MINIMAP_BUILDING_SPRITES)
    MiniMapBuildingSprites::TimingScope recipeTiming(MiniMapBuildingSprites::Timing::AssemblyRecipe);
#endif
    namespace V=MiniMapSpriteValidation;
    V::Result out;out.Resident=AuditClass!=0;out.Route=SelectedTier;out.Selected=Selected;
    AuditActive=true;
    struct End {bool& Active;~End(){Active=false;}} end{AuditActive};
    C::JsonArray nativeRecords,records;records.Limit=12*1024;nativeRecords.Limit=2048;
    std::vector<A::NamedCandidate> names;std::map<std::string,uintptr_t> pointers;
    std::map<std::string,A::Node> nodes;std::map<std::string,A::Result> native;
    uintptr_t cdo=0,root=0;
    if(AuditClass&&ClassObject(AuditClass)&&Derives(AuditClass,Actor))
    {
        cdo=Value<uintptr_t>(AuditClass,0x110);
        if(!Is(cdo,AuditClass,0x1c0)||!(Value<uint32_t>(cdo,8)&0x10)||!Hold(cdo,0x1c0))cdo=0;
    }
    if(cdo)
    {
        root=Value<uintptr_t>(cdo,0x1b8);
        if(!Is(root,Scene,0x250)||!Hold(root,0x250))root=0;
        const auto buildingBase=NativeClass(L"/Script/Chimera.CrBuildingActorBase");
        const bool building=buildingBase&&Derives(AuditClass,buildingBase)&&Is(cdo,AuditClass,0x2e8);
        const std::pair<size_t,const char*> fields[]={{0x1b8,"RootComponent"},{0x2d8,"MainComponent"},{0x2e0,"MainMeshComponent"}};
        for(size_t f=0;f<(building?3u:1u);++f)
        {
            if(!AuditSpend())break;
            const auto p=Value<uintptr_t>(cdo,fields[f].first);
            const bool ok=p&&Is(p,f==2?StaticComponent:Scene,0x250)&&Hold(p,0x250);
            Name name{};if(ok)Read(p,0x18,name);
            nativeRecords.Add(C::Object({{"field",C::Quote(fields[f].second)},
                {"status",C::Quote(!p?"absent":ok?"verified":"invalid")},{"component",ok?Ref(p,Scene):"null"}}));
            if(ok){const auto id=Id(p);names.push_back({id,{name.Index,name.Number}});pointers[id]=p;}
        }
    }
    out.Root=root!=0;out.Transforms=true;out.Materials=true;
    bool sourceValid=true;std::set<std::string> meshes;
    struct MeshInput {std::string Defaults,Overrides,Mapping;bool Valid=false;};
    std::map<std::string,MeshInput> materials;
    MiniMapStaticAssembly::Recipe recipe;recipe.General=true;recipe.Generation=Record.Generation;
    recipe.ActorClass=AuditClass;recipe.RootVerified=out.Root;recipe.HighDetail=SelectedTier=="high_detail_actor";
    std::map<std::string,size_t> recipeIndexes;
    std::map<std::string,A::Result> directTransforms;
    for(const auto p:AuditNodes)
    {
        if(!AuditSpend()||!Is(p,Node,0xd8)||!Hold(p,0xd8)){sourceValid=false;break;}
        A::Node n;n.Id=Id(p);
        const auto component=Value<uintptr_t>(p,0x30),declared=Value<uintptr_t>(p,0x28);
        if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared,0xb8)||!Hold(component,0xb8))
        {sourceValid=false;continue;}
        n.Component=Id(component);n.ParentConflict=AuditConflictingParents.contains(n.Id);
        if(const auto found=AuditParents.find(n.Id);found!=AuditParents.end())n.Parent=found->second;
        const auto attach=Value<Name>(p,0x80);n.Socket=attach.Index!=0||attach.Number!=0;
        if(Value<uint8_t>(p,0x98)!=0)
        {
            if(!n.Parent.empty())n.ParentConflict=true;
            const auto parent=Value<Name>(p,0x88);bool ambiguous=false;
            n.NativeParent=A::Match({parent.Index,parent.Number},names,ambiguous);
            if(ambiguous)n.ParentConflict=true;
            if(!ambiguous&&!n.NativeParent.empty()&&root)
            {std::set<uintptr_t> active;C::JsonArray chain;native[n.NativeParent]=AuditNativeChain(pointers.at(n.NativeParent),root,active,0,chain);}
        }
        n.Scene=Is(component,Scene,0x250);bool socket=false;
        if(n.Scene){n.InputsValid=AuditTransform(component,n.Local,n.Absolute,socket);n.TemplateAttachment=Value<uintptr_t>(component,0xc8)!=0;}
        n.Socket|=socket;
        n.Metadata=n.Scene?C::Object({{"visible",C::Bool(Value<uint8_t>(component,0x1a0)&32)},
            {"hidden_in_game",C::Bool(Value<uint8_t>(component,0x1a1)&8)}}):"null";
        const bool isStatic=Is(component,StaticComponent,0x590),isSkeletal=!isStatic&&Is(component,SkeletalComponent,0x598);
        // A subclass may contain spline/instance/procedural geometry. Its base
        // mesh field alone cannot certify an ordinary rigid-mesh recipe.
        if(isStatic&&Value<uintptr_t>(component,0x10)!=StaticComponent)out.DerivedStatic=true;
        n.Kind=isStatic?"static":isSkeletal?"skeletal":"none";
        if(isStatic||isSkeletal)
        {
            const auto found=AuditAssignments.find(component);const uintptr_t mesh=found==AuditAssignments.end()?0:found->second;
            const bool valid=mesh&&Is(mesh,isStatic?StaticMesh:SkeletalMesh,isStatic?0x170:0x1b0)&&Hold(mesh);
            if(!valid)sourceValid=false;else n.Mesh=Id(mesh);
            bool defaultsValid=false,overridesValid=false;MeshInput input;
            const auto defaults=valid?AuditMaterials(mesh+(isStatic?0x160:0x1a0),isStatic?0x38:0x30,defaultsValid,input.Defaults):std::vector<A::Slot>{};
            const auto overridden=AuditMaterials(component+0x530,8,overridesValid,input.Overrides);
            const auto proposed=A::ProposedMaterials(defaults,overridden);C::JsonArray mapping;
            bool present=!proposed.empty();
            for(const auto& slot:proposed){present&=slot.Valid;mapping.Add(C::Object({{"slot",std::to_string(slot.Index)},{"material_id",slot.Valid?C::Quote(slot.Material):"null"}}));}
            input.Mapping=mapping.String();input.Valid=defaultsValid&&overridesValid&&present;
            if(isStatic)
            {
                if(recipe.Count>=MiniMapStaticAssembly::Limit)AuditTruncated=true;
                else
                {
                    recipeIndexes[n.Id]=recipe.Count;auto& r=recipe.Instances[recipe.Count++];
                    r.ComponentIndex=uint32_t(Value<int32_t>(component,0xc));r.Mesh=valid?mesh:0;
                    r.Visible=(Value<uint8_t>(component,0x1a0)&32)!=0;r.Hidden=(Value<uint8_t>(component,0x1a1)&8)!=0;
                    r.Verified=valid&&input.Valid&&proposed.size()<=MiniMapStaticAssembly::Slots&&Is(mesh,StaticMesh,0x278)&&
                        Read(mesh,0x240,r.LocalBounds.Origin)&&Read(mesh,0x258,r.LocalBounds.Extent);
                    r.MaterialCount=(std::min)(proposed.size(),MiniMapStaticAssembly::Slots);
                    for(const auto& slot:proposed)
                    {
                        const auto found=AuditMaterialPointers.find(slot.Material);
                        if(slot.Index>=r.MaterialCount||!slot.Valid||found==AuditMaterialPointers.end()){r.Verified=false;break;}
                        r.Materials[slot.Index]=found->second;
                        for(const auto& over:overridden)if(over.Index==slot.Index&&over.Valid&&!over.Material.empty())r.Overrides[slot.Index]=true;
                    }
                }
            }
            if(isStatic){out.MaterialSlots+=defaults.size();out.OverrideSlots+=overridden.size();out.Materials&=input.Valid;}
            materials.emplace(n.Id,std::move(input));
        }
        else if(n.Scene){++out.Scene;if(Is(component,SkinnedComponent))out.OtherSkinned=true;}else ++out.NonScene;
        nodes.emplace(n.Id,std::move(n));
    }
    // The three already-verified CDO pointer fields are a bounded native route.
    // Never enumerate OwnedComponents or assume SCS and native lists are additive.
    const bool scsStatic=std::any_of(nodes.begin(),nodes.end(),[](const auto& item){return item.second.Kind=="static";});
    bool mixedNative=false;
    for(const auto& [componentId,p]:pointers)
    {
        if(!CaptureRequested)break; // preserve the R2.6b observational schema/counts
        bool duplicate=false;for(const auto& [_,n]:nodes)duplicate|=n.Component==componentId;
        if(duplicate||!Is(p,StaticComponent,0x590))continue;
        if(Value<uintptr_t>(p,0x10)!=StaticComponent){out.DerivedStatic=true;continue;}
        const auto mesh=Value<uintptr_t>(p,0x588);if(!mesh)continue;
        if(scsStatic){mixedNative=true;continue;}
        if(!Is(mesh,StaticMesh,0x278)||!Hold(mesh,0x278)){sourceValid=false;continue;}
        if(recipe.Count>=MiniMapStaticAssembly::Limit){AuditTruncated=true;break;}
        A::Node n;n.Id="native:"+componentId;n.Component=componentId;n.Kind="static";n.Scene=true;n.Mesh=Id(mesh);
        bool socket=false;n.InputsValid=AuditTransform(p,n.Local,n.Absolute,socket);n.Socket=socket;
        n.Metadata=C::Object({{"source",C::Quote("verified_direct_cdo_component_field")},
            {"visible",C::Bool(Value<uint8_t>(p,0x1a0)&32)},{"hidden_in_game",C::Bool(Value<uint8_t>(p,0x1a1)&8)}});
        std::set<uintptr_t> active;C::JsonArray chain;
        directTransforms[n.Id]=AuditNativeChain(p,root,active,0,chain);
        MeshInput input;bool defaultsValid=false,overridesValid=false;
        const auto defaults=AuditMaterials(mesh+0x160,0x38,defaultsValid,input.Defaults);
        const auto overrides=AuditMaterials(p+0x530,8,overridesValid,input.Overrides);
        const auto proposed=A::ProposedMaterials(defaults,overrides);input.Valid=defaultsValid&&overridesValid&&!proposed.empty();
        C::JsonArray mapping;for(const auto& slot:proposed)mapping.Add(C::Object({{"slot",std::to_string(slot.Index)},{"material_id",slot.Valid?C::Quote(slot.Material):"null"}}));
        input.Mapping=mapping.String();out.MaterialSlots+=defaults.size();out.OverrideSlots+=overrides.size();
        auto& r=recipe.Instances[recipe.Count];recipeIndexes[n.Id]=recipe.Count++;
        r.Mesh=mesh;r.ComponentIndex=uint32_t(Value<int32_t>(p,0xc));r.Visible=(Value<uint8_t>(p,0x1a0)&32)!=0;r.Hidden=(Value<uint8_t>(p,0x1a1)&8)!=0;
        r.Verified=input.Valid&&proposed.size()<=MiniMapStaticAssembly::Slots&&Read(mesh,0x240,r.LocalBounds.Origin)&&Read(mesh,0x258,r.LocalBounds.Extent);
        r.MaterialCount=(std::min)(proposed.size(),MiniMapStaticAssembly::Slots);
        for(const auto& slot:proposed)
        {
            const auto found=AuditMaterialPointers.find(slot.Material);
            if(slot.Index>=r.MaterialCount||!slot.Valid||found==AuditMaterialPointers.end()){r.Verified=false;input.Valid=false;break;}
            r.Materials[slot.Index]=found->second;
            for(const auto& over:overrides)if(over.Index==slot.Index&&over.Valid&&!over.Material.empty())r.Overrides[slot.Index]=true;
        }
        out.Materials&=input.Valid;materials.emplace(n.Id,std::move(input));nodes.emplace(n.Id,std::move(n));
    }
    for(const auto& [id,n]:nodes)
    {
        if(n.Kind=="none")continue;
        auto& count=n.Kind=="static"?out.Static:out.Skeletal;
        if(count>=(n.Kind=="static"?A::StaticLimit:A::SkeletalLimit)){AuditTruncated=true;continue;}++count;
        std::set<std::string> active;size_t remaining=A::VisitLimit-AuditVisits;
        const auto transform=directTransforms.contains(id)?directTransforms.at(id):A::Resolve(id,nodes,native,active,0,&remaining);AuditVisits=A::VisitLimit-remaining;
        if(transform.Status=="truncated")AuditTruncated=true;
        if(n.Kind=="static"){out.Transforms&=transform.Status=="verified";if(!n.Mesh.empty())meshes.insert(n.Mesh);}
        if(const auto found=recipeIndexes.find(id);found!=recipeIndexes.end())
        {auto& r=recipe.Instances[found->second];r.Transform=transform.Effective;r.Verified&=transform.Status=="verified";}
        const auto& m=materials.at(id);
        records.Add(C::Object({{"component_id",C::Quote(n.Component)},{"scs_node_id",C::Quote(id)},
            {"source",C::Quote(directTransforms.contains(id)?"verified_direct_cdo_component_field":"selected_actor_scs_template")},
            {"kind",C::Quote(n.Kind)},{"mesh_id",n.Mesh.empty()?"null":C::Quote(n.Mesh)},
            {"parent_scs_node_id",n.Parent.empty()?"null":C::Quote(n.Parent)},{"native_parent_id",n.NativeParent.empty()?"null":C::Quote(n.NativeParent)},
            {"local_transform",n.InputsValid?A::Json(n.Local):"null"},
            {"effective_root_relative_transform",transform.Status=="verified"?A::Json(transform.Effective):"null"},
            {"transform_status",C::Quote(transform.Status)},{"transform_reason",C::Quote(transform.Reason)},
            {"socket",C::Bool(n.Socket)},{"absolute",C::Bool(n.Absolute)},{"template_attachment",C::Bool(n.TemplateAttachment)},
            {"authored_visibility",n.Metadata},
            {"authored_material_slots",m.Defaults.empty()?"[]":m.Defaults},
            {"component_material_overrides",m.Overrides.empty()?"[]":m.Overrides},{"proposed_effective_slots",m.Mapping}}));
    }
    out.Nodes=AuditNodes.size();out.UniqueMeshes=meshes.size();out.Inherited=GenericInherited;out.Child=GenericChild;
    out.Transforms&=sourceValid;out.Materials&=sourceValid;
    out.Truncated=Truncated||Invalid||Graph.Truncated||Work.Exhausted||AuditTruncated||AuditCollectionTruncated||
        records.Truncated||nativeRecords.Truncated||Sources.Truncated||Components.Truncated||Mass.Truncated||Relationships.Truncated||Candidates.Truncated;
    std::string scsStatus="not_blueprint_class";
    if(Is(AuditClass,Blueprint,0x360))
    {
        const auto scs=Value<uintptr_t>(AuditClass,0x268);
        scsStatus=!scs?"absent":Is(scs,SCS,0xb0)?"resident_validated":"invalid_or_unavailable";
    }
    out.Truncated|=Work.Exhausted;
    out.Details=C::Object({{"existing_cdo_status",C::Quote(cdo?"resident_validated":"unavailable_or_invalid")},
        {"selected_class_scs_status",C::Quote(scsStatus)},
        {"selected_class",Selected},{"native_candidates",nativeRecords.String()},
        {"instances",records.String()}, {"representation_alternatives",Candidates.String().size()<2048?Candidates.String():"[]"},
        {"alternative_details_truncated",C::Bool(Candidates.String().size()>=2048)},
        {"mass_mesh_references_examined",std::to_string(ProbeResult.MeshReferences)},
        {"bounded_scs_nodes_examined",std::to_string(NodeCount)},
        {"native_default_components",C::Quote(CaptureRequested?"verified_direct_cdo_fields_only; other_native_components_not_enumerated":"not_attempted_sparse_set_bound_unverified")},
        {"mixed_native_scs_assembly_unverified",C::Bool(mixedNative)},
        {"inherited_overrides",C::Quote(GenericInherited?"present_effective_selection_unverified":"none_encountered_in_bounded_traversal")},
        {"source_visibility",C::Quote("authored_inputs_only; live state unverified")},
        {"classification",C::Quote("unknown")},{"live_actor_equivalence",C::Quote("not_established")},
        {"customization_application",C::Quote("copied tint key only; not applied to template materials")},
        {"source_retention_for_future_capture",C::Quote("not_attempted; all audit roots released synchronously")}});
    if(out.Details.size()>V::DetailLimit){out.Details="{\"truncated\":true}";out.Truncated=true;}
    out.Status=V::Classify(out,Record.HasSpline);
    if(mixedNative)out.Status=V::Outcome::Conditional;
    out.Reason=mixedNative?"native_and_scs_static_routes_both_present; additive_equivalence_unverified":out.Status==V::Outcome::Eligible?"bounded_selected_class_authored_static_inputs; future_capture_retention_and_lifecycle_gate_required":
        Record.HasSpline?"instance_specific_spline_geometry":!out.Static?"no_selected_scs_static_meshes; native_only_or_skeletal_or_procedural_path_unsupported":
        out.Truncated?"bounded_traversal_or_output_incomplete":!out.Root?"native_root_unavailable":
        out.Inherited?"inherited_component_selection_unverified":out.Child?"child_actor_contributions_not_composed":
        out.OtherSkinned?"other_skinned_component_not_audited":out.DerivedStatic?"derived_static_component_extra_geometry_semantics_unverified":
        !out.Transforms?"attachment_socket_absolute_or_transform_unresolved":"stored_material_inputs_incomplete";
#if defined(MINIMAP_BUILDING_SPRITES)
    if(CaptureRequested&&out.Status==V::Outcome::Eligible)
    {
        recipe.Complete=!out.Truncated;
        MiniMapSpriteCapture::Candidate candidate;candidate.General=true;
        candidate.Identity=MiniMapSpriteCandidates::Copy(Record).Identity;
        candidate.Classification=Classification.CachedPolicy;
        candidate.ClassificationObserved=Classification.Fixed&&Classification.Subclass&&!Classification.Truncated&&Classification.InteriorTag!="unknown";
        candidate.RepresentationVerified=out.Root&&out.Transforms&&out.Materials&&!out.Inherited&&!out.Child&&!out.DerivedStatic&&!out.OtherSkinned;
        candidate.Truncated=out.Truncated||Classification.Truncated;
        candidate.ActorObject=Value<int32_t>(AuditClass,0xc);const auto name=Value<Name>(AuditClass,0x18);
        candidate.ActorName=name.Index;candidate.ActorNumber=name.Number;
        candidate.Tier=recipe.HighDetail?MiniMapSpriteDiscoveryCore::Tier::High:MiniMapSpriteDiscoveryCore::Tier::Low;
        const auto decision=MiniMapSmelterCapture::OfferStaticAssembly(candidate,Record,recipe,MiniMapMap::GetWorld());
        out.Reason=decision.Reason;
        if(!decision.Allowed())out.Status=V::Outcome::Conditional;
    }
#endif
    return out;
}
