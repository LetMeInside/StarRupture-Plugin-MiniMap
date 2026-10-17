// Candidate validation only. Unreal constructs the selected Blueprint's components.
MiniMapSpriteValidation::Result GenericAudit()
{
    namespace V=MiniMapSpriteValidation;
    V::Result out;out.Resident=AuditClass!=0;out.Route="private_blueprint";out.Selected=Selected;
    if(!AuditClass||ProbeResult.DefinitionIdentity!="valid"||ProbeResult.Truncated||ProbeResult.Incomplete||!BlueprintIdentity.Path)
    {out.Status=V::Outcome::Conditional;out.Reason="candidate_identity_or_blueprint_unavailable";return out;}
    if(Classification.CachedPolicy.ExcludeFromMiniMap==MiniMapSpriteClassification::Exclusion::Exclude)
    {out.Status=V::Outcome::Unsupported;out.Reason="classification_excluded";return out;}
    MiniMapSpriteCapture::Candidate candidate;candidate.General=true;
    candidate.Identity=MiniMapSpriteCandidates::Copy(Record).Identity;
    candidate.CanonicalBlueprint=BlueprintIdentity.Path;
    candidate.Classification=Classification.CachedPolicy;
    candidate.ClassificationObserved=Classification.Fixed&&Classification.Subclass&&!Classification.Truncated&&Classification.InteriorTag!="unknown";
    const auto* type=reinterpret_cast<const SDK::UObject*>(AuditClass);
    candidate.ActorObject=int32_t(type->Index);candidate.ActorName=type->Name.ComparisonIndex;candidate.ActorNumber=type->Name.Number;
    candidate.Tier=SelectedTier=="low_detail_actor"?MiniMapSpriteDiscoveryCore::Tier::Low:MiniMapSpriteDiscoveryCore::Tier::High;
    candidate.Truncated=false;
    out.Status=V::Outcome::Eligible;out.Reason="private_blueprint_candidate";
    out.AuditRendererRevision=0; // Session candidate identity is the source; no resource revisions.
#if defined(MINIMAP_BUILDING_SPRITES)
    if(CaptureRequested&&!DeferCapture)
    {
        const auto decision=MiniMapBuildingCapture::OfferBlueprint(candidate,AuditClass,Record.BuildingId,MiniMapMap::GetWorld());
        out.RecordHandoff(decision);out.Reason=decision.Reason;
        if(out.Disposition==V::AuditDisposition::Completed&&!decision.Allowed())out.Status=V::Outcome::Conditional;
    }
#endif
    return out;
}
