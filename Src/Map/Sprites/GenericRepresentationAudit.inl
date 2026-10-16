// Bounded Context-local audit; capture handoff, when requested, acquires its own roots.
MiniMapSpriteValidation::Result GenericAudit()
{
#if defined(MINIMAP_BUILDING_SPRITES)
    MiniMapBuildingSprites::TimingScope recipeTiming(MiniMapBuildingSprites::Timing::AssemblyRecipe);
#endif
    AuditElapsed recipeElapsed(Metrics?&Metrics->RecipeNs:nullptr);
    namespace V=MiniMapSpriteValidation;
    V::Result out;out.Resident=AuditClass!=0;out.Route=SelectedTier;out.Selected=Selected;
    AuditActive=true;
    struct End {bool& Active;~End(){Active=false;}} end{AuditActive};
    C::JsonArray nativeRecords,records;records.Limit=12*1024;nativeRecords.Limit=2048;
    std::vector<A::NamedCandidate> names;std::map<std::string,uintptr_t> pointers;
    std::map<std::string,A::Node> nodes;std::map<std::string,A::Result> native;
    uintptr_t cdo=0,root=0,nativeMain=0;bool authoredMainMeshUnresolved=false;
    if(AuditClass&&ClassObject(AuditClass)&&Derives(AuditClass,Actor))
    {
        cdo=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UClass*>(AuditClass)->ClassDefaultObject);
        if(!Is(cdo,AuditClass)||!(uint32_t(reinterpret_cast<const SDK::UObject*>(cdo)->Flags)&0x10)||!Hold(cdo))cdo=0;
    }
    if(cdo)
    {
        root=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::AActor*>(cdo)->RootComponent);
        if(!Is(root,Scene)||!Hold(root))root=0;
        const auto buildingBase=NativeClass(L"/Script/Chimera.CrBuildingActorBase");
        const bool building=buildingBase&&Derives(AuditClass,buildingBase)&&Is(cdo,AuditClass);
        // An authored building CDO may expose connectors before native initialization
        // establishes its main mesh. Those templates alone cannot certify the body.
        if(AuthoredFallback)
        {
            if(!buildingBase)authoredMainMeshUnresolved=true;
            else if(Derives(AuditClass,buildingBase))
            {
                const auto main=building?reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::ACrBuildingActorBase*>(cdo)->MainMeshComponent):0;
                nativeMain=main;
                authoredMainMeshUnresolved=!main||!Is(main,StaticComponent)||!Hold(main)||
                    !Is(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStaticMeshComponent*>(main)->StaticMesh),StaticMesh);
            }
        }
        const std::pair<uintptr_t,const char*> fields[]={{reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::AActor*>(cdo)->RootComponent),"RootComponent"},{building?reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::ACrBuildingActorBase*>(cdo)->MainComponent):0,"MainComponent"},{building?reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::ACrBuildingActorBase*>(cdo)->MainMeshComponent):0,"MainMeshComponent"}};
        for(size_t f=0;f<(building?3u:1u);++f)
        {
            if(!AuditSpend())break;
            const auto p=fields[f].first;
            const bool ok=p&&Is(p,f==2?StaticComponent:Scene)&&Hold(p);
            const auto name=ok?reinterpret_cast<const SDK::UObject*>(p)->Name:SDK::FName{};
            nativeRecords.Add(C::Object({{"field",C::Quote(fields[f].second)},
                {"status",C::Quote(!p?"absent":ok?"verified":"invalid")},{"component",ok?Ref(p,Scene):"null"}}));
            if(ok){const auto id=Id(p);names.push_back({id,{name.ComparisonIndex,name.Number}});pointers[id]=p;}
        }
    }
    out.Root=root!=0;out.Transforms=true;out.Materials=true;
    bool sourceValid=true;std::set<std::string> meshes;
    struct MeshInput {std::string Defaults,Overrides,Mapping;bool Valid=false;};
    std::map<std::string,MeshInput> materials;
    MiniMapStaticAssembly::Recipe recipe;recipe.General=true;recipe.Generation=Record.Generation;
    struct SkeletalSource {uintptr_t Component{},Mesh{};bool Valid=false;};
    std::array<SkeletalSource,A::SkeletalLimit> skeletalSources{};size_t skeletalSourceCount=0;
    bool skeletalSourcesTruncated=false;
    recipe.ActorClass=AuditClass;recipe.RootVerified=out.Root;recipe.HighDetail=SelectedTier=="high_detail_actor";
    // A separate body route is permitted only after proving that every other
    // discovered static template is noncontributing. Unresolved visible inputs
    // never become omissions, and exact component identity is the only duplicate rule.
    const bool nativeBodyConsidered=AuthoredFallback&&CaptureRequested&&(nativeMain||authoredMainMeshUnresolved);
    bool nativeBodySelected=nativeBodyConsidered&&!authoredMainMeshUnresolved&&out.Root&&
        Is(nativeMain,StaticComponent)&&Hold(nativeMain)&&
        reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(nativeMain)->Class)==StaticComponent;
    size_t omittedHidden=0,omittedNull=0,duplicates=0,unresolvedVisible=0;
    bool unresolvedSkinned=false;
    std::set<uintptr_t> bodyOmissions,bodyExamined;
    if(nativeBodyConsidered)
    {
        const auto* visibility=reinterpret_cast<const SDK::USceneComponent*>(nativeMain);
        nativeBodySelected=nativeBodySelected&&visibility->bVisible&&!visibility->bHiddenInGame;
        auto examine=[&](uintptr_t component)
        {
            if(!bodyExamined.insert(component).second)return;
            if(component==nativeMain)return;
            if(!Is(component,StaticComponent)||!Hold(component))
            {++unresolvedVisible;nativeBodySelected=false;return;}
            const auto* visibility=reinterpret_cast<const SDK::USceneComponent*>(component);
            const auto mesh=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStaticMeshComponent*>(component)->StaticMesh);
            if(!visibility->bVisible||visibility->bHiddenInGame){++omittedHidden;bodyOmissions.insert(component);return;}
            // A null base mesh does not prove that a derived/instanced component
            // contributes no geometry. Its additional semantics remain unsupported.
            if(!mesh&&reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class)==StaticComponent&&
                (!AuditAssignments.contains(component)||!AuditAssignments.at(component)))
            {++omittedNull;bodyOmissions.insert(component);return;}
            ++unresolvedVisible;nativeBodySelected=false;
        };
        for(const auto& observation:StaticObservations)
        {if(observation.Component==nativeMain)++duplicates;examine(observation.Component);}
        for(const auto& [_,component]:pointers)
            if(Is(component,StaticComponent))examine(component);
        // ComponentTemplates can contain geometry outside the selected SCS list.
        // This static-body route cannot certify any such skinned contribution.
        for(const auto component:ComponentSet)
            if(Is(component,SkinnedComponent))unresolvedSkinned=true;
        nativeBodySelected&=!StaticObservationOverflow&&!GenericInherited&&!GenericChild&&!unresolvedSkinned;
    }
    std::map<std::string,size_t> recipeIndexes;
    size_t omittedDerived=0,omittedNative=0;
    bool subsetSelected=false;
    MiniMapStaticAssembly::SubsetSelection subset;
    std::string skeletalTestNode;
    // First capture-requested skeletal candidate only; reuse evaluated audit data.
    static bool skeletalAdmissionReported=false;
    static size_t skeletalSummaryReports=0;
    struct AdmissionDiagnostic
    {
        std::string Node,TransformStatus="not_evaluated",TransformReason="not_evaluated";
        bool Asset=false,Defaults=false,Overrides=false,Materials=false,Capacity=false,Exact=false,
            Derives=false,Visible=false,Hidden=true,SlotPointers=true,Admitted=false;
        size_t MaterialCount=0;
    } skeletalAdmission;

    std::map<std::string,A::Result> directTransforms;
    for(const auto p:AuditNodes)
    {
        if(!AuditSpend()||!Is(p,Node)||!Hold(p)){sourceValid=false;break;}
        A::Node n;n.Id=Id(p);
        const auto* node=reinterpret_cast<const SDK::USCS_Node*>(p);
        const auto component=reinterpret_cast<uintptr_t>(node->ComponentTemplate),declared=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(node->ComponentClass));
        if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared)||!Hold(component))
        {sourceValid=false;continue;}
        n.Component=Id(component);n.ParentConflict=AuditConflictingParents.contains(n.Id);
        if(const auto found=AuditParents.find(n.Id);found!=AuditParents.end())n.Parent=found->second;
        const auto attach=reinterpret_cast<const SDK::USCS_Node*>(p)->AttachToName;n.Socket=attach.ComparisonIndex!=0||attach.Number!=0;
        if(uint8_t(reinterpret_cast<const SDK::USCS_Node*>(p)->bIsParentComponentNative)!=0)
        {
            if(!n.Parent.empty())n.ParentConflict=true;
            const auto parent=reinterpret_cast<const SDK::USCS_Node*>(p)->ParentComponentOrVariableName;bool ambiguous=false;
            n.NativeParent=A::Match({parent.ComparisonIndex,parent.Number},names,ambiguous);
            if(ambiguous)n.ParentConflict=true;
            if(!ambiguous&&!n.NativeParent.empty()&&root)
            {std::set<uintptr_t> active;C::JsonArray chain;native[n.NativeParent]=AuditNativeChain(pointers.at(n.NativeParent),root,active,0,chain);}
        }
        n.Scene=Is(component,Scene);bool socket=false;
        if(n.Scene){n.InputsValid=AuditTransform(component,n.Local,n.Absolute,socket);n.TemplateAttachment=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USceneComponent*>(component)->AttachParent)!=0;}
        n.Socket|=socket;
        n.Metadata=n.Scene?C::Object({{"visible",C::Bool(reinterpret_cast<const SDK::USceneComponent*>(component)->bVisible)},
            {"hidden_in_game",C::Bool(reinterpret_cast<const SDK::USceneComponent*>(component)->bHiddenInGame)}}):"null";
        const bool isStatic=Is(component,StaticComponent),isSkeletal=!isStatic&&Is(component,SkeletalComponent);
        const auto* visibility=reinterpret_cast<const SDK::USceneComponent*>(component);

        const bool nonDrawable=isStatic&&MiniMapStaticAssembly::AuthoredStaticMembership(
            reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class)==StaticComponent,true,isStatic&&visibility->bVisible,isStatic&&visibility->bHiddenInGame)==
                MiniMapStaticAssembly::DrawableMembership::Noncontributing;
        // A subclass may contain spline/instance/procedural geometry. Its base
        // mesh field alone cannot certify an ordinary rigid-mesh recipe.
        const bool bodyOmitted=nativeBodySelected&&isStatic&&
            (bodyOmissions.contains(component)||component==nativeMain);
        const bool derived=isStatic&&!bodyOmitted&&reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class)!=StaticComponent;
        if(derived)out.DerivedStatic=true;
        const bool omitDerived=CaptureRequested&&derived;
        n.Kind=isStatic&&!bodyOmitted&&!omitDerived?"static":isSkeletal?"skeletal":"none";
        if(omitDerived)
        {
            // Keep the hierarchy node for independently resolvable children.
            // No instance/spline/procedural placement is inferred from the base mesh.
            ++omittedDerived;
            records.Add(C::Object({{"component_id",C::Quote(n.Component)},{"scs_node_id",C::Quote(n.Id)},
                {"component_class",Ref(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class),Class)},
                {"drawable_capture_member","false"},{"omission",C::Quote("derived_static_rendering_semantics_unverified")}}));
        }
        if((isStatic&&!bodyOmitted&&!omitDerived)||isSkeletal)
        {
            const auto found=AuditAssignments.find(component);const uintptr_t mesh=found==AuditAssignments.end()?0:found->second;
            const bool valid=mesh&&Is(mesh,isStatic?StaticMesh:SkeletalMesh)&&Hold(mesh);
            // Preserve source tokens before material rejection; no admission state changes.
            if(isSkeletal)
            {
                if(skeletalSourceCount<skeletalSources.size())skeletalSources[skeletalSourceCount++]={component,mesh,valid};
                else skeletalSourcesTruncated=true;
            }
            if(!valid)sourceValid=false;else n.Mesh=Id(mesh);
            bool defaultsValid=false,overridesValid=false;MeshInput input;
            const bool diagnoseMaterials=CaptureRequested&&!skeletalAdmissionReported&&isSkeletal&&skeletalAdmission.Node.empty();
            const auto defaults=valid?(isStatic?AuditMaterials(reinterpret_cast<const SDK::UStaticMesh*>(mesh)->StaticMaterials,defaultsValid,input.Defaults,diagnoseMaterials):AuditMaterials(reinterpret_cast<const SDK::USkeletalMesh*>(mesh)->Materials,defaultsValid,input.Defaults,diagnoseMaterials)):std::vector<A::Slot>{};
            const auto overridden=AuditMaterials(reinterpret_cast<const SDK::UMeshComponent*>(component)->OverrideMaterials,overridesValid,input.Overrides,diagnoseMaterials);
            const auto proposed=A::ProposedMaterials(defaults,overridden);C::JsonArray mapping;
            bool present=!proposed.empty();
            for(const auto& slot:proposed){present&=slot.Valid;mapping.Add(C::Object({{"slot",std::to_string(slot.Index)},{"material_id",slot.Valid?C::Quote(slot.Material):"null"}}));}
            DiagnosticTruncated|=mapping.Truncated;
            input.Mapping=mapping.String();input.Valid=defaultsValid&&overridesValid&&present;
            // Only the first capture-requested skeletal component. The helper
            // reports the same validation/retention decisions used for admission.
            if(diagnoseMaterials)
            {
                LOG_INFO("MiniMap: skeletal-only material inputs A3.4b: generation=%llu building=%u defaultsValid=%d defaults=%s overridesValid=%d overrides=%s",
                    (unsigned long long)Record.Generation,Record.BuildingId,int(defaultsValid),input.Defaults.c_str(),
                    int(overridesValid),input.Overrides.c_str());
                size_t reported=0;
                for(const auto& slot:proposed)
                {
                    if(reported++>=MiniMapStaticAssembly::Slots)break;
                    const auto def=std::find_if(defaults.begin(),defaults.end(),[&](const auto& s){return s.Index==slot.Index;});
                    const auto over=std::find_if(overridden.begin(),overridden.end(),[&](const auto& s){return s.Index==slot.Index;});
                    const bool useOverride=over!=overridden.end()&&over->Valid&&!over->Material.empty();
                    const bool mapped=slot.Valid&&AuditMaterialPointers.contains(slot.Material);
                    LOG_INFO("MiniMap: skeletal-only effective material: slot=%zu defaultSlotPresent=%d defaultValid=%d overrideSlotPresent=%d overrideValid=%d selectedSource=%s selectedValid=%d pointerMapped=%d failure=%s",
                        slot.Index,int(def!=defaults.end()),int(def!=defaults.end()&&def->Valid),int(over!=overridden.end()),
                        int(over!=overridden.end()&&over->Valid),useOverride?"component_override":def!=defaults.end()?"mesh_default":"none",
                        int(slot.Valid),int(mapped),!slot.Valid?"effective_slot_absent_or_invalid":!mapped?"pointer_mapping_missing":"none");
                }
            }
            // Keep the node, attachment/material checks and transform resolution;
            // only its own proven noncontributing drawable is omitted.
            if(isStatic&&!nonDrawable)
            {
                if(recipe.Count>=MiniMapStaticAssembly::Limit)AuditTruncated=true;
                else
                {
                    recipeIndexes[n.Id]=recipe.Count;auto& r=recipe.Instances[recipe.Count++];
                    r.ComponentIndex=uint32_t(int32_t(reinterpret_cast<const SDK::UObject*>(component)->Index));r.Mesh=valid?mesh:0;
                    r.Visible=reinterpret_cast<const SDK::USceneComponent*>(component)->bVisible;r.Hidden=reinterpret_cast<const SDK::USceneComponent*>(component)->bHiddenInGame;
                    r.Verified=valid&&input.Valid&&proposed.size()<=MiniMapStaticAssembly::Slots&&Is(mesh,StaticMesh);
                    if(r.Verified){const auto& b=reinterpret_cast<const SDK::UStaticMesh*>(mesh)->ExtendedBounds;
                        r.LocalBounds.Origin={b.Origin.X,b.Origin.Y,b.Origin.Z};r.LocalBounds.Extent={b.BoxExtent.X,b.BoxExtent.Y,b.BoxExtent.Z};}
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
            if(CaptureRequested&&!skeletalAdmissionReported&&isSkeletal&&skeletalAdmission.Node.empty())
            {
                auto& d=skeletalAdmission;d.Node=n.Id;d.Asset=valid;d.Defaults=defaultsValid;d.Overrides=overridesValid;
                d.Materials=input.Valid;d.MaterialCount=proposed.size();d.Capacity=proposed.size()<=MiniMapStaticAssembly::Slots;
                d.Exact=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class)==SkeletalComponent;d.Derives=isSkeletal; // Is already validated inheritance
                d.Visible=reinterpret_cast<const SDK::USceneComponent*>(component)->bVisible;d.Hidden=reinterpret_cast<const SDK::USceneComponent*>(component)->bHiddenInGame;
                for(const auto& slot:proposed)
                    d.SlotPointers&=slot.Index<proposed.size()&&slot.Valid&&AuditMaterialPointers.contains(slot.Material);
            }
            // Retain no new roots here: the capture owner independently validates
            // and retains this optional record before the synchronous audit ends.
            if(isSkeletal&&!recipe.SkeletalTest.Present&&valid&&input.Valid&&
                proposed.size()<=MiniMapStaticAssembly::Slots&&reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class)==SkeletalComponent)
            {
                auto& test=recipe.SkeletalTest;auto& r=test.Input;
                test.Present=true;test.Template=component;test.ComponentClass=SkeletalComponent;skeletalTestNode=n.Id;
                r.Mesh=mesh;r.ComponentIndex=uint32_t(int32_t(reinterpret_cast<const SDK::UObject*>(component)->Index));
                r.Visible=reinterpret_cast<const SDK::USceneComponent*>(component)->bVisible;r.Hidden=reinterpret_cast<const SDK::USceneComponent*>(component)->bHiddenInGame;
                r.Verified=true;r.MaterialCount=proposed.size();
                for(const auto& slot:proposed)
                {
                    const auto found=AuditMaterialPointers.find(slot.Material);
                    if(slot.Index>=r.MaterialCount||!slot.Valid||found==AuditMaterialPointers.end()){r.Verified=false;break;}
                    r.Materials[slot.Index]=found->second;
                    for(const auto& over:overridden)if(over.Index==slot.Index&&over.Valid&&!over.Material.empty())r.Overrides[slot.Index]=true;
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
    // Observer runs before the mixed-route skip, on independent validation budgets.
    try{out.Inspection=InspectStaticInventory(nodes,native);}
    catch(...){LOG_WARN("MiniMap: native static inspection unavailable; production admission unchanged");}
    const bool scsStatic=std::any_of(nodes.begin(),nodes.end(),[](const auto& item){return item.second.Kind=="static";});
    bool mixedNative=false;
    for(const auto& [componentId,p]:pointers)
    {
        if(!CaptureRequested)break; // preserve the R2.6b observational schema/counts
        bool duplicate=false;for(const auto& [_,n]:nodes)duplicate|=n.Component==componentId&&
            !(nativeBodySelected&&p==nativeMain&&n.Kind=="none");
        if(nativeBodySelected&&p!=nativeMain)continue;
        if(duplicate||!Is(p,StaticComponent))continue;
        if(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(p)->Class)!=StaticComponent){out.DerivedStatic=true;continue;}
        const auto mesh=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStaticMeshComponent*>(p)->StaticMesh);if(!mesh)continue;
        if(scsStatic){mixedNative=true;++omittedNative;continue;} // Select the SCS route; never guess additive equivalence.
        if(!Is(mesh,StaticMesh)||!Hold(mesh)){sourceValid=false;continue;}
        if(recipe.Count>=MiniMapStaticAssembly::Limit){AuditTruncated=true;break;}
        A::Node n;n.Id="native:"+componentId;n.Component=componentId;n.Kind="static";n.Scene=true;n.Mesh=Id(mesh);
        bool socket=false;n.InputsValid=AuditTransform(p,n.Local,n.Absolute,socket);n.Socket=socket;
        n.Metadata=C::Object({{"source",C::Quote("verified_direct_cdo_component_field")},
            {"visible",C::Bool(reinterpret_cast<const SDK::USceneComponent*>(p)->bVisible)},{"hidden_in_game",C::Bool(reinterpret_cast<const SDK::USceneComponent*>(p)->bHiddenInGame)}});
        std::set<uintptr_t> active;C::JsonArray chain;
        directTransforms[n.Id]=AuditNativeChain(p,root,active,0,chain);
        MeshInput input;bool defaultsValid=false,overridesValid=false;
        const auto defaults=AuditMaterials(reinterpret_cast<const SDK::UStaticMesh*>(mesh)->StaticMaterials,defaultsValid,input.Defaults);
        const auto overrides=AuditMaterials(reinterpret_cast<const SDK::UMeshComponent*>(p)->OverrideMaterials,overridesValid,input.Overrides);
        const auto proposed=A::ProposedMaterials(defaults,overrides);input.Valid=defaultsValid&&overridesValid&&!proposed.empty();
        C::JsonArray mapping;for(const auto& slot:proposed)mapping.Add(C::Object({{"slot",std::to_string(slot.Index)},{"material_id",slot.Valid?C::Quote(slot.Material):"null"}}));
        DiagnosticTruncated|=mapping.Truncated;
        input.Mapping=mapping.String();out.MaterialSlots+=defaults.size();out.OverrideSlots+=overrides.size();
        auto& r=recipe.Instances[recipe.Count];recipeIndexes[n.Id]=recipe.Count++;
        r.Mesh=mesh;r.ComponentIndex=uint32_t(int32_t(reinterpret_cast<const SDK::UObject*>(p)->Index));r.Visible=reinterpret_cast<const SDK::USceneComponent*>(p)->bVisible;r.Hidden=reinterpret_cast<const SDK::USceneComponent*>(p)->bHiddenInGame;
        const auto& b=reinterpret_cast<const SDK::UStaticMesh*>(mesh)->ExtendedBounds;
        r.LocalBounds.Origin={b.Origin.X,b.Origin.Y,b.Origin.Z};r.LocalBounds.Extent={b.BoxExtent.X,b.BoxExtent.Y,b.BoxExtent.Z};
        r.Verified=input.Valid&&proposed.size()<=MiniMapStaticAssembly::Slots;
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
        if(id==skeletalTestNode)
        {auto& r=recipe.SkeletalTest.Input;r.Transform=transform.Effective;r.Verified&=transform.Status=="verified";}
        if(id==skeletalAdmission.Node)
        {skeletalAdmission.TransformStatus=transform.Status;skeletalAdmission.TransformReason=transform.Reason;skeletalAdmission.Admitted=id==skeletalTestNode;}
        const auto& m=materials.at(id);
        records.Add(C::Object({{"component_id",C::Quote(n.Component)},{"scs_node_id",C::Quote(id)},
            {"source",C::Quote(directTransforms.contains(id)?"verified_direct_cdo_component_field":"selected_actor_scs_template")},
            {"kind",C::Quote(n.Kind)},{"mesh_id",n.Mesh.empty()?"null":C::Quote(n.Mesh)},
            {"recipe_input_collected",C::Bool(recipeIndexes.contains(id))},
            {"drawable_capture_member",C::Bool(recipeIndexes.contains(id)&&recipe.Instances[recipeIndexes.at(id)].Verified)},
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
    out.Truncated=Truncated||Invalid||Graph.Truncated||Work.Exhausted||AuditTruncated||AuditCollectionTruncated;
    out.DiagnosticTruncated=DiagnosticTruncated||records.Truncated||nativeRecords.Truncated||Sources.Truncated||Components.Truncated||Mass.Truncated||Relationships.Truncated||Candidates.Truncated;
    std::string scsStatus="not_blueprint_class";
    if(Is(AuditClass,Blueprint))
    {
        const auto scs=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UBlueprintGeneratedClass*>(AuditClass)->SimpleConstructionScript);
        scsStatus=!scs?"absent":Is(scs,SCS)?"resident_validated":"invalid_or_unavailable";
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
        {"native_main_body_selection",C::Object({{"considered",C::Bool(nativeBodyConsidered)},
            {"noncontributing_static_inventory_verified",C::Bool(nativeBodySelected)},
            {"omitted_hidden_or_invisible",std::to_string(omittedHidden)},
            {"omitted_null_mesh",std::to_string(omittedNull)},{"exact_main_duplicates",std::to_string(duplicates)},
            {"unresolved_visible_static",std::to_string(unresolvedVisible)},
            {"unresolved_skinned_contribution",C::Bool(unresolvedSkinned)},
            {"visual_equivalence",C::Quote("not_established; experimental static body")}})},
        {"inherited_overrides",C::Quote(GenericInherited?"present_effective_selection_unverified":"none_encountered_in_bounded_traversal")},
        {"source_visibility",C::Quote("authored_inputs_only; live state unverified")},
        {"classification",C::Quote("unknown")},{"live_actor_equivalence",C::Quote("not_established")},
        {"customization_application",C::Quote("copied tint key only; not applied to template materials")},
        {"source_retention_for_future_capture",C::Quote("not_attempted; all audit roots released synchronously")}});
    out.DiagnosticTruncated|=Candidates.String().size()>=2048;
    V::BoundDiagnosticDetails(out);
    out.Status=V::Classify(out,Record.HasSpline);
    if(mixedNative||authoredMainMeshUnresolved)out.Status=V::Outcome::Conditional;
    out.Reason=authoredMainMeshUnresolved?"authored_class_main_mesh_unresolved; connector_templates_cannot_certify_building_body":mixedNative?"native_and_scs_static_routes_both_present; additive_equivalence_unverified":out.Status==V::Outcome::Eligible?"bounded_selected_class_authored_static_inputs; future_capture_retention_and_lifecycle_gate_required":
        Record.HasSpline?"instance_specific_spline_geometry":!out.Static?"no_selected_scs_static_meshes; native_only_or_skeletal_or_procedural_path_unsupported":
        out.Truncated?"bounded_operational_evidence_incomplete":!out.Root?"native_root_unavailable":
        out.Inherited?"inherited_component_selection_unverified":out.Child?"child_actor_contributions_not_composed":
        out.OtherSkinned?"other_skinned_component_not_audited":out.DerivedStatic?"derived_static_component_extra_geometry_semantics_unverified":
        !out.Transforms?"attachment_socket_absolute_or_transform_unresolved":"stored_material_inputs_incomplete";
    if(nativeBodyConsidered)
    {
        // Skeletal contributions are not part of this independent static-body
        // proof. Keep the existing diagnostic recipe, but do not omit them here.
        if(!nativeBodySelected||out.Skeletal||StaticObservationOverflow)
        {
            out.Status=V::Outcome::Conditional;
            out.Reason=!nativeMain?"native_main_body_missing":
                GenericInherited?"native_main_body_inherited_contribution_unresolved":
                GenericChild?"native_main_body_child_contribution_unresolved":
                unresolvedSkinned?"native_main_body_skinned_contribution_unresolved":
                out.Skeletal?"native_main_body_skeletal_contribution_unresolved":
                StaticObservationOverflow?"native_main_body_static_inventory_incomplete":
                unresolvedVisible?"native_main_body_additional_visible_static_unresolved":
                "native_main_body_visibility_or_identity_unverified";
        }
        else if(out.Status==V::Outcome::Eligible)
        {
            out.Route="native_main_body_static";
            out.Reason="experimental_native_main_body_static; live_actor_equivalence_unverified";
        }
        bool report=false;
        {
            std::scoped_lock lock(blueprintIdentityAccess);
            if(BlueprintIdentity.Path&&blueprintIdentities.Generation==Record.Generation&&blueprintIdentities.NativeBodyReports<64)
                if(auto* path=blueprintIdentities.FindPath(*BlueprintIdentity.Path);path&&!path->NativeBodyReported)
                {path->NativeBodyReported=true;++blueprintIdentities.NativeBodyReports;report=true;}
        }
        if(report)LOG_INFO("MiniMap: native-main-body selection: generation=%llu path=%s considered=1 mainInputsVerified=%d materials=%d transform=%d omittedHidden=%zu omittedNullMesh=%zu exactDuplicates=%zu unresolvedVisible=%zu inheritedUnresolved=%d outcome=%s source=%s reason=%s; experimental static body; live equivalence unverified",
            (unsigned long long)Record.Generation,BlueprintIdentity.Path->c_str(),int(nativeBodySelected&&out.Materials&&out.Transforms&&!out.Truncated),int(out.Materials),int(out.Transforms),
            omittedHidden,omittedNull,duplicates,unresolvedVisible,int(GenericInherited),V::Label(out.Status),out.Route.c_str(),out.Reason.c_str());
    }
    if(CaptureRequested&&out.Status==V::Outcome::Conditional&&!Record.HasSpline)
    {
        recipe.Complete=!out.Truncated;
        // A missing native main body leaves visual completeness unverified.
        // Independently verified SCS members still use the existing subset safety checks.
        subsetSelected=V::ValidateSupportedSubset(out,recipe,
            sourceValid&&!StaticObservationOverflow,subset);
        if(subsetSelected)
        {
            out.Route="supported_static_subset";
            out.Reason="verified_static_subset; omitted_contributions_visual_completeness_unverified";
            // This describes deliberate selection separately from traversal loss.
            out.Details=C::Object({
                {"bounded_audit",out.Details},
                {"supported_static_subset",C::Object({
                    {"included",std::to_string(recipe.Count)},
                    {"omitted_derived",std::to_string(omittedDerived)},
                    {"omitted_distinct_native_route",std::to_string(omittedNative)},
                    {"omitted_unverified_or_nondrawable_inputs",std::to_string(subset.Omitted)},
                    {"exact_equivalent_duplicates",std::to_string(subset.Duplicates)},
                    {"visual_completeness_verified","false"}
                })}
            });
            V::BoundDiagnosticDetails(out);
        }
    }
    // Lifetime extends through the synchronous offer, which independently retains
    // every included asset. Never borrow roots from an exhausted SCS audit scope.
    std::unique_ptr<Context> independentNative;
    bool nativeSubsetSelected=false;
    if(CaptureRequested&&out.Status==V::Outcome::Conditional&&!Record.HasSpline&&
        ProbeResult.DefinitionIdentity=="valid"&&!ProbeResult.Truncated&&!ProbeResult.Incomplete)
    {
        independentNative=std::make_unique<Context>();auto& d=*independentNative;
        d.Metrics=Metrics;d.Class=Class;d.Actor=Actor;d.Scene=Scene;d.StaticComponent=StaticComponent;
        d.StaticMesh=StaticMesh;d.Material=Material;d.FullTraversal=true;d.AuditActive=true;
        MiniMapStaticAssembly::Recipe nativeRecipe;const char* failure=nullptr;
        const bool nativeVerified=d.AuditIndependentNativeMain(AuditClass,Record.Generation,recipe.HighDetail,nativeRecipe,failure);
        const bool visualTraversalIncomplete=out.Truncated;
        nativeSubsetSelected=nativeVerified&&V::ValidateIndependentNativeSubset(out,nativeRecipe,true);
        if(nativeSubsetSelected)
        {
            recipe=std::move(nativeRecipe);
            out.Route="independent_native_main_static_subset";
            out.Reason="verified_native_main_only; omitted_scs_and_other_contributions_visual_completeness_unverified";
        }
        out.Details=C::Object({
            {"bounded_visual_audit",out.Details},
            {"independent_native_main",C::Object({
                {"selected",C::Bool(nativeSubsetSelected)},
                {"included_members",nativeSubsetSelected?"1":"0"},
                {"omitted_visual_traversal_incomplete",C::Bool(visualTraversalIncomplete)},
                {"failure",failure?C::Quote(failure):"null"},
                {"visual_completeness_verified","false"}
            })}
        });
        out.DiagnosticTruncated|=d.DiagnosticTruncated;V::BoundDiagnosticDetails(out);
    }
    if(CaptureRequested&&!skeletalAdmission.Node.empty()&&!skeletalAdmissionReported)
    {
        skeletalAdmissionReported=true;const auto& d=skeletalAdmission;
        const char* reason=!d.Asset?"mesh_validation_or_retention_failed":!d.Materials?"material_inputs_invalid":
            !d.Capacity?"material_capacity_exceeded":!d.Exact?"exact_native_template_class_required":
            !d.Admitted?"record_not_selected":!d.SlotPointers?"material_pointer_mapping_invalid":
            d.TransformStatus!="verified"?"effective_transform_unverified":"record_admitted";
        LOG_INFO("MiniMap: skeletal-only recipe admission: generation=%llu building=%u discovered=1 admitted=%d admissionReason=%s isSkeletal=%d assetValidRetained=%d defaultsValid=%d overridesValid=%d inputValid=%d materialCount=%zu capacity=%zu capacityOK=%d materialPointersValid=%d exactNativeClass=%d derivesNativeClass=%d visible=%d hidden=%d transform=%s transformReason=%s recordVerified=%d highDetail=%d staticEligibility=%s reason=%s",
            (unsigned long long)Record.Generation,Record.BuildingId,int(d.Admitted),reason,int(d.Derives),int(d.Asset),int(d.Defaults),int(d.Overrides),int(d.Materials),
            d.MaterialCount,MiniMapStaticAssembly::Slots,int(d.Capacity),int(d.SlotPointers),int(d.Exact),int(d.Derives),int(d.Visible),int(d.Hidden),
            d.TransformStatus.c_str(),d.TransformReason.c_str(),int(recipe.SkeletalTest.Input.Verified),int(recipe.HighDetail),V::Label(out.Status),out.Reason.c_str());
    }
    if(CaptureRequested&&skeletalSummaryReports<8)
    {
        ++skeletalSummaryReports;
        LOG_INFO("MiniMap: skeletal-only audit summary %zu/8: generation=%llu building=%u discovered=%zu recordPresent=%d captureRequested=1 staticEligibility=%s reason=%s",
            skeletalSummaryReports,(unsigned long long)Record.Generation,Record.BuildingId,out.Skeletal,int(recipe.SkeletalTest.Present),V::Label(out.Status),out.Reason.c_str());
    }
#if defined(MINIMAP_BUILDING_SPRITES)
    // Offer the same normal candidate to the private owner before static-recipe
    // eligibility. Skeletal-only/inherited recipes may be unsupported by copying
    // while their complete Blueprint still has a verified construction contract.
    // No result, policy, lease, FIFO order or production disposition is changed.
    if(CaptureRequested&&!DeferCapture&&AuditClass&&cdo&&ProbeResult.DefinitionIdentity=="valid"&&
        !ProbeResult.Truncated&&!ProbeResult.Incomplete)
    {
        MiniMapSpriteCapture::Candidate parallel;parallel.General=true;
        parallel.Identity=MiniMapSpriteCandidates::Copy(Record).Identity;
        parallel.CanonicalBlueprint=BlueprintIdentity.Path;
        parallel.Classification=Classification.CachedPolicy;
        parallel.ClassificationObserved=Classification.Fixed&&Classification.Subclass&&!Classification.Truncated&&Classification.InteriorTag!="unknown";
        parallel.ActorObject=int32_t(reinterpret_cast<const SDK::UObject*>(AuditClass)->Index);const auto name=reinterpret_cast<const SDK::UObject*>(AuditClass)->Name;
        parallel.ActorName=name.ComparisonIndex;parallel.ActorNumber=name.Number;
        parallel.Tier=recipe.HighDetail?MiniMapSpriteDiscoveryCore::Tier::High:MiniMapSpriteDiscoveryCore::Tier::Low;
        MiniMapSmelterCapture::OfferParallelBlueprint(parallel,AuditClass,Record.BuildingId,MiniMapMap::GetWorld());
    }
    // Read-only audit fence, also for ValidateRepresentation (CaptureRequested=false).
    // The scheduler commits it only after completed-result and exact-source checks.
    if(out.Status==V::Outcome::Eligible&&out.Disposition==V::AuditDisposition::Completed)
    {
        const auto auditSource=MiniMapBuildingSprites::InspectDispatch(MiniMapSpriteCandidates::Copy(Record).Identity,{});
        if(auditSource.Current&&auditSource.KeyAdopted)out.AuditRendererRevision=auditSource.SourceRevision;
    }
    if(CaptureRequested&&out.Status==V::Outcome::Eligible)
    {
        recipe.Complete=!out.Truncated;
        MiniMapSpriteCapture::Candidate candidate;candidate.General=true;
        candidate.Identity=MiniMapSpriteCandidates::Copy(Record).Identity;
        candidate.CanonicalBlueprint=BlueprintIdentity.Path;
        candidate.Classification=Classification.CachedPolicy;
        candidate.ClassificationObserved=Classification.Fixed&&Classification.Subclass&&!Classification.Truncated&&Classification.InteriorTag!="unknown";
        candidate.RepresentationVerified=nativeSubsetSelected||subsetSelected||
            (out.Root&&out.Transforms&&out.Materials&&!out.Inherited&&!out.Child&&!out.DerivedStatic&&!out.OtherSkinned);
        candidate.Truncated=out.Truncated||Classification.Truncated;
        candidate.ActorObject=int32_t(reinterpret_cast<const SDK::UObject*>(AuditClass)->Index);const auto name=reinterpret_cast<const SDK::UObject*>(AuditClass)->Name;
        candidate.ActorName=name.ComparisonIndex;candidate.ActorNumber=name.Number;
        candidate.Tier=recipe.HighDetail?MiniMapSpriteDiscoveryCore::Tier::High:MiniMapSpriteDiscoveryCore::Tier::Low;
        // An unresolved identity preserves the existing session-only capture path.
        if(candidate.CanonicalBlueprint&&IsBlueprintBlacklisted(*candidate.CanonicalBlueprint))
        {out.Status=V::Outcome::Unsupported;out.Reason="blueprint_blacklisted";return out;}
        V::ValidateStaticRecipe(out,recipe);
        if(out.Status!=V::Outcome::Eligible)return out; // Same recipe authority, no offer/lease/GPU work.
        // A.4.9h TEMPORARY 60-SECOND CAPTURE DELAY: validation stays capture-grade.
        if(!DeferCapture)
        {
            const auto decision=MiniMapSmelterCapture::OfferStaticAssembly(candidate,Record,recipe,MiniMapMap::GetWorld());
            out.RecordHandoff(decision);out.Reason=decision.Reason;
            if(out.Disposition==V::AuditDisposition::Completed&&!decision.Allowed())out.Status=V::Outcome::Conditional;
        }
    }
#endif
    // Optional observations have independent budgets and roots. Run after all
    // admission/handoff decisions, never feeding counts into readiness or policy.
    try
    {
        Context diagnostic;diagnostic.Class=Class;diagnostic.StaticMesh=StaticMesh;
        diagnostic.SkeletalMesh=SkeletalMesh;diagnostic.Material=Material;diagnostic.Ids=Ids;
        diagnostic.SourceMetadata(recipe,skeletalSources,skeletalSourceCount,skeletalSourcesTruncated,out);
    }
    catch(...){/* Optional metadata cannot fail validation or capture handoff. */}
    return out;
}
