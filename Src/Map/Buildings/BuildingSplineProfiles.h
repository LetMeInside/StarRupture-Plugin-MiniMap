#pragma once
#include "BuildingGeometryTypes.h"
#include <vector>
#include "Native/DefinitionAcquisitionCore.h"
#include <algorithm>
#include <memory>
#include <string>

// Owned numerical snapshots. Template candidates retain approximate provenance.
namespace MiniMapBuildingSplineProfiles
{
    using Identity=MiniMapBuildingGeometryTypes::Identity;
    using Point=MiniMapBuildingGeometryTypes::Point;
    enum class State : uint8_t { Pending, Partial, Complete, Unsupported };
    enum class Availability : uint8_t { Unverified, Available, Pending, Unsupported, Invalid };
    template<class T> struct Field
    {
        T Value{};
        Availability Status=Availability::Unverified;
        bool operator==(const Field&) const=default;
    };
    struct Bounds
    {
        Point Origin{},Extent{};
        double SphereRadius=0;
        bool operator==(const Bounds&) const=default;
    };
    inline bool Valid(const Bounds& b)
    {
        return MiniMapBuildingGeometryTypes::Finite(b.Origin) && MiniMapBuildingGeometryTypes::Finite(b.Extent) &&
            b.Extent[0]>=0 && b.Extent[1]>=0 && b.Extent[2]>=0 &&
            std::isfinite(b.SphereRadius) && b.SphereRadius>=0;
    }
    struct RelativeTransform
    {
        // Unreal units, native FRotator pitch/yaw/roll in degrees. Not world TRS.
        Point Translation{},RotationDegrees{},Scale{};
        bool operator==(const RelativeTransform&) const=default;
    };
    struct CrossSection
    {
        std::array<double,2> StartScale{},EndScale{},StartOffset{},EndOffset{};
        double StartRoll=0,EndRoll=0; // Radians, native spline-mesh convention.
        Point Up{};
        bool operator==(const CrossSection&) const=default;
    };
    enum class Mechanism : uint8_t { Unknown, ModularWalkway, DroneRail };
    enum class Selection : uint8_t { Candidate, NativeBuilderInput };
    enum class Origin : uint8_t { MassRepresentation, PlacementActorClass };
    enum class ClassLookup : uint8_t
    {
        Unattempted, CanonicalUnavailable, CanonicalInvalid, FacilityUnavailable,
        Interrupted, ReturnedNull, TypeRejected, IdentityUnverified, IdentityRejected, ClassValidated, Count
    };
    enum class MeshAcquisition : uint8_t
    { Unexamined, Null, TransientInvalid, ClassMismatch, BoundsReadFailed, InvalidBounds, Copied,
      RetainFailed, Interrupted, ReleaseFailed, Count };
    enum class MeshSerial : uint8_t { Unobserved, Zero, Positive, Negative, Count };
    inline const char* Label(MeshAcquisition value)
    {
        constexpr const char* labels[]={"unexamined","null","transient_invalid","class_mismatch","bounds_read_failed",
            "invalid_bounds","copied","retain_failed","interrupted","release_failed"};
        return size_t(value)<size_t(MeshAcquisition::Count)?labels[size_t(value)]:"unknown";
    }
    struct Mesh
    {
        Field<Bounds> LocalBounds;
        bool operator==(const Mesh&) const=default;
    };
    enum class Backing : uint8_t { PersistentIdentity, NativeCopy };
    // An owned commit seal, not a native-object identity. Each successful copy
    // gets a new seal; sharing it only shares that immutable observation.
    struct NativeCopyObservation {};
    struct Profile
    {
        Backing Ownership=Backing::PersistentIdentity;
        std::shared_ptr<const NativeCopyObservation> CopyObservation;
        Field<Identity> VisualClass,Defaults,TemplateOwner,Template;
        Origin Provenance=Origin::MassRepresentation;
        // High/low is meaningful only for MassRepresentation. Placement is an
        // independent authored source, never a verified high/low alternative.
        bool HighResolution=true;
        Field<Mechanism> Construction;
        Field<RelativeTransform> TemplateTransform,ConstructionTransform;
        Field<CrossSection> TemplateCrossSection,ConstructionCrossSection;
        Field<uint8_t> TemplateForwardAxis,ConstructionForwardAxis;
        // Diagnostic outcome only: no mesh object address, index or serial identity survives.
        MeshAcquisition MeshRead=MeshAcquisition::Unexamined;
        MeshSerial ObservedMeshSerial=MeshSerial::Unobserved;
        // Set only for a verified spline-mesh component in the owning class template array.
        bool SplineTemplate=false;
        std::shared_ptr<const Mesh> Geometry;
        Selection Source=Selection::Candidate;
        // A native builder input is verified independently of whether a derived
        // Blueprint executes that builder unchanged, or adds other components.
        Availability ActiveUse=Availability::Unverified;
        State Status=State::Partial;
        bool operator==(const Profile&) const=default;
    };
    struct Set
    {
        struct Discovery
        {
            Availability Placement=Availability::Pending,EntityConfig=Availability::Pending;
            Availability ActorClassReference=Availability::Unverified,ResidentClass=Availability::Unverified;
            Availability PlacementInventory=Availability::Unverified,MassInventory=Availability::Unverified;
            std::shared_ptr<const std::string> CanonicalClassPath;
            ClassLookup ResidentLookup=ClassLookup::Unattempted;
            MiniMapDefinitionAcquisition::IdentityObservation IdentityValidation;
            MiniMapDefinitionAcquisition::IdentityObservation LookupValidation,PublicationIdentity,TraversalZero;
            bool ResidentLookupExecuted=false,ResidentLookupCompleted=false,ResidentObjectFound=false,CdoAbsent=false;
            Availability CdoAvailability=Availability::Unverified;
            bool NativeCopyAttempted=false;
            Availability NativeNumericalCopy=Availability::Unverified,PersistentClassIdentity=Availability::Unverified;
            MeshAcquisition NativeCopyMesh=MeshAcquisition::Unexamined;
            bool PlacementUsable=false; // Certified with the existing numerical-input eligibility policy.
            bool MassCacheAbsent=false; // Null cache, distinct from an unreadable/pending resident config.
            bool TraitTypesReady=false;
            size_t Configs=0,Traits=0,VisualTraits=0,Classes=0,Defaults=0,NativeBuilders=0,Templates=0;
            bool operator==(const Discovery&) const=default;
        };
        uint64_t Generation=0;
        Field<Identity> Definition;
        std::vector<Identity> Resources; // Configuration/trait dependencies.
        std::vector<Profile> Profiles;
        Availability ActiveSet=Availability::Unverified;
        Availability Inventory=Availability::Unverified;
        State Status=State::Pending;
        Discovery Acquisition; // Owned evidence for the existing bounded debug summaries.
        bool operator==(const Set&) const=default;
    };
    inline bool Copied(const Profile& profile){return profile.Ownership==Backing::NativeCopy;}
    inline bool Pending(const Profile& profile)
    {
        if(Copied(profile))
            return !profile.CopyObservation || profile.Source!=Selection::NativeBuilderInput ||
                profile.Status==State::Pending || !profile.Geometry ||
                profile.Geometry->LocalBounds.Status!=Availability::Available ||
                profile.ConstructionForwardAxis.Status!=Availability::Available ||
                profile.ConstructionTransform.Status!=Availability::Available ||
                profile.ConstructionCrossSection.Status!=Availability::Available;

        return profile.Status==State::Pending ||
            profile.VisualClass.Status==Availability::Pending || profile.Defaults.Status==Availability::Pending ||
            profile.TemplateOwner.Status==Availability::Pending || profile.Template.Status==Availability::Pending ||
            profile.Construction.Status==Availability::Pending || profile.TemplateTransform.Status==Availability::Pending ||
            profile.ConstructionTransform.Status==Availability::Pending || profile.TemplateCrossSection.Status==Availability::Pending ||
            profile.ConstructionCrossSection.Status==Availability::Pending || profile.TemplateForwardAxis.Status==Availability::Pending ||
            profile.ConstructionForwardAxis.Status==Availability::Pending ||
            (!profile.Geometry && profile.MeshRead!=MeshAcquisition::Unexamined &&
                profile.MeshRead!=MeshAcquisition::ClassMismatch && profile.MeshRead!=MeshAcquisition::InvalidBounds) ||
            profile.ActiveUse==Availability::Pending ||
            (profile.Geometry && profile.Geometry->LocalBounds.Status==Availability::Pending);
    }
    inline bool Pending(const Set& set)
    {
        return set.Definition.Status==Availability::Pending || set.Inventory==Availability::Pending ||
            std::any_of(set.Profiles.begin(),set.Profiles.end(),[](const Profile& profile){return Pending(profile);});
    }
    inline bool NeedsRetry(const Set& set,uint64_t generation)
    {
        if(set.Generation!=generation)return true;
        // A committed numerical copy is owned data, not a live native owner.
        // Retry other initial dependencies without reopening that completed copy.
        // In particular do not hide a still-pending Mass source behind placement.
        if(std::any_of(set.Profiles.begin(),set.Profiles.end(),[](const Profile& p){return Copied(p) && !Pending(p);}))
            return set.Definition.Status!=Availability::Available || set.Inventory!=Availability::Available || Pending(set);
        // Failed/uncommitted observations keep the existing retry behavior.
        if(set.Acquisition.NativeCopyAttempted ||
            std::any_of(set.Profiles.begin(),set.Profiles.end(),[](const Profile& p){return Copied(p);}))return true;
        // An unexamined null Mass cache is still Pending evidence. It need not
        // drive repeated metadata traversal once placement alone supplies a
        // settled, usable approximation. Actual pending placement resources or
        // an available configuration retain their normal retry behavior.
        const bool placementReady=set.Acquisition.PlacementInventory==Availability::Available && set.Acquisition.PlacementUsable;
        if(placementReady && set.Definition.Status==Availability::Available &&
            set.Acquisition.MassCacheAbsent && set.Acquisition.EntityConfig==Availability::Pending && set.Acquisition.Configs==0 &&
            std::none_of(set.Profiles.begin(),set.Profiles.end(),[](const Profile& p)
                {return p.Provenance==Origin::PlacementActorClass && Pending(p);}))return false;
        return Pending(set);
    }
    inline void Finalize(Set& set)
    {
        bool complete=!set.Profiles.empty() && set.Definition.Status==Availability::Available && set.Inventory==Availability::Available;
        bool unsupported=!set.Profiles.empty();
        for(const auto& profile:set.Profiles)
        {
            complete=complete && profile.Status==State::Complete && profile.ActiveUse==Availability::Available;
            unsupported=unsupported && profile.Status==State::Unsupported;
        }
        set.ActiveSet=complete?Availability::Available:Availability::Unverified;
        set.Status=complete?State::Complete:Pending(set)?State::Pending:
            unsupported?State::Unsupported:State::Partial;
    }
}
