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
    if(!Is(definition,Placement)||!Hold(definition))
    {o.Status="invalid";o.Reason="placement_extent_or_budget_failed";return;}
    const auto* source=reinterpret_cast<const SDK::UAuActorPlacementData*>(definition);
    const bool* flags[]={&source->bCanBePlacedOnTerrain,&source->bHasToBePlacedOnTerrain,&source->RequireHabitat,
        &source->RequiresBeingInsideHabitat,&source->SplineBased,&source->CheckBaseCoreArea,&source->CheckOutsideBaseCoreArea};
    for(size_t i=0;i<7;++i)
    {
        const auto b=*reinterpret_cast<const uint8_t*>(flags[i]);
        if(b>1){o.Status="invalid";o.Reason="invalid_boolean_storage";return;}
        o.Flags[i]=b!=0;
    }
    o.Fixed=true;o.Status="verified_inputs";o.Reason="stored_inputs_do_not_establish_capability_or_origin";
    auto object=[&](uintptr_t p,bool klass,uintptr_t expected)
    {
        if(!p)return std::string("{\"status\":\"absent\"}");
        const bool valid=klass?(expected&&ClassObject(p)&&Derives(p,expected)):(expected&&Is(p,expected));
        if(!valid||!Hold(p))return std::string("{\"status\":\"invalid_or_unverified\"}");
        return Ref(p,klass?Class:expected);
    };
    const auto helperType=NativeClass(L"/Script/AuActorPlacement.AuAPHelperActor");
    o.Helper=object(reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(source->HelperActorClass)),true,helperType);
    const auto customType=NativeClass(L"/Script/AuActorPlacement.AuCustomPlacementMethod");
    o.Custom=object(reinterpret_cast<uintptr_t>(source->CustomPlacementMethod),false,customType);
    const auto config=reinterpret_cast<uintptr_t>(source->EntityType.EntityConfigPtr);
    o.CachedConfig=!config?"null_not_authored_absence":Is(config,Config)&&Hold(config)?"resident_validated":"invalid_or_budget_exhausted";
    auto soft=[&](const SDK::FSoftObjectPtr& reference,K::SoftState& s)
    {
        const auto& path=reference.ObjectID;const auto& package=path.AssetPath.PackageName;const auto& asset=path.AssetPath.AssetName;
        s.WeakIndex=reference.WeakPtr.ObjectIndex;s.WeakSerial=reference.WeakPtr.ObjectSerialNumber;
        s.Package={package.ComparisonIndex,package.Number};s.Asset={asset.ComparisonIndex,asset.Number};
        const C::ArrayHeader subpath{reinterpret_cast<uintptr_t>(path.SubPathString.CStr()),path.SubPathString.Num(),path.SubPathString.Max()};
        s.Read=true;s.SubpathCount=subpath.Num;s.Weak=K::WeakShape(s.WeakIndex,s.WeakSerial);
        // Installed FSoftObjectPath stores a UTF-8 TString, not FString/UTF-16.
        const auto b=C::BoundArray(subpath,1,1,256);
        if(s.Package.Index<0||s.Asset.Index<0||!b.Valid||(b.Bytes&&!Readable(subpath.Data,b.Bytes,1)))
        {s.Presence="invalid";return;}
        const bool names=s.Package.Index||s.Package.Number||s.Asset.Index||s.Asset.Number;
        if(b.Truncated){s.Presence="nonempty_subpath_truncated";o.Truncated=true;return;}
        if(subpath.Num&&path.SubPathString.CStr()[subpath.Num-1]!=0)
        {s.Presence="invalid_unterminated_subpath";return;}
        s.Presence=names||subpath.Num>1?"nonempty_authored_reference_unresolved":"null_default_reference";
    };
    soft(source->EntityType.EntityConfig,o.ConfigReference);soft(source->ActorClass,o.ActorReference);
    const auto buildingType=NativeClass(L"/Script/Chimera.CrBuildingData");
    if(!buildingType){o.BuildingData="unavailable_native_class_anchor";return;}
    if(!Is(definition,buildingType))
    {o.BuildingData=Work.Exhausted?"unavailable_budget_exhausted":"not_validated_as_building_subclass";return;}
    const auto* building=reinterpret_cast<const SDK::UCrBuildingData*>(definition);
    o.UI=uint8_t(building->UIType);o.Subtype=uint8_t(building->UISubType);
    const auto dynamic=*reinterpret_cast<const uint8_t*>(&building->CanBeDynamic);
    if(dynamic>1){o.BuildingData="invalid_subclass_fields";return;}
    o.Subclass=true;o.Dynamic=dynamic!=0;o.BuildingData="resident_validated";
    const C::ArrayHeader tags{reinterpret_cast<uintptr_t>(building->Tags.GetDataPtr()),building->Tags.Num(),building->Tags.Max()};
    const auto bounds=C::BoundArray(tags,sizeof(SDK::FName),alignof(SDK::FName),K::TagLimit);
    if(!bounds.Valid||(bounds.Bytes&&!Readable(tags.Data,bounds.Bytes,4))){o.TagsStatus="invalid";return;}
    o.TagsStatus=bounds.Status;o.Truncated|=bounds.Truncated;
    o.InteriorComparison="exact_FName_Equals_Interior";
    for(size_t i=0;i<bounds.Count;++i)
    {
        if(!Work.Spend(false)){o.TagsStatus="truncated_budget";o.Truncated=true;break;}
        const auto name=building->Tags[int32_t(i)];++o.TagVisits;
        if(name.ComparisonIndex<0){o.TagsStatus="invalid_element";break;}
        o.Tags[o.TagCount++]={name.ComparisonIndex,name.Number};
        // Read only names from the validated, retained definition's bounded array.
        // Same comparison used by installed FCrBuildingBaseView::IsInterior.
        // No pool enumeration, string allocation, name insertion or package load.
        if(name.Number==0&&MiniMapNative::Get()->asset.nameEqualsWide(name,L"Interior"))
            o.InteriorTag="present";
    }
    if(o.InteriorTag=="present")
    {K::Evidence evidence;evidence.InteriorTag=true;o.CachedPolicy=K::Decide(evidence);}
    else if(!bounds.Truncated&&o.TagCount==bounds.Count&&o.TagsStatus!="invalid_element"&&!Work.Exhausted)
        o.InteriorTag="absent";
}
