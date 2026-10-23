#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

// Stage 41I diagnostics only. Fixed counters, copied values, no native access.
namespace MiniMapBuildingCollectorAudit
{
    template<class E> constexpr size_t Index(E value){return static_cast<size_t>(value);}
    template<class E> constexpr uint64_t Bit(E value){return uint64_t(1)<<Index(value);}
    enum class Block : uint8_t
    {
        Settled,NoAcquisition,NotLiveOrBuilt,MissingBuildingId,UnknownType,Definition,Transform,MissingCurve,Reconstructed,InvalidCurve,
        TintUnknown,InteriorUnknown,InteriorExcluded,Admission,FreshTransform,FreshPlacement,OtherReadiness,
        AwaitingPreparation,StalePreparation,Sampling,Envelope,RejectedCells,NativeProfile,TemplateInventory,
        AckRejected,SyntheticEligible,EligibleUnsettled,Count
    };
    inline constexpr const char* BlockNames[]={"settled","notObserved","notLiveOrBuilt","authoritativeId","unknownType","definition","transform","missingCurve","reconstructed","invalidCurve",
        "tintUnknown","interiorUnknown","interiorExcluded","admission","freshTransform","freshPlacement","otherReadiness",
        "awaitingPreparation","stalePreparation","sampling","envelope","rejectedCells","nativeProfile","profileOrInventoryPending","ackRejected","syntheticEligible","eligibleUnsettled"};
    enum class Ack : uint8_t
    {
        Settled,AlreadySettled,Inactive,Generation,ZeroRevision,RecordNotReady,MissingCurve,Reconstructed,MissingSampling,Sampling,
        Envelope,RejectedCells,SyntheticType,Metadata,ProfileIndex,NativeProfileOrInventory,NativeUnavailable,MassNotLive,SourceAbsent,StaleRevision,Other,Count
    };
    inline constexpr const char* AckNames[]={"settled","already","inactive","generation","zeroRevision","recordNotReady","missingCurve","reconstructed","missingSampling","sampling",
        "envelope","rejectedCells","syntheticType","metadata","profileIndex","nativeProfileOrInventory","nativeUnavailable","massNotLive","sourceAbsent","staleRevision","other"};
    enum class Revision : uint8_t {Identity,Definition,Readiness,Transform,Tint,Admission,Canonical,ProfileOwner,CurveOwner,Count};
    inline constexpr const char* RevisionNames[]={"identity","definition","readiness","transform","tint","admission","canonical","profileOwner","curveOwner"};
    enum class Publication : uint8_t
    {FastReuse,CanonicalReuse,NoPrevious,Generation,Region,Query,Completeness,SerialReplacement,Added,Removed,Sources,Order,Ready,Revision,Other,Failed,Count};
    inline constexpr const char* PublicationNames[]={"fastReuse","canonicalReuse","noPrevious","generation","region","query","completeness","serialReplacementAtOrdinal","added","removed","sources","order","ready","revision","other","failed"};
    template<class E> struct Counts
    {
        std::array<uint64_t,Index(E::Count)> Values{};
        void Add(E value){++Values[Index(value)];}
        void AddMask(uint64_t bits){for(size_t i=0;i<Values.size();++i)Values[i]+=(bits&(uint64_t(1)<<i))!=0;}
        uint64_t Total() const {uint64_t n=0;for(auto value:Values)n+=value;return n;}
    };
    struct Events
    {
        Counts<Ack> Acknowledgements;
        Counts<Publication> Publications,PublicationFlags;
        Counts<Revision> RevisionPrimary,RevisionFlags,PublishedRevisionFlags;
        uint64_t Prepared=0,SamplingComplete=0,Eligible=0,AckAttempts=0,RevisionEvents=0,OrdinaryRevisions=0,SplineRevisions=0;
        uint64_t ReadyGained=0,ReadyLost=0,InitialDefinition=0,InitialCurve=0,InitialProfile=0,RevisionOnlyPublished=0,PublishedOrdinary=0,PublishedSpline=0,TypeKnownGained=0,TintKnownGained=0,InteriorKnownGained=0;
    };
    struct Census
    {
        uint64_t Known=0,Admitted=0,Ready=0,Settled=0,CurrentPrepared=0,SyntheticEligible=0,NumericallyEligible=0;
        std::array<std::array<uint64_t,4>,4> Category{}; // known/admitted/ready/settled, W/R/Z/other
        Counts<Block> Primary,Overlapping;
        void Add(size_t category,bool admitted,bool ready,bool settled,bool prepared,uint64_t mask,bool eligible=false)
        {
            ++Known;Admitted+=admitted;Ready+=ready;Settled+=settled;CurrentPrepared+=prepared;NumericallyEligible+=eligible;
            SyntheticEligible+=!settled && (mask&Bit(Block::SyntheticEligible))!=0;
            auto& c=Category[category];++c[0];c[1]+=admitted;c[2]+=ready;c[3]+=settled;
            Overlapping.AddMask(mask);
            if(settled){Primary.Add(Block::Settled);return;}
            mask&=~Bit(Block::SyntheticEligible);
            for(size_t i=1;i<Index(Block::Count);++i)if(mask&(uint64_t(1)<<i)){++Primary.Values[i];return;}
            Primary.Add(Block::EligibleUnsettled);
        }
    };
    template<class T,size_t N> bool Exact(const std::array<T,N>& a,const std::array<T,N>& b)
    {
        for(size_t i=0;i<N;++i)
            if constexpr(sizeof(T)==8){if(std::bit_cast<uint64_t>(a[i])!=std::bit_cast<uint64_t>(b[i]))return false;}
            else {if(std::bit_cast<uint32_t>(a[i])!=std::bit_cast<uint32_t>(b[i]))return false;}
        return true;
    }
    template<class B> bool BoundsEqual(const B& a,const B& b)
    {return a.Valid==b.Valid && Exact(std::array{a.MinX,a.MinY,a.MaxX,a.MaxY},std::array{b.MinX,b.MinY,b.MaxX,b.MaxY});}
    template<class R> uint64_t Difference(const R& a,const R& b)
    {
        uint64_t mask=0;
        auto different=[&](bool same,Revision field){if(!same)mask|=Bit(field);};
        different(a.Generation==b.Generation && a.Index==b.Index && a.Serial==b.Serial,Revision::Identity);
        different(a.BuildingId==b.BuildingId && a.DefinitionIndex==b.DefinitionIndex && a.DefinitionNumber==b.DefinitionNumber &&
            a.DefinitionObjectIndex==b.DefinitionObjectIndex && a.DefinitionObjectSerial==b.DefinitionObjectSerial && a.DefinitionValid==b.DefinitionValid,Revision::Definition);
        different(a.TransformValid==b.TransformValid && a.HasSpline==b.HasSpline && a.InteriorKnown==b.InteriorKnown &&
            a.InteriorOnly==b.InteriorOnly && a.SplineReady==b.SplineReady && a.SplineTintKnown==b.SplineTintKnown,Revision::Readiness);
        different(Exact(a.Position,b.Position)&&Exact(a.Scale,b.Scale)&&Exact(a.Rotation,b.Rotation),Revision::Transform);
        different(Exact(a.Tint,b.Tint),Revision::Tint);
        different(BoundsEqual(a.Extent,b.Extent)&&BoundsEqual(a.SourceFootprint,b.SourceFootprint)&&a.LocalFootprint.Valid==b.LocalFootprint.Valid&&
            Exact(a.LocalFootprint.Min,b.LocalFootprint.Min)&&Exact(a.LocalFootprint.Max,b.LocalFootprint.Max)&&
            std::bit_cast<uint64_t>(a.SplineInflation)==std::bit_cast<uint64_t>(b.SplineInflation)&&a.GeometryStatus==b.GeometryStatus&&
            a.UsesSafetyAllowance==b.UsesSafetyAllowance&&a.InvalidFootprintSource==b.InvalidFootprintSource&&
            a.UnboundedGeometry==b.UnboundedGeometry&&a.BoundsKind==b.BoundsKind,Revision::Admission);
        different(a.CanonicalBlueprint==b.CanonicalBlueprint,Revision::Canonical);
        different(a.DefinitionShape==b.DefinitionShape,Revision::ProfileOwner);different(a.Curve==b.Curve,Revision::CurveOwner);
        return mask;
    }
    template<class R> void ObserveRevision(Events& events,const R& a,const R& b)
    {
        ++events.RevisionEvents;
        const bool spline=a.HasSpline||b.HasSpline||a.DynamicObserved||b.DynamicObserved;
        if(spline)++events.SplineRevisions;else ++events.OrdinaryRevisions;
        const auto mask=Difference(a,b);events.RevisionFlags.AddMask(mask);
        for(size_t i=0;i<Index(Revision::Count);++i)if(mask&(uint64_t(1)<<i)){++events.RevisionPrimary.Values[i];break;}
        events.ReadyGained+=!a.SplineReady&&b.SplineReady;events.ReadyLost+=a.SplineReady&&!b.SplineReady;
        events.TintKnownGained+=!a.SplineTintKnown&&b.SplineTintKnown;events.InteriorKnownGained+=!a.InteriorKnown&&b.InteriorKnown;
        events.InitialDefinition+=!a.DefinitionValid&&b.DefinitionValid;
        events.InitialCurve+=!a.Curve&&bool(b.Curve);events.InitialProfile+=!a.DefinitionShape&&bool(b.DefinitionShape);
    }
    template<class R> uint64_t Identity(const R& r){return (uint64_t(uint32_t(r.Serial))<<32)|uint32_t(r.Index);}
    // Fold into the existing materialization pass. No record history/index,
    // allocation, second canonical pass or native inspection.
    template<class Records> struct Population
    {
        const Records* Previous=nullptr;
        Events* Event=nullptr;
        size_t Ordinal=0,Cursor=0;
        uint64_t Flags=0,PublishedRevisionReasons=0;
        bool Equal=true,OrdinaryChanged=false,SplineChanged=false;
        explicit Population(const Records* previous,Events* event):Previous(previous),Event(event){}
        void Observe(const typename Records::value_type& r)
        {
            if(!Previous){Equal=false;Flags|=Bit(Publication::Added);++Ordinal;return;}
            if(Ordinal>=Previous->size())Equal=false;
            else
            {
                const auto& old=(*Previous)[Ordinal];
                Equal=Equal && old.Generation==r.Generation && old.Index==r.Index && old.Serial==r.Serial && old.RenderRevision==r.RenderRevision && old.Sources==r.Sources;
                if(old.Index!=r.Index || old.Serial!=r.Serial)Flags|=Bit(Publication::Order);
                if(old.Index==r.Index && old.Serial!=r.Serial)Flags|=Bit(Publication::SerialReplacement);
            }
            while(Cursor<Previous->size() && Identity((*Previous)[Cursor])<Identity(r)){Flags|=Bit(Publication::Removed);++Cursor;}
            if(Cursor>=Previous->size() || Identity((*Previous)[Cursor])!=Identity(r))Flags|=Bit(Publication::Added);
            else
            {
                const auto& old=(*Previous)[Cursor++];
                if(old.Generation!=r.Generation)Flags|=Bit(Publication::Generation);
                if(old.Sources!=r.Sources)Flags|=Bit(Publication::Sources);
                if(old.RenderRevision!=r.RenderRevision)
                {
                    Flags|=Bit(Publication::Revision);OrdinaryChanged|=!r.HasSpline;SplineChanged|=r.HasSpline;
                    if(!old.SplineReady&&r.SplineReady)Flags|=Bit(Publication::Ready);
                    const auto mask=Difference(old,r);PublishedRevisionReasons|=mask;
                    if(!mask && Event)++Event->RevisionOnlyPublished;
                }
            }
            ++Ordinal;
        }
        void Finish()
        {if(Previous && Cursor<Previous->size())Flags|=Bit(Publication::Removed);Equal=Equal && Previous && Ordinal==Previous->size();}
    };
    inline Publication PrimaryPublication(uint64_t mask)
    {for(size_t i=Index(Publication::NoPrevious);i<Index(Publication::Other);++i)if(mask&(uint64_t(1)<<i))return static_cast<Publication>(i);return Publication::Other;}
    static_assert(std::size(BlockNames)==Index(Block::Count));
    static_assert(std::size(AckNames)==Index(Ack::Count));
    static_assert(std::size(RevisionNames)==Index(Revision::Count));
    static_assert(std::size(PublicationNames)==Index(Publication::Count));
}
