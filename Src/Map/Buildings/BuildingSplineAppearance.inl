// Included inside BuildingCollector's acquisition namespace, after
// ProfileRetention. Reuses its reader, type checks and reference contracts.
namespace Appearance=MiniMapBuildingSplineAppearance;
struct AppearanceOps
{
    struct Held {uintptr_t Object;int32_t Index,Serial;};
    std::vector<Held> HeldObjects;
    const SDK::UWorld* World=MiniMapMap::GetWorld();
    uint64_t Generation=g_generation;
    bool Released=false,Valid=true;
    bool Current() const
    {return World && World==g_world && World==MiniMapMap::GetWorld() && Generation==g_generation && MiniMapResidentMetadata::Available();}
    ~AppearanceOps(){if(!Released)Finish();}
    bool Hold(uintptr_t object,size_t extent=sizeof(SDK::UObject))
    {
        if(!Current())return false;
        MiniMapDefinitionAcquisition::IdentityObservation observed;
        if(!ProfileTransient(object,extent,&observed,MiniMapDefinitionAcquisition::IdentityRole::LookupResult,0,object))return false;
        auto existing=std::find_if(HeldObjects.begin(),HeldObjects.end(),[&](const Held& held){return held.Object==object;});
        if(existing!=HeldObjects.end())return existing->Index==observed.Index && existing->Serial==observed.Serial;
        // Reserve BEFORE acquiring a root, so allocation failure cannot lose it.
        if(HeldObjects.size()==HeldObjects.capacity())
        {
            if(HeldObjects.size()==HeldObjects.max_size())throw std::length_error("appearance retention storage");
            HeldObjects.reserve(HeldObjects.empty()?1:HeldObjects.size()>HeldObjects.max_size()/2?
                HeldObjects.max_size():HeldObjects.size()*2);
        }
        if(!MiniMapResidentMetadata::Retain(object,extent))return false;
        HeldObjects.push_back({object,observed.Index,observed.Serial});return true;
    }
    bool Finish()
    {
        if(Released)return Valid;
        Valid=Current();
        for(const auto& held:HeldObjects)
        {
            MiniMapDefinitionAcquisition::IdentityObservation observed;
            if(!ProfileTransient(held.Object,sizeof(SDK::UObject),&observed,
                MiniMapDefinitionAcquisition::IdentityRole::LookupResult,0,held.Object) ||
                held.Index!=observed.Index || held.Serial!=observed.Serial)Valid=false;
        }
        Released=true;
        for(auto it=HeldObjects.rbegin();it!=HeldObjects.rend();++it)
            if(!MiniMapResidentMetadata::Release(it->Object))Valid=false;
        return Valid=Valid && Current();
    }
    static std::string Name(SDK::FName name)
    {return std::to_string(name.ComparisonIndex)+":"+std::to_string(name.Number);}
    static std::string Number(double value)
    {std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<value;return out.str();}
    std::string Ref(uintptr_t object)
    {
        if(!object)return "null_reference";
        if(!Hold(object))return "unavailable_reference";
        ProfileReader read;SDK::FName name{};int32_t index=0;
        if(!read.Read(object+offsetof(SDK::UObject,Name),name) || !read.Read(object+offsetof(SDK::UObject,Index),index))return "unreadable_reference";
        const auto held=std::find_if(HeldObjects.begin(),HeldObjects.end(),[&](const Held& h){return h.Object==object;});
        std::string value="index="+std::to_string(index)+" serial="+std::to_string(held->Serial)+
            (held->Serial?" stable_identity":" transient_copy_not_cache_identity")+
            " path=undecoded name_token="+Name(name)+" outer_name_tokens=";
        std::unordered_set<uintptr_t> seen;uintptr_t outer=object;
        while(outer)
        {
            if(!seen.insert(outer).second){value+="[cycle]";break;}
            if(!Hold(outer) || !read.Read(outer+offsetof(SDK::UObject,Name),name)){value+="[unavailable]";break;}
            value+=Name(name)+"/";
            if(!read.Read(outer+offsetof(SDK::UObject,Outer),outer)){value+="[unavailable]";break;}
        }
        return value;
    }
    template<class T> bool Read(uintptr_t object,T& out)
    {return Hold(object,sizeof(T)) && ProfileReadBytes(object,&out,sizeof(T));}
    template<class T> bool Array(const SDK::TArray<T>& array,std::vector<T>& out)
    {
        struct Layout {uintptr_t Data;int32_t Num,Max;};Layout a{};
        static_assert(sizeof(a)==sizeof(array));std::memcpy(&a,&array,sizeof(a));
        if(a.Num<0 || a.Max<a.Num || (a.Num && !a.Data) || size_t(a.Num)>SIZE_MAX/sizeof(T) ||
            a.Data>UINTPTR_MAX-size_t(a.Num)*sizeof(T))return false;
        out.resize(size_t(a.Num));
        return out.empty() || ProfileReadBytes(a.Data,out.data(),out.size()*sizeof(T));
    }
    bool Is(uintptr_t object,const wchar_t* type,const wchar_t* package=L"/Script/Engine")
    {return ProfileDerives(object,nullptr,type,package)==ProfileCheck::Matches;}
    static void Row(Appearance::Report& report,const std::string& source,const std::string& object,const std::string& detail)
    {report.Rows.push_back({source,object,detail});}
    void Link(Appearance::Report& report,std::vector<Appearance::Work>& children,Appearance::Kind kind,
        uintptr_t object,uintptr_t owner,const std::string& source)
    {
        Row(report,source,Ref(owner),"reference="+Ref(object));
        if(object)children.emplace_back(kind,object,owner,source);
    }
    template<class T> bool FieldArray(uintptr_t object,size_t offset,std::vector<T>& out)
    {SDK::TArray<T> a{};ProfileReader read;return read.Read(object+offset,a) && Array(a,out);}
    bool PointerArray(uintptr_t object,size_t offset,std::vector<uintptr_t>& out)
    {return FieldArray(object,offset,out);}
    std::vector<uintptr_t> MeshDefaults(uintptr_t mesh,bool& valid)
    {
        std::vector<SDK::FStaticMaterial> slots;std::vector<uintptr_t> result;
        valid=mesh && Hold(mesh,sizeof(SDK::UStaticMesh)) && Is(mesh,L"StaticMesh") &&
            FieldArray(mesh,offsetof(SDK::UStaticMesh,StaticMaterials),slots);
        if(valid)for(const auto& slot:slots)result.push_back(reinterpret_cast<uintptr_t>(slot.MaterialInterface));
        return result;
    }
    void Components(Appearance::Report& report,std::vector<Appearance::Work>& children,uintptr_t object,
        size_t offset,uintptr_t owner,const std::string& source)
    {
        std::vector<uintptr_t> values;
        if(!PointerArray(object,offset,values)){Row(report,source,Ref(object),"unavailable_or_invalid_array; not confirmed absence");return;}
        Row(report,source,Ref(object),"count="+std::to_string(values.size()));
        for(auto component:values)Link(report,children,Appearance::Kind::Component,component,owner,source);
    }
    // Only SDK-declared bit fields; no guessed byte offsets or source buffers.
    static bool CPUAccess(uintptr_t mesh,bool& value) noexcept
    {
        __try {value=reinterpret_cast<const SDK::UStaticMesh*>(mesh)->bAllowCPUAccess!=0;return true;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
    }
    struct TextureNumbers
    {
        MiniMapNative::TextureAccess::PlatformData Platform;
        int32_t Resident=-1,Total=-1;
        bool PlatformAvailable=false;
    };
    static bool TextureData(uintptr_t object,TextureNumbers& result) noexcept
    {
        __try
        {
            const auto* api=MiniMapNative::Get();if(!api)return false;
            auto* texture=reinterpret_cast<SDK::UTexture2D*>(object);
            if(api->texture.getNumResidentMips)result.Resident=api->texture.getNumResidentMips(texture);
            if(api->texture.getNumMips)result.Total=api->texture.getNumMips(texture);
            result.PlatformAvailable=MiniMapNative::TextureAccess::QueryPlatformData(texture,result.Platform);return true;
        }
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
    }
    static bool MipData(const MiniMapNative::TextureAccess::PlatformData& platform,int32_t index,
        MiniMapNative::TextureAccess::MipData& mip) noexcept
    {
        __try {return MiniMapNative::TextureAccess::QueryMipData(platform,index,mip);}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
    }
    static std::string Parameter(const SDK::FMaterialParameterInfo& info)
    {return "name=undecoded name_token="+Name(info.Name)+" association="+std::to_string(int(info.Association))+" index="+std::to_string(info.Index);}
    // Prove the expected anchor is a class or its actual existing CDO. Never
    // infer authored ownership merely from an Actor flag or a matching name.
    bool OwnerClass(uintptr_t owner,uintptr_t& type)
    {
        if(!Hold(owner))return false;
        const auto isClass=ProfileDerives(owner,nullptr,L"Class",L"/Script/CoreUObject");
        if(isClass==ProfileCheck::Matches)
        {type=owner;return Hold(type,sizeof(SDK::UClass));}
        if(isClass!=ProfileCheck::NoMatch || ProfileDerives(owner,nullptr,L"Actor")!=ProfileCheck::Matches)return false;
        ProfileReader read;int32_t index=0;
        return read.Read(owner+offsetof(SDK::UObject,Class),type) && Hold(type,sizeof(SDK::UClass)) &&
            Is(type,L"Class",L"/Script/CoreUObject") && read.Read(owner+offsetof(SDK::UObject,Index),index) &&
            ProfileCDORelationship(type,owner,index);
    }
    bool AuthoredComponentOwner(uintptr_t component,uintptr_t owner)
    {
        uintptr_t type=0;if(!OwnerClass(owner,type))return false;
        ProfileReader read;std::unordered_set<uintptr_t> classes;
        while(type)
        {
            if(!classes.insert(type).second || !Hold(type,sizeof(SDK::UClass)) ||
                !Is(type,L"Class",L"/Script/CoreUObject"))return false;
            if(!read.Read(type+offsetof(SDK::UStruct,SuperStruct),type))return false;
        }
        uintptr_t outer=0;std::unordered_set<uintptr_t> seen;
        if(!read.Read(component+offsetof(SDK::UObject,Outer),outer))return false;
        while(outer && seen.insert(outer).second)
        {
            if(!Hold(outer))return false;
            if(outer==owner || classes.contains(outer))return true;
            const auto actor=ProfileDerives(outer,nullptr,L"Actor");
            if(actor==ProfileCheck::Matches)
            {
                uintptr_t cdoType=0;
                return OwnerClass(outer,cdoType) && classes.contains(cdoType);
            }
            if(actor!=ProfileCheck::NoMatch || !read.Read(outer+offsetof(SDK::UObject,Outer),outer))return false;
        }
        return false;
    }
    template<class T> void Parameters(uintptr_t object,size_t offset,const char* kind,Appearance::Report& report,
        std::vector<Appearance::Work>& children)
    {
        std::vector<T> values;
        if(!FieldArray(object,offset,values)){Row(report,kind,Ref(object),"unavailable_or_invalid_array; not confirmed absence");return;}
        Row(report,kind,Ref(object),"authored_override_count="+std::to_string(values.size()));
        for(const auto& v:values)
        {
            std::string detail=Parameter(v.ParameterInfo);
            detail+=" expression_guid="+std::to_string(v.ExpressionGUID.A)+":"+std::to_string(v.ExpressionGUID.B)+":"+
                std::to_string(v.ExpressionGUID.C)+":"+std::to_string(v.ExpressionGUID.D);
            if constexpr(std::is_same_v<T,SDK::FTextureParameterValue>)
            {
                detail+=" texture="+Ref(reinterpret_cast<uintptr_t>(v.ParameterValue));
                Link(report,children,Appearance::Kind::Texture,reinterpret_cast<uintptr_t>(v.ParameterValue),object,"authored_texture_parameter "+Parameter(v.ParameterInfo));
            }
            else if constexpr(std::is_same_v<T,SDK::FScalarParameterValue>)detail+=" value="+Number(v.ParameterValue);
            else detail+=" rgba="+Number(v.ParameterValue.R)+","+Number(v.ParameterValue.G)+","+Number(v.ParameterValue.B)+","+Number(v.ParameterValue.A);
            Row(report,kind,Ref(object),detail);
        }
    }
    void Inspect(const Appearance::Work& item,Appearance::Report& report,std::vector<Appearance::Work>& children)
    {
        using K=Appearance::Kind;
        const auto object=item.Object;const auto reference=Ref(object);ProfileReader read;
        auto missing=[&](const char* reason){Row(report,item.Source,reference,std::string("unavailable: ")+reason+"; not confirmed absence");};
        if(!Hold(object)){missing("object validation/retention");return;}
        switch(item.Type)
        {
        case K::Class:
        {
            if(!Hold(object,sizeof(SDK::UClass)) || !Is(object,L"Class",L"/Script/CoreUObject")){missing("class type");return;}
            uintptr_t parent=0,cdo=0;
            if(!read.Read(object+offsetof(SDK::UStruct,SuperStruct),parent) ||
                !read.Read(object+offsetof(SDK::UClass,ClassDefaultObject),cdo)){missing("class fields");return;}
            Row(report,item.Source,reference,"authored hierarchy; construction_script=unverified_not_executed; runtime_modifications=unverified_not_observed");
            Link(report,children,K::Defaults,cdo,object,"existing_cdo");
            const auto blueprint=ProfileDerives(object,nullptr,L"BlueprintGeneratedClass");
            if(blueprint==ProfileCheck::Matches)
            {
                if(!Hold(object,sizeof(SDK::UBlueprintGeneratedClass))){missing("Blueprint class extent");return;}
                Components(report,children,object,offsetof(SDK::UBlueprintGeneratedClass,ComponentTemplates),object,"class_component_templates");
                uintptr_t scs=0,handler=0;
                if(read.Read(object+offsetof(SDK::UBlueprintGeneratedClass,SimpleConstructionScript),scs))Link(report,children,K::Script,scs,object,"authored_scs");
                else missing("SCS reference");
                if(read.Read(object+offsetof(SDK::UBlueprintGeneratedClass,InheritableComponentHandler),handler))Link(report,children,K::Handler,handler,object,"inheritable_component_handler");
                else missing("inheritable handler reference");
            }
            else if(blueprint!=ProfileCheck::NoMatch)missing("Blueprint class classification");
            if(parent)Link(report,children,K::Class,parent,object,"superclass");
            break;
        }
        case K::Defaults:
        {
            MiniMapDefinitionAcquisition::IdentityObservation observed;
            const auto actor=ProfileDerives(object,nullptr,L"Actor");
            if(actor==ProfileCheck::NoMatch){Row(report,item.Source,reference,"non_actor_cdo; no component inventory");return;}
            if(actor!=ProfileCheck::Matches){missing("CDO Actor classification");return;}
            if(!ProfileTransient(object,sizeof(SDK::AActor),&observed,MiniMapDefinitionAcquisition::IdentityRole::LookupResult,0,object) ||
                !Hold(object,sizeof(SDK::AActor)) || !ProfileCDORelationship(item.Owner,object,observed.Index)){missing("existing actor CDO relationship");return;}
            Components(report,children,object,offsetof(SDK::AActor,InstanceComponents),object,"cdo_instance_components");
            Components(report,children,object,offsetof(SDK::AActor,BlueprintCreatedComponents),object,"cdo_blueprint_created_components");
            uintptr_t root=0;
            if(read.Read(object+offsetof(SDK::AActor,RootComponent),root))Link(report,children,K::Component,root,object,"cdo_root_component");
            else missing("CDO root reference");
            Row(report,"native_component_inventory",reference,"partial: attached CDO graph and SDK arrays; complete native OwnedComponents set has no verified field/access contract here");
            if(Is(object,L"CrModularRampWalkway",L"/Script/Chimera"))
            {
                uintptr_t mesh=0;
                if(read.Read(object+offsetof(SDK::ACrModularRampWalkway,SplineMesh),mesh))Link(report,children,K::Mesh,mesh,object,"native_builder_cdo_mesh X_forward");
                else missing("native builder authored SplineMesh");
            }
            break;
        }
        case K::Script:
        {
            if(!Hold(object,sizeof(SDK::USimpleConstructionScript)) || !Is(object,L"SimpleConstructionScript")){missing("SCS type/extent");return;}
            for(auto offset:{offsetof(SDK::USimpleConstructionScript,RootNodes),offsetof(SDK::USimpleConstructionScript,AllNodes)})
            {
                std::vector<uintptr_t> nodes;
                if(!PointerArray(object,offset,nodes)){missing("SCS nodes array");continue;}
                Row(report,"scs_nodes",reference,"field_offset="+std::to_string(offset)+" count="+std::to_string(nodes.size()));
                for(auto node:nodes)Link(report,children,K::Node,node,item.Owner,"scs_node");
            }
            break;
        }
        case K::Node:
        {
            SDK::USCS_Node node{};
            if(!Is(object,L"SCS_Node") || !Read(object,node)){missing("SCS node type/extent");return;}
            Row(report,"scs_key",reference,"owner="+Ref(item.Owner)+" variable="+Name(node.InternalVariableName)+" guid="+
                std::to_string(node.VariableGUID.A)+":"+std::to_string(node.VariableGUID.B)+":"+
                std::to_string(node.VariableGUID.C)+":"+std::to_string(node.VariableGUID.D));
            const auto declared=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(node.ComponentClass));
            const auto component=reinterpret_cast<uintptr_t>(node.ComponentTemplate);
            if(component && (!Hold(declared,sizeof(SDK::UClass)) || !Is(declared,L"Class",L"/Script/CoreUObject") ||
                ProfileDerives(component,reinterpret_cast<const SDK::UClass*>(declared))!=ProfileCheck::Matches))missing("SCS declared component class relationship");
            else Link(report,children,K::Component,component,item.Owner,"scs_component_template");
            std::vector<SDK::USCS_Node*> nodes;
            if(!Array(node.ChildNodes,nodes))missing("SCS child array");
            else for(auto child:nodes)Link(report,children,K::Node,reinterpret_cast<uintptr_t>(child),item.Owner,"scs_child_node");
            break;
        }
        case K::Handler:
        {
            if(!Hold(object,sizeof(SDK::UInheritableComponentHandler)) || !Is(object,L"InheritableComponentHandler")){missing("inheritable handler type/extent");return;}
            std::vector<SDK::FComponentOverrideRecord> records;
            if(!FieldArray(object,offsetof(SDK::UInheritableComponentHandler,Records),records)){missing("inheritable override records");return;}
            Row(report,"inheritable_records",reference,"count="+std::to_string(records.size())+" cross_template_selection=unverified; raw keys kept distinct");
            for(const auto& record:records)
            {
                const auto keyOwner=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(record.ComponentKey.OwnerClass));
                const auto component=reinterpret_cast<uintptr_t>(record.ComponentTemplate);
                const auto declared=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(record.ComponentClass));
                Row(report,"inheritable_override_key",Ref(component),"owner="+Ref(keyOwner)+" variable="+Name(record.ComponentKey.SCSVariableName)+" guid="+
                    std::to_string(record.ComponentKey.AssociatedGuid.A)+":"+std::to_string(record.ComponentKey.AssociatedGuid.B)+":"+
                    std::to_string(record.ComponentKey.AssociatedGuid.C)+":"+std::to_string(record.ComponentKey.AssociatedGuid.D));
                if(component && (!Hold(declared,sizeof(SDK::UClass)) || !Is(declared,L"Class",L"/Script/CoreUObject") ||
                    ProfileDerives(component,reinterpret_cast<const SDK::UClass*>(declared))!=ProfileCheck::Matches))missing("override declared component class relationship");
                else Link(report,children,K::Component,component,item.Owner,"inherited_component_template_override");
            }
            break;
        }
        case K::Component:
        {
            if(!Hold(object,sizeof(SDK::UActorComponent)) || !Is(object,L"ActorComponent")){missing("component type/extent");return;}
            // Prove this is owned by the authored class/CDO graph, never a live actor.
            if(!AuthoredComponentOwner(object,item.Owner)){missing("component outer does not establish authored owner");return;}
            const auto scene=ProfileDerives(object,nullptr,L"SceneComponent");
            if(scene==ProfileCheck::Matches)
                Components(report,children,object,offsetof(SDK::USceneComponent,AttachChildren),item.Owner,"authored_attached_components");
            else if(scene!=ProfileCheck::NoMatch)missing("SceneComponent classification");
            const auto staticMesh=ProfileDerives(object,nullptr,L"StaticMeshComponent");
            if(staticMesh==ProfileCheck::NoMatch){Row(report,item.Source,reference,"not a static mesh component");return;}
            if(staticMesh!=ProfileCheck::Matches){missing("StaticMeshComponent classification");return;}
            if(!Hold(object,sizeof(SDK::UStaticMeshComponent))){missing("static mesh component extent");return;}
            uintptr_t mesh=0;
            if(!read.Read(object+offsetof(SDK::UStaticMeshComponent,StaticMesh),mesh)){missing("template mesh reference");return;}
            Link(report,children,K::Mesh,mesh,object,"component_template_mesh");
            std::vector<uintptr_t> overrides;bool defaultsValid=!mesh;
            auto defaults=mesh?MeshDefaults(mesh,defaultsValid):std::vector<uintptr_t>{};
            if(!defaultsValid || !PointerArray(object,offsetof(SDK::UMeshComponent,OverrideMaterials),overrides)){missing("material slot arrays");return;}
            const auto slots=Appearance::SelectSlots(defaults,overrides);
            for(size_t i=0;i<slots.size();++i)
            {
                Row(report,"template_local_material_slot",reference,"slot="+std::to_string(i)+" mesh_default="+Ref(slots[i].Default)+
                    " component_override="+Ref(slots[i].Override)+" selected="+Ref(slots[i].Selected)+" gameplay_effective=unverified");
                Link(report,children,K::Material,slots[i].Override,object,"authored_component_material_override slot="+std::to_string(i));
            }
            if(Is(object,L"SplineMeshComponent"))
            {
                uint8_t axis=0;
                if(read.Read(object+offsetof(SDK::USplineMeshComponent,ForwardAxis),axis))Row(report,"template_forward_axis",reference,"axis="+std::to_string(axis)+" source_UV_orientation=unverified; no material tiling inferred from spline scale/offset");
                else missing("spline template forward axis");
                SDK::FSplineMeshParams params{};SDK::FVector up{};float minimum=0,maximum=0;
                if(read.Read(object+offsetof(SDK::USplineMeshComponent,SplineParams),params) &&
                    read.Read(object+offsetof(SDK::USplineMeshComponent,SplineUpDir),up) &&
                    read.Read(object+offsetof(SDK::USplineMeshComponent,SplineBoundaryMin),minimum) &&
                    read.Read(object+offsetof(SDK::USplineMeshComponent,SplineBoundaryMax),maximum))
                    Row(report,"template_spline_deformation",reference,"start_scale="+Number(params.StartScale.X)+","+Number(params.StartScale.Y)+
                        " end_scale="+Number(params.EndScale.X)+","+Number(params.EndScale.Y)+" start_offset="+Number(params.StartOffset.X)+","+Number(params.StartOffset.Y)+
                        " end_offset="+Number(params.EndOffset.X)+","+Number(params.EndOffset.Y)+" rolls="+Number(params.StartRoll)+","+Number(params.EndRoll)+
                        " up="+Number(up.X)+","+Number(up.Y)+","+Number(up.Z)+" boundaries="+Number(minimum)+","+Number(maximum)+"; deformation values are not material UV parameters");
                else missing("spline template deformation fields");
            }
            break;
        }
        case K::Mesh:
        {
            bool valid=false;const auto materials=MeshDefaults(object,valid);
            if(!valid){missing("static mesh type/extent/material array");return;}
            bool cpu=false;
            Row(report,"source_mesh_uv",reference,std::string("allow_cpu_access=")+(CPUAccess(object,cpu)?(cpu?"true":"false"):"unavailable")+
                " LOD_sections=unavailable vertex_index_UV_buffers=unavailable; missing verified StaticMesh render-data/LOD/section/vertex-buffer accessor; UV orientation/repetition unverified");
            std::vector<SDK::FStaticMaterial> slots;
            if(FieldArray(object,offsetof(SDK::UStaticMesh,StaticMaterials),slots))for(size_t i=0;i<slots.size();++i)
            {
                Row(report,"mesh_material_slot",reference,"slot="+std::to_string(i)+" slot_name="+Name(slots[i].MaterialSlotName)+
                    " UV_density_initialized="+std::to_string(slots[i].UVChannelData.bInitialized)+" densities="+
                    Number(slots[i].UVChannelData.LocalUVDensities[0])+","+Number(slots[i].UVChannelData.LocalUVDensities[1])+","+
                    Number(slots[i].UVChannelData.LocalUVDensities[2])+","+Number(slots[i].UVChannelData.LocalUVDensities[3])+"; densities are not vertex UVs");
                Link(report,children,K::Material,materials[i],object,"mesh_default_material slot="+std::to_string(i));
            }
            break;
        }
        case K::Material:
        {
            if(!Hold(object,sizeof(SDK::UMaterialInterface)) || !Is(object,L"MaterialInterface")){missing("material interface type/extent");return;}
            const auto instance=ProfileDerives(object,nullptr,L"MaterialInstance");
            if(instance==ProfileCheck::NoMatch)
            {Row(report,"base_material",reference,"shader graph/default texture bindings unavailable through existing NativeApi; base color/masks/normals/procedural use unverified");return;}
            if(instance!=ProfileCheck::Matches){missing("MaterialInstance classification");return;}
            if(!Hold(object,sizeof(SDK::UMaterialInstance))){missing("material instance extent");return;}
            uintptr_t parent=0;
            if(read.Read(object+offsetof(SDK::UMaterialInstance,Parent),parent))Link(report,children,K::Material,parent,object,"authored_material_parent");
            else missing("material parent reference");
            Parameters<SDK::FTextureParameterValue>(object,offsetof(SDK::UMaterialInstance,TextureParameterValues),"texture_parameter",report,children);
            Parameters<SDK::FScalarParameterValue>(object,offsetof(SDK::UMaterialInstance,ScalarParameterValues),"scalar_parameter",report,children);
            Parameters<SDK::FVectorParameterValue>(object,offsetof(SDK::UMaterialInstance,VectorParameterValues),"vector_parameter",report,children);
            Row(report,"material_resolution",reference,"raw authored overrides and parent chain; inherited/default/layered evaluation not certified; double-vector/static-switch/collection/overlay inputs not evaluated");
            break;
        }
        case K::Texture:
        {
            if(!Is(object,L"Texture")){missing("texture type");return;}
            const auto texture2D=ProfileDerives(object,nullptr,L"Texture2D");
            if(texture2D==ProfileCheck::NoMatch){Row(report,"texture_type",reference,"referenced resident non-Texture2D; platform/mips unsupported by existing TextureAccess");return;}
            if(texture2D!=ProfileCheck::Matches){missing("Texture2D classification");return;}
            if(!Hold(object,sizeof(SDK::UTexture2D))){missing("Texture2D extent");return;}
            SDK::ETextureAddress x{},y{};
            if(read.Read(object+offsetof(SDK::UTexture2D,AddressX),x) && read.Read(object+offsetof(SDK::UTexture2D,AddressY),y))
                Row(report,"texture_address",reference,"authored_x="+std::to_string(int(x))+" authored_y="+std::to_string(int(y))+"; effective material sampler override unverified");
            else missing("authored texture address fields");
            TextureNumbers numbers{};
            if(!TextureData(object,numbers))
            {report.Residency.push_back({"texture_residency",reference,"unavailable: texture metadata native query; no current observation"});return;}
            report.Residency.push_back({"texture_residency",reference,"resident_mips="+std::to_string(numbers.Resident)+" total_mips="+std::to_string(numbers.Total)+"; sampled_now; -1=unavailable; object residency does not certify CPU bulk residency"});
            if(!numbers.PlatformAvailable)
            {report.Residency.push_back({"texture_platform",reference,"unavailable: texture platform data not currently available; no load/stream/wait attempted"});return;}
            const auto& p=numbers.Platform;
            report.Residency.push_back({"texture_platform",reference,"format="+std::to_string(p.PixelFormat)+" size="+std::to_string(p.SizeX)+"x"+std::to_string(p.SizeY)+
                " mips="+std::to_string(p.MipCount)+" srgb="+std::to_string(p.SRGB)+" VT="+std::to_string(p.VTData!=nullptr)+" CPUCopy="+std::to_string(p.CPUCopy!=nullptr)});
            if(p.MipCount<0 || p.MipCapacity<p.MipCount || (p.MipCount && !p.HasMipPointers))
            {report.Residency.push_back({"texture_mip",reference,"unavailable: platform mip array"});return;}
            for(int32_t i=0;i<p.MipCount;++i)
            {
                MiniMapNative::TextureAccess::MipData mip{};
                if(!MipData(p,i,mip)){report.Residency.push_back({"texture_mip",reference,"index="+std::to_string(i)+" unavailable; not confirmed absent"});continue;}
                report.Residency.push_back({"texture_mip",reference,"index="+std::to_string(i)+" size="+std::to_string(mip.SizeX)+"x"+std::to_string(mip.SizeY)+"x"+std::to_string(mip.SizeZ)+
                    " bulk_bytes="+std::to_string(mip.BulkSize)+" can_load_from_disk="+std::to_string(mip.CanLoadFromDisk)+" CPU_bulk_residency=unverified; no pixel copy attempted"});
            }
            break;
        }
        }
    }
};
