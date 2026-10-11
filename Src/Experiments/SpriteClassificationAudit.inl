// Included inside Context. All owning references remain in Context::Roots.
void ClassificationInputs(uintptr_t definition)
{
#if defined(MINIMAP_BUILDING_SPRITES)
    MiniMapBuildingSprites::TimingScope classificationTiming(MiniMapBuildingSprites::Timing::Classification);
#endif

    namespace K=MiniMapSpriteClassification;
    auto& o=Classification;
    const auto start=Work.SelectionVisits+Work.RepresentationVisits;
    struct Finish {Context& C;K::Observation& O;size_t Start;~Finish()
        {O.IdentityVisits=C.Work.SelectionVisits+C.Work.RepresentationVisits-Start-O.TagVisits;O.Truncated|=C.Work.Exhausted;}} finish{*this,o,start};
    if(!Is(definition,Placement,0x1f0)||!Hold(definition,0x1f0))
    {o.Status="invalid";o.Reason="placement_extent_or_budget_failed";return;}
    constexpr size_t offsets[]={0xc1,0xc2,0x1b0,0x1b1,0x1b2,0x1b3,0x1b4};
    for(size_t i=0;i<7;++i)
    {
        uint8_t b=0;
        if(!Read(definition,offsets[i],b)||b>1){o.Status="invalid";o.Reason="invalid_boolean_storage";return;}
        o.Flags[i]=b!=0;
    }
    o.Fixed=true;o.Status="verified_inputs";o.Reason="stored_inputs_do_not_establish_capability_or_origin";
    auto object=[&](size_t offset,bool klass,uintptr_t expected)
    {
        uintptr_t p=0;if(!Read(definition,offset,p))return std::string("{\"status\":\"invalid_read\"}");
        if(!p)return std::string("{\"status\":\"absent\"}");
        const bool valid=klass?(expected&&ClassObject(p)&&Derives(p,expected)):(expected&&Is(p,expected));
        if(!valid||!Hold(p,klass?0x200:0x28))return std::string("{\"status\":\"invalid_or_unverified\"}");
        return Ref(p,klass?Class:expected);
    };
    const auto helperType=NativeClass(L"/Script/AuActorPlacement.AuAPHelperActor");
    o.Helper=object(0x30,true,helperType);
    const auto customType=NativeClass(L"/Script/AuActorPlacement.AuCustomPlacementMethod");
    o.Custom=object(0xb0,false,customType);
    uintptr_t config=0;
    if(!Read(definition,0x150,config))o.CachedConfig="invalid_read";
    else if(!config)o.CachedConfig="null_not_authored_absence";
    else o.CachedConfig=Is(config,Config,0x60)&&Hold(config,0x60)?"resident_validated":"invalid_or_budget_exhausted";
    auto soft=[&](size_t offset,K::SoftState& s)
    {
        C::ArrayHeader subpath{};
        if(!Read(definition,offset,s.WeakIndex)||!Read(definition,offset+4,s.WeakSerial)||
            !Read(definition,offset+8,s.Package)||!Read(definition,offset+16,s.Asset)||!Read(definition,offset+24,subpath))return;
        s.Read=true;s.SubpathCount=subpath.Num;s.Weak=K::WeakShape(s.WeakIndex,s.WeakSerial);
        // Installed FSoftObjectPath stores a UTF-8 TString, not FString/UTF-16.
        const auto b=C::BoundArray(subpath,1,1,256);
        if(s.Package.Index<0||s.Asset.Index<0||!b.Valid||(b.Bytes&&!Readable(subpath.Data,b.Bytes,1)))
        {s.Presence="invalid";return;}
        const bool names=s.Package.Index||s.Package.Number||s.Asset.Index||s.Asset.Number;
        if(b.Truncated){s.Presence="nonempty_subpath_truncated";o.Truncated=true;return;}
        uint8_t last=0;
        if(subpath.Num&&(!Read(subpath.Data,size_t(subpath.Num)-1,last)||last))
        {s.Presence="invalid_unterminated_subpath";return;}
        s.Presence=names||subpath.Num>1?"nonempty_authored_reference_unresolved":"null_default_reference";
    };
    soft(0x120,o.ConfigReference);soft(0x38,o.ActorReference);
    const auto buildingType=NativeClass(L"/Script/Chimera.CrBuildingData");
    if(!buildingType){o.BuildingData="unavailable_native_class_anchor";return;}
    if(!Is(definition,buildingType))
    {o.BuildingData=Work.Exhausted?"unavailable_budget_exhausted":"not_validated_as_building_subclass";return;}
    if(!Is(definition,buildingType,0x460)){o.BuildingData="invalid_extent";return;}
    uint8_t dynamic=0;
    if(!Read(definition,0x2c1,o.UI)||!Read(definition,0x2c2,o.Subtype)||!Read(definition,0x458,dynamic)||dynamic>1)
    {o.BuildingData="invalid_subclass_fields";return;}
    o.Subclass=true;o.Dynamic=dynamic!=0;o.BuildingData="resident_validated";
    C::ArrayHeader tags{};if(!Read(definition,0x3b0,tags)){o.TagsStatus="invalid_read";return;}
    const auto bounds=C::BoundArray(tags,8,4,K::TagLimit);
    if(!bounds.Valid||(bounds.Bytes&&!Readable(tags.Data,bounds.Bytes,4))){o.TagsStatus="invalid";return;}
    o.TagsStatus=bounds.Status;o.Truncated|=bounds.Truncated;
    o.InteriorComparison=nameEqualsWide?"exact_FName_Equals_Interior":"unavailable_binding";
    for(size_t i=0;i<bounds.Count;++i)
    {
        if(!Work.Spend(false)){o.TagsStatus="truncated_budget";o.Truncated=true;break;}
        K::Name name{};++o.TagVisits;
        if(!Read(tags.Data,i*8,name)||name.Index<0){o.TagsStatus="invalid_element";break;}
        o.Tags[o.TagCount++]=name;
        // Read only names from the validated, retained definition's bounded array.
        // Same comparison used by installed FCrBuildingBaseView::IsInterior.
        // No pool enumeration, string allocation, name insertion or package load.
        static_assert(sizeof(K::Name)==8&&alignof(K::Name)==4);
        using Equals=bool(*)(K::Name,const wchar_t*);
        if(nameEqualsWide&&name.Number==0&&reinterpret_cast<Equals>(nameEqualsWide)(name,L"Interior"))
            o.InteriorTag="present";
    }
    if(o.InteriorTag=="present")
    {K::Evidence evidence;evidence.InteriorTag=true;o.CachedPolicy=K::Decide(evidence);}
    else if(nameEqualsWide&&!bounds.Truncated&&o.TagCount==bounds.Count&&o.TagsStatus!="invalid_element"&&!Work.Exhausted)
        o.InteriorTag="absent";
}
