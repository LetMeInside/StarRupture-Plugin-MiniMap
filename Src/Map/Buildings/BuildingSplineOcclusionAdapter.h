#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "DiagnosticExports.h"
#include "BuildingSplineOcclusion.h"
#include "BuildingVisuals.h" // Reuse the actual SelectSpline policy; never submit draws here.
#include "BuildingSplinePolicy.h"
#include <memory>
#include <limits>
#include <optional>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <bit>

namespace MiniMapBuildingSplineOcclusionAdapter
{
    namespace O=MiniMapBuildingSplineOcclusion;
    namespace C=MiniMapBuildingCollector;
    namespace E=MiniMapBuildingSplineEnvelope;
    struct OrdinarySubmission
    {
        size_t RecordIndex=0;
        // Copy the EXACT world quad used for a successful AtlasQuad/SubmitQuad,
        // not the collector targeting proxy or a reconstructed rectangle.
        std::array<O::XY,4> Quad{};
        O::Point Min{},Max{}; // Copied AtlasEntry assembly bounds, never an entry pointer.
        bool Submitted=false;
        bool operator==(const OrdinarySubmission&) const=default;
    };
    struct SplineSource
    {
        size_t RecordIndex=0;
        std::shared_ptr<const E::Result> Envelope;
        uint8_t Category=3; // Snapshot-local submission category: walkway/rail/zipline/other.
        bool operator==(const SplineSource&) const=default;
    };
    struct Policy
    {
        uint64_t Version=5; // Numerical policy and dependency/evidence reuse contract; bump when changed.
        bool operator==(const Policy&) const=default;
    };
    struct Request
    {
        // Published source objects must be immutable, including their nested data.
        std::shared_ptr<const C::Snapshot> Snapshot;
        uint64_t AtlasSession=0,AtlasRevision=0;
        std::vector<OrdinarySubmission> Ordinary; // Actual submission order, successes AND optional misses.
        std::vector<SplineSource> Splines; // Unique record indices, snapshot-local order.
        Policy NumericalPolicy;
    };
    enum class Failure : uint8_t { Context,InvalidOccluder,Selection,SectionData,Planner,AllocationFailure };
    struct Example {size_t Record=0,Section=0;Failure Cause=Failure::Context;O::Status Planner=O::Status::Complete;};
    struct Diagnostics
    {
        std::array<size_t,6> Counts{};
        std::vector<Example> Examples;
        bool AllocationFailed=false;
        size_t PlannerAllocationFailures=0,PlannerInvalidInputs=0;
        void Add(size_t record,size_t section,Failure cause,O::Status planner=O::Status::Complete)
        {
            Counts[size_t(cause)]=O::AddSize(Counts[size_t(cause)],1);
            if(cause==Failure::Planner && planner==O::Status::AllocationFailure)PlannerAllocationFailures=O::AddSize(PlannerAllocationFailures,1);
            if(cause==Failure::Planner && planner==O::Status::InvalidInput)PlannerInvalidInputs=O::AddSize(PlannerInvalidInputs,1);
            if(!AllocationFailed)
            {
                try{Examples.push_back({record,section,cause,planner});}
                catch(const std::bad_alloc&){AllocationFailed=true;}
                catch(const std::length_error&){AllocationFailed=true;}
            }
        }
    };
    struct Dependency
    {
        size_t RecordIndex=0;
        O::Occluder Value; // Canonical geometry AND current request-slot key, in processing order.
    };
    struct SectionGeometry
    {
        std::vector<O::Triangle> Visible; // Complete result of exactly ONE section transaction.
        std::vector<O::Triangle> WalkwayInsets; // Final XYZ, left/right fan order; Key is the band start offset.
        std::vector<O::Triangle> SourceTriangles; // Original section XYZ; never clipped/rewritten.
        O::AABB SourceBounds{}; // Union of BOTH original triangle AABBs; conservative strict broad phase.
    };
    struct SectionPlan
    {
        size_t SectionIndex=0;
        std::shared_ptr<const SectionGeometry> Geometry;
        std::vector<Dependency> Dependencies;
        O::RemovalDiagnostics Removed; // Always in this wrapper's current dependency/request key space.
        bool Reusable=false; // Set only after geometry, dependencies and local evidence complete.
    };
    struct RecordPlan
    {
        size_t RecordIndex=0,SelectedProfile=0,SourceProfile=0;
        uint8_t Category=3;
        std::vector<std::shared_ptr<const SectionPlan>> Sections; // Immutable transactions; no triangle-array transfer copies.
        size_t FailedSections=0;
    };
    struct CoverageExample
    {
        size_t SplineRecord=0,OrdinaryRecord=0,Section=0;
        uint8_t Category=3;
        double StartZ=0,EndZ=0;
        bool FullyRemoved=false; // FINAL section result, after all canonical occluders.
        O::Removal Evidence;
    };
    struct CoverageDiagnostics
    {
        size_t Sections=0,FullyRemovedSections=0,Polygons=0;
        long double RemovedArea=0;
        std::vector<CoverageExample> Examples;
        bool AllocationFailed=false,PlannerExamplesAllocationFailed=false;
    };
    struct RenderPlan
    {
        Request Binding; // Own source snapshot/envelopes and the exact ordered submission observations.
        uint64_t Generation=0;
        std::vector<RecordPlan> Records;
        Diagnostics Diagnostic;
        CoverageDiagnostics Coverage; // Successful subtraction, independent of failure diagnostics.
        size_t SectionsAttempted=0,Triangles=0,WalkwayInsetTriangles=0; // Triangles retains its body-only meaning.
        bool AllocationFailed=false; // Never cache/reuse a resource-exhausted attempt.
        std::array<size_t,4> DrawableRecords{}; // Records with surviving geometry, never fragment counts.
    };

    enum class Invalidation : uint8_t
    { None,NoPrevious,Source,AtlasSession,AtlasRevision,Policy,Splines,Ordinary,Allocation,Incomplete,Context,Count };
    enum class PreparationPhase : uint8_t { Binding,Occluders,Dependencies,Body,Insets,Assembly,Translation,Evidence,Count };
    struct PreparationWork
    {
        uint64_t Reused=0,Recomputed=0,RejectedSource=0,RejectedOccluders=0,RejectedKeys=0,RejectedLifecycle=0,
            ReusedBody=0,ReusedInsets=0,PreparedInsets=0;
        uint64_t Remapped=0,RemappedBody=0,RemappedInsets=0,PreparedBody=0,TranslatedExamples=0,
            RejectedIdentity=0,RejectedGeometry=0,RejectedOrder=0,RejectedEvidence=0,RejectedNonReusable=0,TranslationFailures=0;
        std::array<double,size_t(PreparationPhase::Count)> Ms{};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        void Add(const PreparationWork& b) noexcept
        {
            Reused+=b.Reused;Recomputed+=b.Recomputed;RejectedSource+=b.RejectedSource;
            RejectedOccluders+=b.RejectedOccluders;RejectedKeys+=b.RejectedKeys;RejectedLifecycle+=b.RejectedLifecycle;
            ReusedBody+=b.ReusedBody;ReusedInsets+=b.ReusedInsets;PreparedInsets+=b.PreparedInsets;
            Remapped+=b.Remapped;RemappedBody+=b.RemappedBody;RemappedInsets+=b.RemappedInsets;
            PreparedBody+=b.PreparedBody;TranslatedExamples+=b.TranslatedExamples;
            RejectedIdentity+=b.RejectedIdentity;RejectedGeometry+=b.RejectedGeometry;RejectedOrder+=b.RejectedOrder;
            RejectedEvidence+=b.RejectedEvidence;RejectedNonReusable+=b.RejectedNonReusable;TranslationFailures+=b.TranslationFailures;
            for(size_t i=0;i<Ms.size();++i)Ms[i]+=b.Ms[i];
        }
#endif
    };
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    struct PreparationTimer
    {
        using Clock=std::chrono::steady_clock;
        PreparationWork* Work;
        PreparationPhase Phase;
        Clock::time_point Begin;
        PreparationTimer(PreparationWork* work,PreparationPhase phase):Work(work),Phase(phase),Begin(work?Clock::now():Clock::time_point{}) {}
        ~PreparationTimer()
        {if(Work)Work->Ms[size_t(Phase)]+=std::chrono::duration<double,std::milli>(Clock::now()-Begin).count();}
    };
#endif

    inline Invalidation Mismatch(const RenderPlan* cached,const Request& request)
    {
        if(!cached)return Invalidation::NoPrevious;
        if(cached->AllocationFailed || cached->Coverage.AllocationFailed || cached->Coverage.PlannerExamplesAllocationFailed ||
            cached->Diagnostic.AllocationFailed)return Invalidation::Allocation;
        const auto& a=cached->Binding;
        if(!request.Snapshot || a.Snapshot!=request.Snapshot || cached->Generation!=request.Snapshot->Generation)return Invalidation::Source;
        if(a.AtlasSession!=request.AtlasSession)return Invalidation::AtlasSession;
        if(a.AtlasRevision!=request.AtlasRevision)return Invalidation::AtlasRevision;
        if(a.NumericalPolicy!=request.NumericalPolicy)return Invalidation::Policy;
        if(a.Splines!=request.Splines)return Invalidation::Splines;
        if(a.Ordinary!=request.Ordinary)return Invalidation::Ordinary;
        return Invalidation::None;
    }

    inline O::AABB SectionBounds(const std::vector<O::Triangle>& triangles)
    {
        O::AABB bounds{{INFINITY,INFINITY},{-INFINITY,-INFINITY}};
        for(const auto& t:triangles)
        {
            const auto box=O::Bounds(t.Vertices);
            for(size_t axis=0;axis<2;++axis)
            {bounds.Min[axis]=(std::min)(bounds.Min[axis],box.Min[axis]);bounds.Max[axis]=(std::max)(bounds.Max[axis],box.Max[axis]);}
        }
        return bounds;
    }
    inline bool SameGeometry(const O::Occluder& a,const O::Occluder& b)
    {
        // Stronger than value equality for signed zero; no hashing or tolerance.
        const auto same=[](double x,double y){return std::bit_cast<uint64_t>(x)==std::bit_cast<uint64_t>(y);};
        if(a.Active!=b.Active || !same(a.TopZ,b.TopZ))return false;
        for(size_t i=0;i<4;++i)for(size_t axis=0;axis<2;++axis)if(!same(a.Corners[i][axis],b.Corners[i][axis]))return false;
        return true;
    }
    enum class SectionMatch : uint8_t { Exact,Remap,Identity,Geometry,Order,Evidence,NonReusable,Translation };
    inline SectionMatch MatchDependencies(const SectionPlan& old,const O::PreparedOccluders& prepared,
        const std::vector<size_t>& candidates,const Request& before,const Request& request,bool& keysChanged)
    {
        keysChanged=false;
        if(old.Removed.AllocationFailed || old.Removed.Examples.size()!=old.Removed.Polygons)return SectionMatch::Evidence;
        if(!old.Reusable || !old.Geometry)return SectionMatch::NonReusable;
        if(old.Dependencies.size()!=candidates.size())return SectionMatch::Identity;
        for(size_t i=0;i<candidates.size();++i)
        {
            const auto& current=prepared.Items()[candidates[i]].Value;const auto& previous=old.Dependencies[i];
            if(previous.Value.Key>=before.Ordinary.size() || current.Key>=request.Ordinary.size() ||
                previous.RecordIndex!=before.Ordinary[size_t(previous.Value.Key)].RecordIndex ||
                (i && (previous.Value.Key<=old.Dependencies[i-1].Value.Key ||
                    current.Key<=prepared.Items()[candidates[i-1]].Value.Key)))return SectionMatch::Translation;
            if(previous.RecordIndex!=request.Ordinary[size_t(current.Key)].RecordIndex)
            {
                keysChanged=false;
                // Already rejected: classify a permutation without letting diagnostic
                // allocation failure prevent normal fresh numerical preparation.
                try
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    std::vector<size_t> a,b;a.reserve(candidates.size());b.reserve(candidates.size());
                    for(const auto& dependency:old.Dependencies)a.push_back(dependency.RecordIndex);
#endif
                    for(size_t candidate:candidates)
                    {
                        const auto key=prepared.Items()[candidate].Value.Key;
                        if(key>=request.Ordinary.size())return SectionMatch::Translation;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        b.push_back(request.Ordinary[size_t(key)].RecordIndex);
#endif
                    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
                    return a==b?SectionMatch::Order:SectionMatch::Identity;
#endif
                }
                catch(const std::bad_alloc&){return SectionMatch::Identity;}
                catch(const std::length_error&){return SectionMatch::Identity;}
                return SectionMatch::Identity;
            }
            if(!SameGeometry(previous.Value,current)){keysChanged=false;return SectionMatch::Geometry;}
            keysChanged|=previous.Value.Key!=current.Key;
        }
        for(const auto& evidence:old.Removed.Examples)
        {
            const auto found=std::lower_bound(old.Dependencies.begin(),old.Dependencies.end(),evidence.OccluderKey,
                [](const auto& dependency,uint64_t key){return dependency.Value.Key<key;});
            if(found==old.Dependencies.end() || found->Value.Key!=evidence.OccluderKey)return SectionMatch::Translation;
        }
        return keysChanged?SectionMatch::Remap:SectionMatch::Exact;
    }
    inline std::shared_ptr<const SectionPlan> RemapSection(const SectionPlan& old,const O::PreparedOccluders& prepared,
        const std::vector<size_t>& candidates,const Request& request)
    {
        auto remapped=std::make_shared<SectionPlan>();
        remapped->SectionIndex=old.SectionIndex;remapped->Geometry=old.Geometry;
        remapped->Removed=old.Removed; // Copy evidence only; never recalculate any non-key value.
        remapped->Dependencies.reserve(candidates.size());
        for(size_t candidate:candidates)
        {
            const auto& value=prepared.Items()[candidate].Value;
            remapped->Dependencies.push_back({request.Ordinary[size_t(value.Key)].RecordIndex,value});
        }
        for(auto& evidence:remapped->Removed.Examples)
        {
            const auto found=std::lower_bound(old.Dependencies.begin(),old.Dependencies.end(),evidence.OccluderKey,
                [](const auto& dependency,uint64_t key){return dependency.Value.Key<key;});
            // MatchDependencies proved every range, identity and unique ordered pair.
            if(found==old.Dependencies.end() || found->Value.Key!=evidence.OccluderKey)return {};
            evidence.OccluderKey=remapped->Dependencies[size_t(found-old.Dependencies.begin())].Value.Key;
        }
        remapped->Reusable=true;return remapped;
    }

    struct WalkwayInsetScratch
    {
        O::Detail::Polygon Subject,Inside,Outside;
        WalkwayInsetScratch() {Subject.reserve(8);Inside.reserve(8);Outside.reserve(8);}
        void Clear() noexcept {Subject.clear();Inside.clear();Outside.clear();}
    };
    inline bool WalkwayInsetBands(const MiniMapBuildingSplineEnvelope::Section& section,double fraction,
        std::array<O::XY,4>& left,std::array<O::XY,4>& right)
    {
        if(!std::isfinite(fraction) || !(fraction>0.0) || !(fraction<0.5))return false;
        const auto inset=[&](const O::XY& outer,const O::XY& opposite)
        {return O::XY{outer[0]+(opposite[0]-outer[0])*fraction,outer[1]+(opposite[1]-outer[1])*fraction};};
        const auto& start=section.Start.Edges;const auto& end=section.End.Edges;
        const auto leftStart=inset(start[0],start[1]),leftEnd=inset(end[0],end[1]);
        const auto rightStart=inset(start[1],start[0]),rightEnd=inset(end[1],end[0]);
        left={start[0],end[0],leftEnd,leftStart};
        right={rightStart,rightEnd,end[1],start[1]};
        for(const auto* band:{&left,&right})
        {
            for(const auto& point:*band)if(!std::isfinite(point[0]) || !std::isfinite(point[1]))return false;
            for(size_t i=0;i<4;++i)
            {
                const auto turn=O::Detail::Turn((*band)[i],(*band)[(i+1)%4],(*band)[(i+2)%4]);
                if(!std::isfinite(turn) || !(turn>0.0L))return false;
            }
        }
        return true;
    }
    inline void ClipWalkwayInsetTriangle(const O::Triangle& source,const std::array<O::XY,4>& band,
        WalkwayInsetScratch& scratch,std::vector<O::Triangle>& output,bool& complete)
    {
        const auto committed=output.size();
        try
        {
            O::Triangle triangle=source;O::Detail::Canonical(triangle);
            scratch.Subject.clear();scratch.Inside.clear();scratch.Outside.clear();
            for(size_t i=0;i<3;++i)
            {
                O::Detail::Vertex vertex;vertex.P=triangle.Vertices[i];
                vertex.Supports[0]=O::Detail::MakeEdge(triangle.Vertices[i],triangle.Vertices[(i+1)%3]);
                vertex.Supports[1]=O::Detail::MakeEdge(triangle.Vertices[i],triangle.Vertices[(i+2)%3]);
                vertex.Count=2;scratch.Subject.push_back(vertex);
            }
            for(size_t edge=0;edge<4;++edge)
            {
                O::Detail::Plane plane;plane.Height=false;plane.A=band[edge];plane.B=band[(edge+1)%4];
                scratch.Inside.clear();scratch.Outside.clear();
                O::Detail::Split(scratch.Subject,plane,scratch.Inside,scratch.Outside);
                scratch.Subject.swap(scratch.Inside);scratch.Inside.clear();scratch.Outside.clear();
                if(!O::Detail::HasArea(scratch.Subject)){scratch.Clear();return;}
            }
            for(size_t i=1;i+1<scratch.Subject.size();++i)
            {
                const auto& a=scratch.Subject[0].P;const auto& b=scratch.Subject[i].P;const auto& c=scratch.Subject[i+1].P;
                const auto worldArea=O::Detail::Area(a,b,c);
                if(!std::isfinite(worldArea) || !(worldArea>0.0L))continue;
                output.push_back({{a,b,c},uint64_t(committed)}); // Inset key identifies this band transaction.
            }
        }
        catch(O::Status) {output.resize(committed);complete=false;} // Original per-band numerical rejection.
        // Allocation failures propagate to the existing section transaction.
        scratch.Clear();
    }
    inline bool PrepareWalkwayInsets(const E::Section& section,const std::vector<O::Triangle>& visible,
        WalkwayInsetScratch& scratch,std::vector<O::Triangle>& output)
    {
        std::array<O::XY,4> left{},right{};
        if(!WalkwayInsetBands(section,MiniMapBuildingSplinePolicy::kWalkwayInsetFractionPerSide,left,right))return false;
        bool complete=true;
        for(const auto& triangle:visible)
        {
            ClipWalkwayInsetTriangle(triangle,left,scratch,output,complete);
            ClipWalkwayInsetTriangle(triangle,right,scratch,output,complete);
        }
        return complete;
    }

    inline bool MakeOccluder(const C::Record& record,const OrdinarySubmission& submission,uint64_t key,O::Occluder& out)
    {
        if(!submission.Submitted || !record.TransformValid || record.HasSpline)return false;
        double norm=0;
        for(double q:record.Rotation){if(!std::isfinite(q))return false;norm+=q*q;}
        if(!std::isfinite(norm) || !(norm>0))return false;
        const double inverse=1/std::sqrt(norm);
        const double x=record.Rotation[0]*inverse,y=record.Rotation[1]*inverse,
            z=record.Rotation[2]*inverse,w=record.Rotation[3]*inverse;
        for(size_t axis=0;axis<3;++axis)
            if(!std::isfinite(record.Position[axis]) || !std::isfinite(record.Scale[axis]) ||
                !std::isfinite(submission.Min[axis]) || !std::isfinite(submission.Max[axis]) || submission.Max[axis]<submission.Min[axis])return false;
        O::Occluder candidate{submission.Quad,-std::numeric_limits<double>::infinity(),key,true};
        for(unsigned mask=0;mask<8;++mask)
        {
            O::Point v;
            for(size_t axis=0;axis<3;++axis)
                v[axis]=((mask&(1u<<axis))?submission.Max[axis]:submission.Min[axis])*record.Scale[axis];
            // Same normalized quaternion, signed scale, and expression order as
            // BuildWorldQuad. All eight corners matter under tilt/negative scale.
            const double tx=2*(y*v[2]-z*v[1]),ty=2*(z*v[0]-x*v[2]),tz=2*(x*v[1]-y*v[0]);
            const O::Point world{record.Position[0]+v[0]+w*tx+y*tz-z*ty,
                record.Position[1]+v[1]+w*ty+z*tx-x*tz,record.Position[2]+v[2]+w*tz+x*ty-y*tx};
            for(double value:world)if(!std::isfinite(value))return false;
            candidate.TopZ=(std::max)(candidate.TopZ,world[2]);
        }
        try {auto checked=candidate;O::Detail::Canonical(checked);}catch(O::Status){return false;}
        out=candidate;return true; // Preserve handed-in XY order; planner canonicalizes its own copy.
    }

    inline bool SectionInput(const E::Result& envelope,const E::Profile& profile,size_t sectionIndex,O::Input& output)
    {
        if(!envelope.Sampling || sectionIndex>=profile.Sections.size())return false;
        const auto& section=profile.Sections[sectionIndex];const auto& samples=*envelope.Sampling;
        if(section.VertexCount!=4 || section.IndexCount!=6 || section.SourceSegment>=samples.Sections.size() ||
            section.FirstVertex>profile.Vertices.size() || profile.Vertices.size()-section.FirstVertex<4 ||
            section.FirstIndex>profile.Indices.size() || profile.Indices.size()-section.FirstIndex<6 ||
            !std::isfinite(section.StartT) || !std::isfinite(section.EndT) || section.StartT>=section.EndT)return false;
        const auto& sampled=samples.Sections[section.SourceSegment];
        if(sampled.Discontinuous || sampled.First>samples.Samples.size() || sampled.Count>samples.Samples.size()-sampled.First)return false;
        const auto begin=samples.Samples.begin()+sampled.First,end=begin+sampled.Count;
        const auto start=std::lower_bound(begin,end,section.StartT,[](const auto& a,double t){return a.T<t;});
        if(start==end || start->T!=section.StartT || start+1==end || (start+1)->T!=section.EndT)return false;
        const double firstZ=start->Position[2],lastZ=(start+1)->Position[2];
        if(!std::isfinite(firstZ) || !std::isfinite(lastZ))return false;
        std::array<size_t,6> indices;
        for(size_t i=0;i<6;++i)
        {
            const size_t index=profile.Indices[section.FirstIndex+i];
            if(index<section.FirstVertex || index-section.FirstVertex>=4)return false;
            indices[i]=index-section.FirstVertex;
        }
        if(indices!=std::array<size_t,6>{0,1,2,0,2,3} && indices!=std::array<size_t,6>{0,1,3,1,2,3})return false;
        O::Input candidate;
        for(size_t i=0;i<2;++i)
        {
            O::Triangle triangle;triangle.Key=i;
            for(size_t j=0;j<3;++j)
            {
                const size_t local=indices[i*3+j];const auto& xy=profile.Vertices[section.FirstVertex+local];
                triangle.Vertices[j]={xy[0],xy[1],(local==0 || local==3)?firstZ:lastZ};
            }
            candidate.Triangles.push_back(triangle);
        }
        output=std::move(candidate);return true;
    }

    inline bool Compatible(const RenderPlan& cached,const Request& request)
    {return Mismatch(&cached,request)==Invalidation::None;}

    inline void Coverage(RenderPlan& result,const RecordPlan& record,const SectionPlan& section)
    {
        const auto& removed=section.Removed;
        if(!removed.Polygons)return;
        auto& coverage=result.Coverage;
        ++coverage.Sections;
        const bool fully=section.Geometry->Visible.empty();
        coverage.FullyRemovedSections+=fully;
        coverage.Polygons+=removed.Polygons;coverage.RemovedArea+=removed.Area;
        coverage.PlannerExamplesAllocationFailed|=removed.AllocationFailed;
        for(const auto& evidence:removed.Examples)
        {
            if(evidence.OccluderKey>=result.Binding.Ordinary.size())continue;
            if(!coverage.AllocationFailed)
            {
                try{coverage.Examples.push_back({record.RecordIndex,
                    result.Binding.Ordinary[size_t(evidence.OccluderKey)].RecordIndex,section.SectionIndex,record.Category,
                    section.Geometry->SourceTriangles[0].Vertices[0][2],section.Geometry->SourceTriangles[0].Vertices[1][2],fully,evidence});}
                catch(const std::bad_alloc&){coverage.AllocationFailed=true;}
                catch(const std::length_error&){coverage.AllocationFailed=true;}
            }
        }
    }
    struct CoverageAllocationFailure {};
    inline std::shared_ptr<const RenderPlan> Prepare(const Request& request,std::shared_ptr<const RenderPlan> previous={},
        PreparationWork* work=nullptr,Invalidation discarded=Invalidation::NoPrevious)
    {
        const auto mismatch=Mismatch(previous.get(),request);
        if(mismatch==Invalidation::None)return previous;
        // Whole-plan semantics unchanged. Partial reuse is ONLY within the same
        // complete source/atlas/policy/envelope binding, with ordinary observations differing.
        const bool reuseSections=previous && mismatch==Invalidation::Ordinary;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const auto rejected=previous?mismatch:discarded;
#endif
        auto result=std::make_shared<RenderPlan>();
        auto retainCommitted=[&]
        {
            result->Triangles=0;result->WalkwayInsetTriangles=0;result->DrawableRecords={};result->AllocationFailed=true;
            for(const auto& record:result->Records)
            {
                bool drawable=false;
                for(const auto& section:record.Sections)
                {result->Triangles+=section->Geometry->Visible.size();result->WalkwayInsetTriangles+=section->Geometry->WalkwayInsets.size();drawable|=!section->Geometry->Visible.empty();}
                if(drawable)++result->DrawableRecords[(std::min)(size_t(record.Category),size_t(3))];
            }
            result->Diagnostic.Add(0,0,Failure::AllocationFailure,O::Status::AllocationFailure);
        };
        try
        {
            if(!request.Snapshot || ((!request.AtlasSession || !request.AtlasRevision) &&
                std::any_of(request.Ordinary.begin(),request.Ordinary.end(),[](const auto& o){return o.Submitted;})))
            {result->Diagnostic.Add(0,0,Failure::Context);return result;}
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                PreparationTimer timer(work,PreparationPhase::Binding);
#endif
                result->Binding=request;result->Generation=request.Snapshot->Generation;
            }
            const auto& records=request.Snapshot->Records;
            O::PreparedOccluders prepared;
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                PreparationTimer timer(work,PreparationPhase::Occluders);
#endif
                std::vector<O::Occluder> occluders;
                for(size_t i=0;i<request.Ordinary.size();++i)
                {
                    const auto& submitted=request.Ordinary[i];if(!submitted.Submitted)continue;
                    O::Occluder occluder;
                    if(submitted.RecordIndex>=records.size() || records[submitted.RecordIndex].Generation!=result->Generation ||
                        !MakeOccluder(records[submitted.RecordIndex],submitted,i,occluder))
                    {result->Diagnostic.Add(submitted.RecordIndex,0,Failure::InvalidOccluder);continue;}
                    occluders.push_back(occluder);
                }
                prepared=O::PrepareOccluders(occluders);
            }
            if(prepared.GetStatus()!=O::Status::Complete)
            {
                result->AllocationFailed=prepared.GetStatus()==O::Status::AllocationFailure;
                result->Diagnostic.Add(0,0,Failure::Planner,prepared.GetStatus());return result;
            }
            std::vector<size_t> visited,candidates; // One BVH-query scratch vector, reused across sections.
            size_t previousRecord=0;
            for(const auto& source:request.Splines)
            {
                const auto index=source.RecordIndex;
                if(index>=records.size() || !source.Envelope || records[index].Generation!=result->Generation ||
                    std::find(visited.begin(),visited.end(),index)!=visited.end())
                {result->Diagnostic.Add(index,0,Failure::Selection);continue;}
                visited.push_back(index);
                const auto* profile=MiniMapBuildingVisuals::Detail::SelectSpline(records[index],*source.Envelope);
                if(!profile){result->Diagnostic.Add(index,0,Failure::Selection);continue;}
                result->Records.push_back({index,size_t(profile-source.Envelope->Profiles.data()),profile->SourceProfile,source.Category,{}});
                auto& planned=result->Records.back();
                const RecordPlan* oldRecord=nullptr;
                if(reuseSections)
                {
                    while(previousRecord<previous->Records.size() && previous->Records[previousRecord].RecordIndex!=index)++previousRecord;
                    if(previousRecord<previous->Records.size())
                    {
                        const auto& old=previous->Records[previousRecord++];
                        if(old.SelectedProfile==planned.SelectedProfile && old.SourceProfile==planned.SourceProfile && old.Category==planned.Category)oldRecord=&old;
                    }
                }
                const auto* insetProfile=MiniMapBuildingSplinePolicy::IsWalkwayDefinition(records[index].DefinitionValid,records[index].BuildingId) &&
                    planned.SourceProfile<source.Envelope->Profiles.size()?&source.Envelope->Profiles[planned.SourceProfile]:nullptr;
                std::optional<WalkwayInsetScratch> insetScratch;
                size_t previousSection=0;
                for(size_t section=0;section<profile->Sections.size();++section)
                {
                    result->SectionsAttempted=O::AddSize(result->SectionsAttempted,1);
                    try
                    {
                        std::shared_ptr<const SectionPlan> old,completed;
                        SectionMatch match=SectionMatch::NonReusable;
                        O::Input input;O::AABB bounds;
                        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            PreparationTimer timer(work,PreparationPhase::Dependencies);
#endif
                            if(oldRecord)
                            {
                                while(previousSection<oldRecord->Sections.size() && oldRecord->Sections[previousSection]->SectionIndex<section)++previousSection;
                                if(previousSection<oldRecord->Sections.size() && oldRecord->Sections[previousSection]->SectionIndex==section)
                                    old=oldRecord->Sections[previousSection];
                            }
                            // Immutable source owner/profile identity proves the saved original XYZ and bounds.
                            if(old && old->Reusable && old->Geometry)bounds=old->Geometry->SourceBounds;
                            else
                            {
                                if(!SectionInput(*source.Envelope,*profile,section,input))
                                {++planned.FailedSections;result->Diagnostic.Add(index,section,Failure::SectionData);continue;}
                                bounds=SectionBounds(input.Triangles);
                            }
                            prepared.Candidates(bounds,candidates); // Includes non-removing and newly present candidates.
                            bool keysChanged=false;
                            match=old?MatchDependencies(*old,prepared,candidates,previous->Binding,request,keysChanged):SectionMatch::NonReusable;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            // Preserve the Stage42M3 gate count, including sections now remappable.
                            // The report explicitly labels this legacy pre-translation meaning.
                            if(work && old && (keysChanged || !old->Reusable))++work->RejectedKeys;
#endif
                            if(old && match==SectionMatch::Exact)completed=old;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            else if(work && match!=SectionMatch::Remap)
                            {
                                if(old)
                                {
                                    if(match==SectionMatch::Identity)++work->RejectedIdentity;
                                    if(match==SectionMatch::Geometry)++work->RejectedGeometry;
                                    if(match==SectionMatch::Order)++work->RejectedOrder;
                                    if(match==SectionMatch::Evidence)++work->RejectedEvidence;
                                    if(match==SectionMatch::NonReusable)++work->RejectedNonReusable;
                                    if(match==SectionMatch::Translation)++work->TranslationFailures;
                                    if(match==SectionMatch::Identity || match==SectionMatch::Geometry || match==SectionMatch::Order)++work->RejectedOccluders;
                                }
                                else if(rejected==Invalidation::AtlasSession || rejected==Invalidation::AtlasRevision || rejected==Invalidation::Allocation || rejected==Invalidation::Context)
                                    ++work->RejectedLifecycle;
                                else if(oldRecord || rejected==Invalidation::Source || rejected==Invalidation::Policy || rejected==Invalidation::Splines)
                                    ++work->RejectedSource;
                            }
#endif
                        }
                        if(old && match==SectionMatch::Remap)
                        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            PreparationTimer translation(work,PreparationPhase::Translation);
#endif
                            try{completed=RemapSection(*old,prepared,candidates,request);}
                            catch(const std::bad_alloc&)
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                if(work)++work->TranslationFailures;
#endif
                                throw;
                            }
                            catch(const std::length_error&)
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                if(work)++work->TranslationFailures;
#endif
                                throw;
                            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            if(!completed && work)++work->TranslationFailures;
#endif
                        }
                        if(!completed)
                        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            if(work)++work->Recomputed;
#endif
                            // No input reconstruction/copy on a reused section. On a changed
                            // dependency, saved original XYZ is read directly by the unchanged planner.
                            const auto& triangles=old && old->Reusable && old->Geometry?old->Geometry->SourceTriangles:input.Triangles;
                            O::Result plan;
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                PreparationTimer timer(work,PreparationPhase::Body);
#endif
                                plan=O::Plan(triangles,prepared);
                            }
                            if(plan.State!=O::Status::Complete)
                            {++planned.FailedSections;result->AllocationFailed|=plan.State==O::Status::AllocationFailure;
                             result->Diagnostic.Add(index,section,Failure::Planner,plan.State);continue;}
                            std::shared_ptr<SectionPlan> built;
                            std::shared_ptr<SectionGeometry> geometry;
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                PreparationTimer timer(work,PreparationPhase::Assembly);
#endif
                                built=std::make_shared<SectionPlan>();built->SectionIndex=section;
                                geometry=std::make_shared<SectionGeometry>();
                                geometry->Visible=std::move(plan.Visible);built->Removed=std::move(plan.Removed);geometry->SourceBounds=bounds;
                                built->Reusable=!built->Removed.AllocationFailed;
                                result->AllocationFailed|=built->Removed.AllocationFailed;
                            }
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                PreparationTimer timer(work,PreparationPhase::Insets);
#endif
                                if(insetProfile && section<insetProfile->Sections.size())
                                {
                                    if(!insetScratch)insetScratch.emplace();
                                    built->Reusable&=PrepareWalkwayInsets(insetProfile->Sections[section],geometry->Visible,*insetScratch,geometry->WalkwayInsets);
                                }
                            }
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                PreparationTimer timer(work,PreparationPhase::Dependencies);
#endif
                                if(old && old->Reusable && old->Geometry)geometry->SourceTriangles=old->Geometry->SourceTriangles;
                                else geometry->SourceTriangles=std::move(input.Triangles);
                                for(size_t candidate:candidates)
                                {
                                    const auto& value=prepared.Items()[candidate].Value;
                                    built->Dependencies.push_back({request.Ordinary[size_t(value.Key)].RecordIndex,value});
                                }
                            }
                            built->Geometry=std::move(geometry);completed=std::move(built);
                        }
                        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            PreparationTimer timer(work,PreparationPhase::Assembly);
#endif
                            const auto total=O::AddSize(result->Triangles,completed->Geometry->Visible.size());
                            const auto insetTotal=O::AddSize(result->WalkwayInsetTriangles,completed->Geometry->WalkwayInsets.size());
                            planned.Sections.push_back(completed);
                            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                PreparationTimer evidence(work,PreparationPhase::Evidence);
#endif
                                auto& c=result->Coverage;
                                const auto sections=c.Sections,fully=c.FullyRemovedSections,polygons=c.Polygons,examples=c.Examples.size();
                                const auto area=c.RemovedArea;const bool plannerFailed=c.PlannerExamplesAllocationFailed;
                                Coverage(*result,planned,*completed);
                                if(c.AllocationFailed)
                                {
                                    // Coverage catches allocation internally. Never commit/count a half-evidenced remap.
                                    c.Sections=sections;c.FullyRemovedSections=fully;c.Polygons=polygons;c.RemovedArea=area;
                                    c.Examples.resize(examples);c.PlannerExamplesAllocationFailed=plannerFailed;
                                    planned.Sections.pop_back();++planned.FailedSections;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                                    if(work && match==SectionMatch::Remap)++work->TranslationFailures;
#endif
                                    throw CoverageAllocationFailure{};
                                }
                            }
                            result->Triangles=total;result->WalkwayInsetTriangles=insetTotal;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            if(work)
                            {
                                if(completed==old){++work->Reused;work->ReusedBody+=completed->Geometry->Visible.size();work->ReusedInsets+=completed->Geometry->WalkwayInsets.size();}
                                else if(old && completed->Geometry==old->Geometry)
                                {
                                    ++work->Remapped;work->RemappedBody+=completed->Geometry->Visible.size();work->RemappedInsets+=completed->Geometry->WalkwayInsets.size();
                                    work->TranslatedExamples+=completed->Removed.Examples.size();
                                }
                                else {work->PreparedBody+=completed->Geometry->Visible.size();work->PreparedInsets+=completed->Geometry->WalkwayInsets.size();}
                            }
#endif
                        }
                    }
                    catch(const std::bad_alloc&){++planned.FailedSections;result->AllocationFailed=true;result->Diagnostic.Add(index,section,Failure::AllocationFailure,O::Status::AllocationFailure);}
                    catch(const std::length_error&){++planned.FailedSections;result->AllocationFailed=true;result->Diagnostic.Add(index,section,Failure::AllocationFailure,O::Status::AllocationFailure);}
                }
                if(std::any_of(planned.Sections.begin(),planned.Sections.end(),[](const auto& s){return !s->Geometry->Visible.empty();}))
                {
                    auto& count=result->DrawableRecords[(std::min)(size_t(source.Category),size_t(3))];count=O::AddSize(count,1);
                }
            }
        }
        catch(CoverageAllocationFailure){retainCommitted();}
        catch(const std::bad_alloc&){retainCommitted();}
        catch(const std::length_error&){retainCommitted();}
        return result;
    }
}
#endif
