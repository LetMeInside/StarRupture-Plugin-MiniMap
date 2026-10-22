#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineOcclusion.h"
#include "BuildingVisuals.h" // Reuse the actual SelectSpline policy; never submit draws here.
#include <memory>
#include <limits>

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
        uint64_t Version=2; // Numerical policy only; no capacity policy.
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
    struct SectionPlan
    {
        size_t SectionIndex=0;
        std::vector<O::Triangle> Visible; // Complete result of exactly ONE section transaction.
    };
    struct RecordPlan
    {
        size_t RecordIndex=0,SelectedProfile=0,SourceProfile=0;
        uint8_t Category=3;
        std::vector<SectionPlan> Sections; // Failed sections have no entry; empty successful entries are valid.
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
        size_t SectionsAttempted=0,Triangles=0;
        bool AllocationFailed=false; // Never cache/reuse a resource-exhausted attempt.
        std::array<size_t,4> DrawableRecords{}; // Records with surviving geometry, never fragment counts.
    };

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
    {
        const auto& a=cached.Binding;
        // Exact immutable-owner equality, never missing native identities or
        // numerical generation alone. Include failed/missing submissions too:
        // conservative invalidation is preferable to stale occlusion.
        return !cached.AllocationFailed && request.Snapshot && a.Snapshot==request.Snapshot && cached.Generation==request.Snapshot->Generation &&
            a.AtlasSession==request.AtlasSession && a.AtlasRevision==request.AtlasRevision &&
            a.NumericalPolicy==request.NumericalPolicy && a.Splines==request.Splines && a.Ordinary==request.Ordinary;
    }
    inline std::shared_ptr<const RenderPlan> Prepare(const Request& request,std::shared_ptr<const RenderPlan> previous={})
    {
        if(previous && Compatible(*previous,request))return previous;
        auto result=std::make_shared<RenderPlan>();
        auto retainCommitted=[&]
        {
            // Shared storage exhaustion must not erase already committed
            // section transactions. Recount without allocating during failure.
            result->Triangles=0;result->DrawableRecords={};result->AllocationFailed=true;
            for(const auto& record:result->Records)
            {
                bool drawable=false;
                for(const auto& section:record.Sections)
                {result->Triangles+=section.Visible.size();drawable|=!section.Visible.empty();}
                if(drawable)++result->DrawableRecords[(std::min)(size_t(record.Category),size_t(3))];
            }
            result->Diagnostic.Add(0,0,Failure::AllocationFailure,O::Status::AllocationFailure);
        };
        // Failure to allocate the result itself propagates std::bad_alloc explicitly.
        try
        {
            // No atlas publication is necessary when no ordinary sprite was
            // submitted. An empty active set must preserve resident ribbons.
            if(!request.Snapshot || ((!request.AtlasSession || !request.AtlasRevision) &&
                std::any_of(request.Ordinary.begin(),request.Ordinary.end(),[](const auto& o){return o.Submitted;})))
            {result->Diagnostic.Add(0,0,Failure::Context);return result;}
            result->Binding=request;result->Generation=request.Snapshot->Generation;
            std::vector<O::Occluder> occluders;
            const auto& records=request.Snapshot->Records;
            for(size_t i=0;i<request.Ordinary.size();++i)
            {
                const auto& submitted=request.Ordinary[i];if(!submitted.Submitted)continue;
                O::Occluder occluder;
                if(submitted.RecordIndex>=records.size() || records[submitted.RecordIndex].Generation!=result->Generation ||
                    !MakeOccluder(records[submitted.RecordIndex],submitted,i,occluder))
                {result->Diagnostic.Add(submitted.RecordIndex,0,Failure::InvalidOccluder);continue;}
                occluders.push_back(occluder);
            }
            const auto prepared=O::PrepareOccluders(occluders);
            if(prepared.GetStatus()!=O::Status::Complete)
            {
                result->AllocationFailed=prepared.GetStatus()==O::Status::AllocationFailure;
                result->Diagnostic.Add(0,0,Failure::Planner,prepared.GetStatus());
                return result;
            }
            std::vector<size_t> visited;
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
                for(size_t section=0;section<profile->Sections.size();++section)
                {
                    result->SectionsAttempted=O::AddSize(result->SectionsAttempted,1);
                    try
                    {
                        O::Input input;
                        if(!SectionInput(*source.Envelope,*profile,section,input))
                        {++planned.FailedSections;result->Diagnostic.Add(index,section,Failure::SectionData);continue;}
                        auto plan=O::Plan(input.Triangles,prepared);
                        if(plan.State!=O::Status::Complete)
                        {++planned.FailedSections;result->AllocationFailed|=plan.State==O::Status::AllocationFailure;
                         result->Diagnostic.Add(index,section,Failure::Planner,plan.State);continue;}
                        const auto total=O::AddSize(result->Triangles,plan.Visible.size());
                        planned.Sections.push_back({section,std::move(plan.Visible)});
                        result->Triangles=total;
                        if(plan.Removed.Polygons)
                        {
                            auto& coverage=result->Coverage;
                            ++coverage.Sections;
                            const bool fully=planned.Sections.back().Visible.empty();
                            coverage.FullyRemovedSections+=fully;
                            coverage.Polygons+=plan.Removed.Polygons;coverage.RemovedArea+=plan.Removed.Area;
                            coverage.PlannerExamplesAllocationFailed|=plan.Removed.AllocationFailed;
                            for(const auto& evidence:plan.Removed.Examples)
                            {
                                // Keys are original Request::Ordinary slots, not
                                // compacted/sorted prepared-occluder positions.
                                if(evidence.OccluderKey>=request.Ordinary.size())continue;
                                if(!coverage.AllocationFailed)
                                {
                                    try{coverage.Examples.push_back({index,
                                        request.Ordinary[size_t(evidence.OccluderKey)].RecordIndex,section,source.Category,
                                        input.Triangles[0].Vertices[0][2],input.Triangles[0].Vertices[1][2],fully,evidence});}
                                    catch(const std::bad_alloc&){coverage.AllocationFailed=true;}
                                    catch(const std::length_error&){coverage.AllocationFailed=true;}
                                }
                            }
                        }
                    }
                    catch(const std::bad_alloc&){++planned.FailedSections;result->AllocationFailed=true;result->Diagnostic.Add(index,section,Failure::AllocationFailure,O::Status::AllocationFailure);}
                    catch(const std::length_error&){++planned.FailedSections;result->AllocationFailed=true;result->Diagnostic.Add(index,section,Failure::AllocationFailure,O::Status::AllocationFailure);}
                }
                if(std::any_of(planned.Sections.begin(),planned.Sections.end(),[](const SectionPlan& s){return !s.Visible.empty();}))
                {
                    auto& count=result->DrawableRecords[(std::min)(size_t(source.Category),size_t(3))];
                    count=O::AddSize(count,1);
                }
            }
        }
        catch(const std::bad_alloc&)
        {retainCommitted();}
        catch(const std::length_error&)
        {retainCommitted();}
        return result;
    }
}
#endif
