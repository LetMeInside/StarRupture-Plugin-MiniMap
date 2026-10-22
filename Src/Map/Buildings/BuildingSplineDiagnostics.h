#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineEnvelope.h"
#include <sstream>
#include <string>
#include <unordered_set>

// Read-only explanation of already prepared, owned inputs. Never prepares
// geometry or resolves native metadata; called only at the debug cadence.
namespace MiniMapBuildingSplineDiagnostics
{
    namespace E=MiniMapBuildingSplineEnvelope;
    namespace P=MiniMapBuildingSplineProfiles;
    namespace C=MiniMapBuildingCollector;
    namespace S=MiniMapBuildingSplineGeometry;
    enum class Cause : size_t
    {
        Drawable,NoProfileOwner,EmptyProfiles,Definition,Sampling,MeshAcquisition,
        MeshBounds,ForwardAxis,ComponentTransform,CrossSection,UnsupportedProjection,
        RejectedCells,Other,Count
    };
    inline const char* Label(Cause value)
    {
        constexpr const char* labels[]={"drawable","no_profile_owner","empty_profiles","definition_identity_generation",
            "logical_sampling","mesh_acquisition","mesh_bounds","forward_axis","component_transform","cross_section",
            "unsupported_projection","degenerate_frame_or_cells","other"};
        return labels[size_t(value)];
    }
    inline const char* Label(P::Availability value)
    {
        switch(value){case P::Availability::Available:return "available";case P::Availability::Pending:return "pending";
        case P::Availability::Unsupported:return "unsupported";case P::Availability::Invalid:return "invalid";default:return "unverified";}
    }
    inline const char* Label(P::ClassLookup outcome)
    {
        constexpr const char* labels[]={"unattempted","canonical_unavailable","canonical_invalid","facility_unavailable",
            "interrupted","returned_null","type_rejected","identity_unverified","identity_rejected","class_validated"};
        const auto index=size_t(outcome);
        return index<sizeof(labels)/sizeof(labels[0])?labels[index]:"unknown";
    }
    inline const char* Label(E::State value)
    {
        switch(value){case E::State::Partial:return "partial";case E::State::Complete:return "complete";
        case E::State::Unsupported:return "unsupported";case E::State::Invalid:return "invalid";default:return "pending";}
    }
    inline std::array<P::Availability,5> Fields(const P::Profile& profile)
    {
        const auto inputs=E::Inputs(profile);
        const auto mesh=profile.Geometry?P::Availability::Available:
            profile.MeshRead==P::MeshAcquisition::ClassMismatch || profile.MeshRead==P::MeshAcquisition::InvalidBounds?
                P::Availability::Invalid:P::Availability::Pending;
        return {mesh,profile.Geometry?profile.Geometry->LocalBounds.Status:P::Availability::Unverified,
            inputs.ForwardAxis.Status,inputs.Transform.Status,inputs.CrossSection.Status};
    }
    inline Cause ProfileCause(const P::Profile& profile,uint32_t reasons)
    {
        const auto fields=Fields(profile);
        constexpr std::array<Cause,5> causes={Cause::MeshAcquisition,Cause::MeshBounds,Cause::ForwardAxis,
            Cause::ComponentTransform,Cause::CrossSection};
        // Eligibility rejects explicit invalid states before missing states.
        for(size_t i=0;i<fields.size();++i)if(fields[i]==P::Availability::Invalid)return causes[i];
        for(size_t i=0;i<fields.size();++i)if(fields[i]!=P::Availability::Available)return causes[i];
        if(reasons&E::InvalidSource)
        {
            const auto inputs=E::Inputs(profile);const auto& cross=inputs.CrossSection.Value;
            for(const auto* pair:{&cross.StartScale,&cross.EndScale,&cross.StartOffset,&cross.EndOffset})
                for(double value:*pair)if(!E::V::Finite(E::V::Point{value,0,0}))return Cause::CrossSection;
            auto up=cross.Up;
            if(!std::isfinite(cross.StartRoll) || !std::isfinite(cross.EndRoll) || !E::Normalize(up))return Cause::CrossSection;
            E::V::Transform component;
            if(!E::ComponentTransform(inputs.Transform.Value,component))return Cause::ComponentTransform;
            if(!profile.Geometry ||
                !P::Valid(profile.Geometry->LocalBounds.Value))return Cause::MeshBounds;
            if(inputs.ForwardAxis.Value>2)return Cause::ForwardAxis;
        }
        if(reasons&E::UnsupportedProjection)return Cause::UnsupportedProjection;
        if(reasons&(E::DegenerateFrame|E::DegenerateProjection))return Cause::RejectedCells;
        return Cause::Other; // e.g. invalid numeric values despite Available, or discontinuity only.
    }
    inline Cause First(const C::Record& record,const S::PreparedCurve* sampled,const E::Result* result,size_t& first)
    {
        first=size_t(-1);
        // Mirror Prepare's early-return order, rather than infer from aggregate reason bits.
        if(!record.HasSpline)return Cause::Other;
        if(!record.Curve || !sampled)return Cause::Sampling;
        const auto metadata=E::Metadata(record);
        if(!metadata)return Cause::NoProfileOwner;
        if(!record.DefinitionValid || metadata->Definition.Status!=P::Availability::Available ||
            metadata->Generation!=record.Generation ||
            metadata->Definition.Value!=P::Identity{record.DefinitionObjectIndex,record.DefinitionObjectSerial})return Cause::Definition;
        if(metadata->Profiles.empty())return Cause::EmptyProfiles;
        if(result && (result->Reasons&E::InvalidRoot))return Cause::Other;
        if(record.Curve->Issue!=C::GeometryIssue::None || sampled->SourceIssue!=C::GeometryIssue::None ||
            sampled->PreparationIssue!=C::GeometryIssue::None || sampled->Sections.size()!=record.Curve->Segments.size())return Cause::Sampling;
        if(!result)return Cause::Other;
        if(result->Status==E::State::Partial || result->Status==E::State::Complete)return Cause::Drawable;
        // When every profile failed, source order defines the representative
        // first failure. All profiles' availability is counted separately below.
        if(!result->Profiles.empty())
        {
            first=result->Profiles.front().SourceProfile;
            if(first<metadata->Profiles.size())return ProfileCause(metadata->Profiles[first],result->Profiles.front().Reasons);
        }
        return Cause::Other;
    }
    struct Summary
    {
        static constexpr size_t ExampleLimit=4;
        size_t Records=0,Profiles=0,ValidBounds=0,BoundsMissingOther=0,MissingFieldsProfiles=0;
        std::array<size_t,size_t(Cause::Count)> Counts{};
        std::array<std::array<size_t,5>,5> Availability{};
        std::array<std::array<size_t,5>,5> MissingAvailability{};
        std::array<std::string,ExampleLimit> Examples;
        std::array<uint64_t,ExampleLimit> ExampleKeys{};
        size_t ExampleCount=0;
        std::array<std::string,ExampleLimit> LookupExamples;
        size_t LookupExampleCount=0;
        std::unordered_set<uint64_t> Definitions;
        std::array<size_t,5> Authored{},Resident{},Cdo{},PlacementInventory{},MassInventory{};
        std::array<size_t,size_t(P::ClassLookup::Count)> ResidentLookups{};
        size_t PlacementProfiles=0,MassProfiles=0,NativeBuilders=0,BlueprintCandidates=0,CopiedBounds=0,PendingDependencies=0;
        size_t NativeCopies=0,PersistentClasses=0,UnavailableClassIdentities=0;
        std::array<size_t,size_t(P::MeshAcquisition::Count)> MeshReads{};
        std::array<std::array<size_t,size_t(P::MeshAcquisition::Count)>,2> BuilderMeshReads{};
        std::array<size_t,size_t(P::MeshSerial::Count)> MeshSerials{};
        std::array<std::array<size_t,size_t(P::MeshSerial::Count)>,2> BuilderMeshSerials{};
        size_t NumericalEligible=0,NumericalBlocked=0,PreparedPartial=0,PreparedComplete=0;
        Cause Add(const C::Record& record,const S::PreparedCurve* sampled,const E::Result* envelope,bool selectable)
        {
            PreparedPartial+=selectable && envelope && envelope->Status==E::State::Partial;
            PreparedComplete+=selectable && envelope && envelope->Status==E::State::Complete;
            ++Records;size_t first;auto cause=First(record,sampled,envelope,first);
            if(cause==Cause::Drawable && !selectable)cause=Cause::Other;
            ++Counts[size_t(cause)];
            const auto metadata=E::Metadata(record);
            if(metadata && metadata->Definition.Status==P::Availability::Available &&
                Definitions.insert(uint64_t(uint32_t(metadata->Definition.Value.Index)) |
                    (uint64_t(uint32_t(metadata->Definition.Value.Serial))<<32)).second)
            {
                const auto& a=metadata->Acquisition;
                ++Authored[size_t(a.ActorClassReference)];++Resident[size_t(a.ResidentClass)];
                ++ResidentLookups[size_t(a.ResidentLookup)];
                NativeCopies+=a.NativeNumericalCopy==P::Availability::Available;
                PersistentClasses+=a.PersistentClassIdentity==P::Availability::Available;
                UnavailableClassIdentities+=a.ResidentObjectFound && a.PersistentClassIdentity!=P::Availability::Available;
                ++PlacementInventory[size_t(a.PlacementInventory)];++MassInventory[size_t(a.MassInventory)];
                PendingDependencies+=a.PlacementInventory==P::Availability::Pending;
                PendingDependencies+=a.MassInventory==P::Availability::Pending;
                for(const auto& profile:metadata->Profiles)
                {
                    (profile.Provenance==P::Origin::PlacementActorClass?PlacementProfiles:MassProfiles)++;
                    ++Cdo[size_t(profile.Defaults.Status)];
                    NativeBuilders+=profile.Source==P::Selection::NativeBuilderInput;
                    BlueprintCandidates+=profile.Template.Status==P::Availability::Available;
                    CopiedBounds+=profile.Geometry && profile.Geometry->LocalBounds.Status==P::Availability::Available;
                    ++MeshReads[size_t(profile.MeshRead)];++MeshSerials[size_t(profile.ObservedMeshSerial)];
                    if(profile.Source==P::Selection::NativeBuilderInput)
                    {
                        const auto builder=profile.Construction.Value==P::Mechanism::DroneRail?size_t(1):size_t(0);
                        ++BuilderMeshReads[builder][size_t(profile.MeshRead)];
                        ++BuilderMeshSerials[builder][size_t(profile.ObservedMeshSerial)];
                    }
                    PendingDependencies+=P::Pending(profile);
                }
                if(LookupExampleCount<ExampleLimit)
                {
                    std::ostringstream lookup;
                    lookup<<"definition="<<metadata->Definition.Value.Index<<':'<<metadata->Definition.Value.Serial
                        <<" authored="<<Label(a.ActorClassReference)<<" path="
                        <<(a.CanonicalClassPath?*a.CanonicalClassPath:"<unavailable>")
                        <<" lookup="<<Label(a.ResidentLookup)<<" executed="<<a.ResidentLookupExecuted
                        <<" completed="<<a.ResidentLookupCompleted<<" objectFound="<<a.ResidentObjectFound
                        <<" cdo="<<Label(a.CdoAvailability)
                        <<" cdoAbsent="<<a.CdoAbsent
                        <<" nativeNumericalCopy="<<Label(a.NativeNumericalCopy)
                        <<" persistentClassIdentity="<<Label(a.PersistentClassIdentity)
                        <<" nativeCopyMesh="<<P::Label(a.NativeCopyMesh);
                    const auto& identity=a.IdentityValidation;
                    lookup<<" identityPredicate="<<MiniMapDefinitionAcquisition::Label(identity.Predicate);
                    lookup<<" role="<<MiniMapDefinitionAcquisition::Label(identity.Role)<<" depth="<<identity.Depth
                        <<" isLookupResult="<<identity.IsLookupResult<<" transient="<<MiniMapDefinitionAcquisition::Label(identity.Transient);
                    if(identity.IndexObserved)lookup<<" observedIndex="<<identity.Index;
                    if(identity.ObjectFlagsObserved)lookup<<" uobjectFlags="<<identity.ObjectFlags;
                    if(identity.SlotObserved)lookup<<" slotSerial="<<identity.Serial<<" fuobjectitemFlags="<<identity.ItemFlags;
                    if(identity.Predicate==MiniMapDefinitionAcquisition::IdentityPredicate::SlotRead ||
                        identity.Predicate==MiniMapDefinitionAcquisition::IdentityPredicate::SlotValues)
                        lookup<<" slotStatus="<<MiniMapDefinitionAcquisition::Label(identity.SlotStatus);
                    lookup<<" lookupTransient="<<MiniMapDefinitionAcquisition::Label(a.LookupValidation.Transient)
                        <<" publicationIdentity="<<MiniMapDefinitionAcquisition::Label(a.PublicationIdentity.Predicate);
                    if(a.LookupValidation.SlotObserved)lookup<<" lookupIndex="<<a.LookupValidation.Index<<" lookupSerial="<<a.LookupValidation.Serial;
                    if(a.PublicationIdentity.SlotObserved)lookup<<" ownerIndex="<<a.PublicationIdentity.Index<<" ownerSerial="<<a.PublicationIdentity.Serial;
                    if(a.TraversalZero.SlotObserved)
                        lookup<<" firstZeroRole="<<MiniMapDefinitionAcquisition::Label(a.TraversalZero.Role)
                            <<" firstZeroDepth="<<a.TraversalZero.Depth<<" firstZeroIsLookup="<<a.TraversalZero.IsLookupResult
                            <<" firstZeroIndex="<<a.TraversalZero.Index<<" firstZeroSerial="<<a.TraversalZero.Serial
                            <<" firstZeroUobjectFlags="<<a.TraversalZero.ObjectFlags<<" firstZeroItemFlags="<<a.TraversalZero.ItemFlags
                            <<" firstZeroTransient="<<MiniMapDefinitionAcquisition::Label(a.TraversalZero.Transient);
                    LookupExamples[LookupExampleCount++]=lookup.str();
                }
            }
            if(metadata)for(const auto& profile:metadata->Profiles)
            {
                ++Profiles;const auto fields=Fields(profile);
                for(size_t i=0;i<fields.size();++i)++Availability[i][size_t(fields[i])];
                const auto eligibility=E::Eligibility(profile);
                MissingFieldsProfiles+=eligibility==E::MissingFields;
                NumericalEligible+=eligibility==E::None;NumericalBlocked+=eligibility!=E::None;
                if(eligibility==E::MissingFields)
                    for(size_t i=0;i<fields.size();++i)if(fields[i]!=P::Availability::Available)
                        ++MissingAvailability[i][size_t(fields[i])];
                const bool bounds=profile.Geometry && fields[1]==P::Availability::Available &&
                    P::Valid(profile.Geometry->LocalBounds.Value);
                ValidBounds+=bounds;
                BoundsMissingOther+=bounds && (fields[2]!=P::Availability::Available ||
                    fields[3]!=P::Availability::Available || fields[4]!=P::Availability::Available);
            }
            if(cause==Cause::Drawable || ExampleCount==ExampleLimit)return cause;
            const P::Profile* profile=metadata && first<metadata->Profiles.size()?&metadata->Profiles[first]:nullptr;
            uint64_t key=uint64_t(cause);
            if(profile)
            {
                key|=uint64_t(profile->Source)<<8;
                const auto fields=Fields(*profile);
                for(size_t i=0;i<fields.size();++i)key|=uint64_t(fields[i])<<(12+3*i);
            }
            if(metadata && cause==Cause::EmptyProfiles)
            {
                const auto& acquisition=metadata->Acquisition;
                key|=uint64_t(acquisition.Placement)<<8;
                key|=uint64_t(acquisition.EntityConfig)<<11;
                key|=uint64_t(acquisition.TraitTypesReady)<<14;
                key|=uint64_t(acquisition.Configs!=0)<<15;
                key|=uint64_t(acquisition.Traits!=0)<<16;
                key|=uint64_t(acquisition.VisualTraits!=0)<<17;
            }
            for(size_t i=0;i<ExampleCount;++i)if(ExampleKeys[i]==key)return cause;
            std::ostringstream out;
            out<<"entity="<<record.Index<<':'<<record.Serial<<" def="<<record.DefinitionObjectIndex<<':'<<record.DefinitionObjectSerial
                <<" gen="<<record.Generation<<" cause="<<Label(cause)<<" shape="<<bool(record.DefinitionShape)<<" owner="<<bool(metadata)
                <<" profiles="<<(metadata?metadata->Profiles.size():0)<<" envelope="<<(envelope?Label(envelope->Status):"absent")
                <<" reasons="<<(envelope?envelope->Reasons:0)<<" sampling="<<bool(sampled);
            if(cause==Cause::Other)
                out<<" detail="<<(!envelope?"no_prepared_envelope":(envelope->Reasons&E::InvalidRoot)?"invalid_root":
                    (envelope->Status==E::State::Partial || envelope->Status==E::State::Complete)?"selection_rejected":
                    (envelope->Reasons&E::Discontinuity)?"discontinuous_sections":
                    envelope->Profiles.empty()?"no_prepared_profile_results":"invalid_source_or_no_accepted_cells");
            if(sampled)out<<" sampleIssues="<<unsigned(sampled->SourceIssue)<<'/'<<unsigned(sampled->PreparationIssue)
                <<" sampledSections="<<sampled->Sections.size();
            if(metadata)out<<" setDef="<<metadata->Definition.Value.Index<<':'<<metadata->Definition.Value.Serial
                <<" setGen="<<metadata->Generation<<" definition="<<Label(metadata->Definition.Status)
                <<" inventory="<<Label(metadata->Inventory);
            if(metadata && cause==Cause::EmptyProfiles)
            {
                const auto& acquisition=metadata->Acquisition;
                out<<" placement="<<Label(acquisition.Placement)<<" entityConfig="<<Label(acquisition.EntityConfig)
                    <<" traitTypes="<<acquisition.TraitTypesReady<<" configs="<<acquisition.Configs
                    <<" traits="<<acquisition.Traits<<" visualTraits="<<acquisition.VisualTraits
                    <<" classes="<<acquisition.Classes<<" defaults="<<acquisition.Defaults
                    <<" nativeBuilders="<<acquisition.NativeBuilders<<" templates="<<acquisition.Templates;
            }
            if(profile)
            {
                constexpr const char* names[]={"mesh","bounds","axis","transform","cross"};
                const auto fields=Fields(*profile);
                out<<" profile="<<first<<" source="<<(profile->Source==P::Selection::Candidate?"template":"native_builder")
                    <<" provenance="<<(profile->Provenance==P::Origin::PlacementActorClass?"placement":"mass")
                    <<" high="<<profile->HighResolution<<" meshRead="<<P::Label(profile->MeshRead)
                    <<" visualClass="<<profile->VisualClass.Value.Index<<':'<<profile->VisualClass.Value.Serial
                    <<" templateOwner="<<profile->TemplateOwner.Value.Index<<':'<<profile->TemplateOwner.Value.Serial
                    <<" template="<<profile->Template.Value.Index<<':'<<profile->Template.Value.Serial
                    <<" eligibility="<<E::Eligibility(*profile)<<" geometryOwner="<<bool(profile->Geometry);
                if(envelope)for(const auto& prepared:envelope->Profiles)if(prepared.SourceProfile==first)
                {out<<" profileStatus="<<Label(prepared.Status)<<" profileReasons="<<prepared.Reasons;break;}
                for(size_t i=0;i<fields.size();++i)out<<' '<<names[i]<<'='<<Label(fields[i]);
                out<<" templateTuple="<<Label(profile->TemplateForwardAxis.Status)<<'/'<<Label(profile->TemplateTransform.Status)<<'/'<<Label(profile->TemplateCrossSection.Status)
                    <<" constructionTuple="<<Label(profile->ConstructionForwardAxis.Status)<<'/'<<Label(profile->ConstructionTransform.Status)<<'/'<<Label(profile->ConstructionCrossSection.Status);
                if(E::Eligibility(*profile)==E::UnsupportedProjection)
                {
                    const auto inputs=E::Inputs(*profile);const auto& cross=inputs.CrossSection.Value;
                    out<<" axisValue="<<unsigned(inputs.ForwardAxis.Value)<<" roll="<<cross.StartRoll<<'/'<<cross.EndRoll
                        <<" scale="<<cross.StartScale[0]<<','<<cross.StartScale[1]<<'/'<<cross.EndScale[0]<<','<<cross.EndScale[1]
                        <<" offset="<<cross.StartOffset[0]<<','<<cross.StartOffset[1]<<'/'<<cross.EndOffset[0]<<','<<cross.EndOffset[1];
                }
            }
            ExampleKeys[ExampleCount]=key;Examples[ExampleCount++]=out.str();
            return cause;
        }
        std::string Text() const
        {
            std::ostringstream out;
            out<<"Spline envelope inputs: records="<<Records<<" profiles="<<Profiles<<" validAssetBounds="<<ValidBounds
                <<" boundsMissingOther="<<BoundsMissingOther<<" missingFieldsProfiles="<<MissingFieldsProfiles
                <<" numericalEligible="<<NumericalEligible<<" numericalBlocked="<<NumericalBlocked
                <<" preparedPartial="<<PreparedPartial<<" preparedComplete="<<PreparedComplete;
            for(size_t i=0;i<Counts.size();++i)out<<' '<<Label(Cause(i))<<'='<<Counts[i];
            out<<"\nField availability [unverified,available,pending,unsupported,invalid]";
            constexpr const char* names[]={"mesh","bounds","axis","transform","cross"};
            for(size_t i=0;i<Availability.size();++i)
            {out<<' '<<names[i]<<'=';for(size_t j=0;j<5;++j)out<<(j?"/":"")<<Availability[i][j];}
            out<<"\nMissingFields-only absent inputs [unverified,available,pending,unsupported,invalid]";
            for(size_t i=0;i<MissingAvailability.size();++i)
            {out<<' '<<names[i]<<'=';for(size_t j=0;j<5;++j)out<<(j?"/":"")<<MissingAvailability[i][j];}
            return out.str();
        }
        std::string AcquisitionText() const
        {
            std::ostringstream out;
            out<<"Spline acquisition by distinct definition: definitions="<<Definitions.size()<<" placementProfiles="<<PlacementProfiles
                <<" massProfiles="<<MassProfiles<<" nativeBuilders="<<NativeBuilders<<" blueprintCandidates="<<BlueprintCandidates
                <<" nativeNumericalCopies="<<NativeCopies<<" persistentClasses="<<PersistentClasses
                <<" unavailableClassIdentities="<<UnavailableClassIdentities
                <<" copiedBounds="<<CopiedBounds<<" pendingDependencies="<<PendingDependencies
                <<" numericalEligible="<<NumericalEligible<<" numericalBlocked="<<NumericalBlocked
                <<" preparedPartial="<<PreparedPartial<<" preparedComplete="<<PreparedComplete
                <<" [unverified/available/pending/unsupported(missing)/invalid]";
            constexpr const char* names[]={"authoredClass","residentClass","cdo","placementInventory","massInventory"};
            const std::array<const std::array<size_t,5>*,5> fields={&Authored,&Resident,&Cdo,&PlacementInventory,&MassInventory};
            for(size_t i=0;i<fields.size();++i)
            {out<<' '<<names[i]<<'=';for(size_t j=0;j<5;++j)out<<(j?"/":"")<<(*fields[i])[j];}
            out<<" residentLookup[unattempted,canonical_unavailable,canonical_invalid,facility_unavailable,interrupted,returned_null,type_rejected,identity_unverified,identity_rejected,class_validated]=";
            for(size_t i=0;i<ResidentLookups.size();++i)out<<(i?"/":"")<<ResidentLookups[i];
            out<<" meshReads[";
            for(size_t i=0;i<MeshReads.size();++i)out<<(i?",":"")<<P::Label(P::MeshAcquisition(i));
            out<<"]=";
            for(size_t i=0;i<MeshReads.size();++i)out<<(i?"/":"")<<MeshReads[i];

            out<<" meshSerial[unobserved,zero,positive,negative]=";
            for(size_t i=0;i<MeshSerials.size();++i)out<<(i?"/":"")<<MeshSerials[i];
            for(size_t builder=0;builder<2;++builder)
            {
                out<<(builder?" droneMeshReads=":" walkwayMeshReads=");
                for(size_t i=0;i<BuilderMeshReads[builder].size();++i)out<<(i?"/":"")<<BuilderMeshReads[builder][i];
                out<<(builder?" droneMeshSerial=":" walkwayMeshSerial=");
                for(size_t i=0;i<BuilderMeshSerials[builder].size();++i)out<<(i?"/":"")<<BuilderMeshSerials[builder][i];
            }
            return out.str();
        }
    };
}
#endif
