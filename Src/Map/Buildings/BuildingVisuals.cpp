#include "Map/Map.h"
#include "CallbackTiming.h"
#include "CaptureTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingVisuals.h"
#include "BuildingScheduler.h"
#include "BuildingAtlas.h"
#include "BuildingVisualGeometry.h"
#include "BuildingSplineGeometry.h"
#include "BuildingSplinePolicy.h"
#include "BuildingSplineVisualSections.h"
#include "BuildingSplineEnvelope.h"
#include "BuildingSplineDiagnostics.h"
#include "BuildingSplineOcclusionRender.h"
#include "BuildingGeometry.h"
#include "BuildingVisibilityTrace.h"
#include "DebugText.h"
#include "plugin_helpers.h"
#include <chrono>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <string_view>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#ifndef MINIMAP_STAGE_C_VISUALS_ENABLED
#define MINIMAP_STAGE_C_VISUALS_ENABLED 0
#endif

namespace
{
    namespace G = MiniMapBuildingVisualGeometry;
    namespace C = MiniMapBuildingCollector;
    namespace S = MiniMapBuildingSplineGeometry;
    namespace A = MiniMapBuildingSplineOcclusionAdapter;
    namespace OR = MiniMapBuildingSplineOcclusionRender;
    namespace P = MiniMapBuildingSplinePolicy;
    static_assert(uint8_t(SDK::EInterpCurveMode::CIM_Linear)==S::Linear &&
        uint8_t(SDK::EInterpCurveMode::CIM_Constant)==S::Constant &&
        uint8_t(SDK::EInterpCurveMode::CIM_CurveAuto)==1 &&
        uint8_t(SDK::EInterpCurveMode::CIM_CurveUser)==3 &&
        uint8_t(SDK::EInterpCurveMode::CIM_CurveBreak)==4 &&
        uint8_t(SDK::EInterpCurveMode::CIM_CurveAutoClamped)==5);
    using Clock = std::chrono::steady_clock;
    constexpr auto kDebugInterval = std::chrono::seconds(5);
    constexpr uint8_t kSplineFillOpacity=255; // Color multiplies by clamped record alpha, preserving zero.
    struct Instance
    {
        C::Record SpriteSource;
        size_t RecordIndex=0; // Index in Snapshot::Source, including filtered-out collector records.
        size_t CanonicalType=size_t(-1); // Immutable value-identity slot; no atlas state.
        C::Bounds Bounds;
        G::XY Origin;
        std::shared_ptr<const C::SplineGeometry> Curve;
        std::shared_ptr<const S::PreparedCurve> PreparedSpline;
        std::shared_ptr<const MiniMapBuildingSplineEnvelope::Result> PreparedEnvelope;
        std::shared_ptr<const MiniMapVisualAccess::Observation> NativeVisuals;
        std::shared_ptr<const MiniMapBuildingSplineVisualSections::Result> NativeVisualSections;
        MiniMapBuildingSplinePolicy::SplineCategory SplineCategory=MiniMapBuildingSplinePolicy::SplineCategory::Other;
        MiniMapBuildingSplinePolicy::VisualAcquisitionState VisualAcquisition=
            MiniMapBuildingSplinePolicy::VisualAcquisitionState::NotRequested;
        std::array<float,4> Tint={1,1,1,1};
        double Elevation=0;
        int32_t EntityIndex=0,EntitySerial=0;
        bool Spline=false, Unresolved=false;
    };
    struct Snapshot
    {
        uint64_t Generation=0;
        std::shared_ptr<const C::Snapshot> Source;
        std::vector<Instance> Instances; // Same stable identity order as collector.
        size_t CanonicalTypeCount=0;
        std::vector<size_t> DrawOrder; // Prepared on snapshot changes, never sorted in Render.
    };
    // Render's UI thread owns this storage; OnTick/Clear never touch it.
    // Only copied world XY is retained, after validating all three world axes.
    struct WorldQuad
    {
        enum class Status : uint8_t { Unknown, Valid, Invalid };
        std::array<G::XY,4> Corners;
        Status State=Status::Unknown;
    };
    struct WorldQuadCache
    {
        std::weak_ptr<const Snapshot> Owner;
        uint64_t Session=0,Revision=0;
        std::vector<WorldQuad> Slots; // One contiguous allocation, indexed by Instance, not type.
        void Bind(const std::shared_ptr<const Snapshot>& snapshot,uint64_t session,uint64_t revision)
        {
            if(Owner.lock()==snapshot && Session==session && Revision==revision)return;
            Owner.reset(); // Allocation failure cannot leave old slots associated with new input.
            try{Slots.resize(snapshot->Instances.size());}
            catch(...){Slots.clear();return;} // Recompute locally; retry allocation next callback.
            for(auto& slot:Slots)slot.State=WorldQuad::Status::Unknown;
            Session=session;Revision=revision;Owner=snapshot;
        }
    };
    struct RenderStats
    {
        size_t Ordinary=0,Splines=0,Culled=0,Unresolved=0,Points=0,Triangles=0;
        size_t SplineWalkways=0,SplineRails=0,SplineZiplines=0,SplineOther=0;
        size_t OrdinaryAtlas=0;
        size_t FullyOccluded=0,FailedSections=0,PlannerFailures=0,AllocationFailures=0,StalePlans=0;
        size_t InvalidSections=0,InvalidOccluders=0,PlanReuses=0,PlanRebuilds=0;
        std::shared_ptr<const A::RenderPlan> CoverageOwner; // Same source/atlas binding as these frame diagnostics.
        size_t OutsideViewportRecords=0,RejectedSplineRecords=0,TransparentSplineRecords=0;
        size_t OutsideViewportTriangles=0,ProjectionRejectedTriangles=0;
        double Ms=0;
    };
    void CountSplineSubmission(RenderStats& stats,MiniMapBuildingSplinePolicy::SplineCategory category)
    {
        using Category=MiniMapBuildingSplinePolicy::SplineCategory;
        switch(category)
        {
        case Category::Walkways:++stats.SplineWalkways;break;
        case Category::Rails:++stats.SplineRails;break;
        case Category::Ziplines:++stats.SplineZiplines;break;
        default:++stats.SplineOther;break;
        }
    }
    bool g_initialized=false;
    uint64_t g_generation=0;
    std::shared_ptr<const C::Snapshot> g_lastInput;
    std::mutex g_mutex;
    std::shared_ptr<const Snapshot> g_published;
    RenderStats g_render;
    size_t g_planReuses=0,g_planRebuilds=0;
    std::string g_debugText;
    Clock::time_point g_nextDebug={};
    Clock::time_point g_nextSplineDebug={};
#if MINIMAP_DEBUG_UI
    std::string g_splineDebugText;
#endif
    double g_prepareMax=0, g_renderMax=0;
    size_t g_prepareCount=0, g_renderCount=0;

    namespace V = MiniMapBuildingVisibilityTrace;
    std::mutex g_traceMutex;
    V::Trace g_trace;
    std::weak_ptr<const C::Snapshot> g_traceInput;
    uint64_t TraceKey(int32_t index,int32_t serial)
    { return (uint64_t(uint32_t(serial))<<32)|uint32_t(index); }
    // Callback-local values only. The loader's Present guard serializes all UI
    // callbacks (including the early TraceFrame path). Only TraceFrame adds
    // targets; concurrent Clear can only empty them. Never retain across Render.
    struct TraceTargets
    {
        uint64_t Generation=0;
        size_t Count=0;
        std::array<uint64_t,V::Trace::MaximumSamples> Keys{};
        bool Contains(const C::Record& record) const
        {
            return Count && record.Generation==Generation &&
                std::binary_search(Keys.begin(),Keys.begin()+Count,TraceKey(record.Index,record.Serial));
        }
    };
    // Caller holds g_traceMutex, including when the collector was withdrawn
    // after Render retained its prepared snapshot. That path can still report
    // SessionUnavailable against the existing trace generation.
    TraceTargets TraceView()
    {
        TraceTargets targets;
        if(g_trace.Events<V::Trace::MaximumEvents)
        {
            targets.Generation=g_trace.Generation;targets.Count=g_trace.Count;
            for(size_t i=0;i<targets.Count;++i)
                targets.Keys[i]=TraceKey(g_trace.Samples[i].Index,g_trace.Samples[i].Serial);
            std::sort(targets.Keys.begin(),targets.Keys.begin()+targets.Count);
        }
        return targets;
    }
    void TraceState(size_t sample,V::Stage stage,V::State state,const C::Record* record=nullptr)
    {
        if(!g_trace.Observe(sample,stage,state))return;
        const auto& id=g_trace.Samples[sample];
        const char* stages[]{"collector","prepared","render"};
        try { if(auto* self=GetSelf();self&&self->logger&&self->logger->Warn)
            self->logger->Warn(self,"MiniMap: sprite visibility: generation=%llu entity=%d:%d building=%u definition=%d stage=%s state=%s canonical=%s tintAlpha=%.3f event=%zu/128",
                static_cast<unsigned long long>(g_trace.Generation),id.Index,id.Serial,id.Building,id.Definition,
                stages[unsigned(stage)],V::Name(state),record&&record->CanonicalBlueprint?record->CanonicalBlueprint->c_str():"unavailable",
                record?double(record->Tint[3]):-1.0,g_trace.Events); }
        catch(...) {} // Diagnostics cannot alter visibility/admission.
    }
    TraceTargets TraceFrame(const std::shared_ptr<const C::Snapshot>& current,
        const std::shared_ptr<const Snapshot>& prepared,bool admitted)
    {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        if(!current)return TraceView();
        if(g_trace.Generation!=current->Generation) {g_trace.Reset(current->Generation);g_traceInput.reset();}
        if(g_trace.Events>=V::Trace::MaximumEvents)return {};
        if(g_traceInput.lock()!=current)
        {
            for(const auto& record:current->Records)
                if(!record.HasSpline)g_trace.Consider(record.Index,record.Serial,record.DefinitionObjectIndex,record.BuildingId);
            g_traceInput=current;
        }
        for(size_t i=0;i<g_trace.Count;++i)
        {
            const auto& id=g_trace.Samples[i];const auto key=TraceKey(id.Index,id.Serial);
            const auto found=std::lower_bound(current->Records.begin(),current->Records.end(),key,
                [](const C::Record& r,uint64_t value){return TraceKey(r.Index,r.Serial)<value;});
            const auto* record=found!=current->Records.end()&&TraceKey(found->Index,found->Serial)==key?&*found:nullptr;
            TraceState(i,V::Stage::Collector,!record?V::State::Absent:
                !record->CanonicalBlueprint?V::State::CanonicalUnavailable:V::State::Present,record);
            const Instance* instance=nullptr;
            if(prepared&&prepared->Generation==current->Generation)
            {
                const auto p=std::lower_bound(prepared->Instances.begin(),prepared->Instances.end(),key,
                    [](const Instance& r,uint64_t value){return TraceKey(r.EntityIndex,r.EntitySerial)<value;});
                if(p!=prepared->Instances.end()&&TraceKey(p->EntityIndex,p->EntitySerial)==key)instance=&*p;
            }
            TraceState(i,V::Stage::Prepared,!instance?V::State::Absent:
                !instance->SpriteSource.CanonicalBlueprint?V::State::CanonicalUnavailable:V::State::Present,
                instance?&instance->SpriteSource:nullptr);
            if(!instance)TraceState(i,V::Stage::Render,V::State::Absent);
            else if(!admitted)TraceState(i,V::Stage::Render,V::State::SessionUnavailable);
        }
        return TraceView();
    }
    void TraceRender(const TraceTargets& targets,const C::Record& record,V::State state)
    {
        if(!targets.Contains(record))return;
        std::lock_guard<std::mutex> lock(g_traceMutex);
        if(record.Generation!=g_trace.Generation||g_trace.Events>=V::Trace::MaximumEvents)return;
        for(size_t i=0;i<g_trace.Count;++i)
            if(record.Index==g_trace.Samples[i].Index&&record.Serial==g_trace.Samples[i].Serial)
            {TraceState(i,V::Stage::Render,state,&record);return;}
    }

    double Ms(Clock::time_point start)
    { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
    auto DrawKey(const Instance& i)
    {return std::tuple(i.Spline?0:1,i.Elevation,i.Origin.Y,i.Origin.X,i.EntitySerial,i.EntityIndex);}
    void Clear()
    {
        {std::lock_guard<std::mutex> lock(g_traceMutex);g_trace.Reset(0);g_traceInput.reset();}
        g_generation=0; g_lastInput.reset(); g_nextDebug={};
        g_prepareMax=0; g_prepareCount=0;
        std::scoped_lock lock(g_mutex); g_published.reset(); g_render={}; g_debugText.clear();
        g_nextSplineDebug={};
#if MINIMAP_DEBUG_UI
        g_splineDebugText.clear();
#endif
        g_renderMax=0; g_renderCount=0;g_planReuses=0;g_planRebuilds=0;
    }
    uint32_t Color(const std::array<float,4>& tint,uint8_t alpha)
    {
        auto channel=[](float v)->uint32_t { return uint32_t(std::lround(std::clamp(std::isfinite(v)?v:1.0f,0.0f,1.0f)*255)); };
        return channel(tint[0])|(channel(tint[1])<<8)|(channel(tint[2])<<16)|
            (uint32_t(std::lround(double(alpha)*std::clamp(std::isfinite(tint[3])?tint[3]:1.0f,0.0f,1.0f)))<<24);
    }
    bool BuildWorldQuad(const C::Record& record, const MiniMapSpriteStatus::AtlasEntry& entry,
        std::array<G::XY,4>& corners)
    {
        if (!record.TransformValid) return false;
        for (double value:entry.Region) if (!std::isfinite(value) || value<0 || value>1) return false;
        if (!(entry.Region[2]>entry.Region[0] && entry.Region[3]>entry.Region[1])) return false;
        double norm=0;
        for (double q:record.Rotation) { if (!std::isfinite(q)) return false; norm+=q*q; }
        if (!std::isfinite(norm) || !(norm>0)) return false;
        const double inverse=1/std::sqrt(norm);
        const double x=record.Rotation[0]*inverse,y=record.Rotation[1]*inverse,
            z=record.Rotation[2]*inverse,w=record.Rotation[3]*inverse;
        // Apply the copied transform, including tilt and signed scale. Only a
        // nonfinite/degenerate projected quad is unrepresentable; these are not
        // additional building-instance eligibility rules.
        std::array<double,3> center{},extent{};
        for (size_t axis=0;axis<3;++axis)
        {
            if (!std::isfinite(record.Position[axis]) || !std::isfinite(record.Scale[axis]) ||
                !std::isfinite(entry.Min[axis]) || !std::isfinite(entry.Max[axis]) ||
                entry.Max[axis]<entry.Min[axis]) return false;
            center[axis]=entry.Min[axis]*0.5+entry.Max[axis]*0.5;
            extent[axis]=entry.Max[axis]*0.5-entry.Min[axis]*0.5;
        }
        if (!(extent[0]>0 && extent[1]>0)) return false;
        // Match the float OrthoWidth actually assigned by ConfigureSceneCapture.
        const double halfWidth=double(float(2*(std::max)(extent[0],extent[1])*1.1))*0.5;
        if (!std::isfinite(halfWidth) || !(halfWidth>0)) return false;
        const auto& rect=entry.SourceRect;
        for (double value:rect) if (!std::isfinite(value)) return false;
        if (rect[0]<0 || rect[1]<0 || rect[0]>=256 || rect[1]>=256 || rect[2]<=rect[0] || rect[3]<=rect[1]) return false;
        const double left=rect[0],top=rect[1],right=rect[2],bottom=rect[3];
        const std::array<std::array<double,2>,4> source{{{left,top},{right,top},{right,bottom},{left,bottom}}};
        for (size_t i=0;i<source.size();++i)
        {
            // Capture rotation (-90,-90,0): image right=+X, image down=+Y.
            // Source pixel BOUNDARIES, not atlas UVs; keep asymmetric crop and
            // Actor-origin offset. FTransform applies signed scale before rotation.
            const std::array<double,3> v{{(center[0]+halfWidth*(2*source[i][0]/256-1))*record.Scale[0],
                (center[1]+halfWidth*(2*source[i][1]/256-1))*record.Scale[1],center[2]*record.Scale[2]}};
            for (double value:v) if (!std::isfinite(value)) return false;
            const double tx=2*(y*v[2]-z*v[1]),ty=2*(z*v[0]-x*v[2]),tz=2*(x*v[1]-y*v[0]);
            const std::array<double,3> world{{record.Position[0]+v[0]+w*tx+y*tz-z*ty,
                record.Position[1]+v[1]+w*ty+z*tx-x*tz,record.Position[2]+v[2]+w*tz+x*ty-y*tx}};
            for (double value:world) if (!std::isfinite(value)) return false;
            corners[i]={world[0],world[1]};
        }
        return true;
    }
    // Candidate A: copy the stable transform once per quad after its Valid guard.
    // Keep every dynamic numerical guard and both float rounding stages.
    struct PreparedTransform : MiniMapMap::Transform {
    using ScreenPoint = MiniMapMap::ScreenPoint;
    explicit PreparedTransform(const MiniMapMap::Transform& t):MiniMapMap::Transform(t) {}
    bool DirectionToScreen(
                double worldDirectionX,
                double worldDirectionY,
                ScreenPoint& outDirection) const
            {
                outDirection = {};

                if (
                    !std::isfinite(worldDirectionX) ||
                    !std::isfinite(worldDirectionY))
                {
                    return false;
                }

                const double screenX =
                    CosMapRotation *
                    worldDirectionX -
                    SinMapRotation *
                    worldDirectionY;

                const double screenY =
                    SinMapRotation *
                    worldDirectionX +
                    CosMapRotation *
                    worldDirectionY;

                if (!std::isfinite(screenX) ||
                    !std::isfinite(screenY))
                {
                    return false;
                }

                outDirection.X =
                    static_cast<float>(
                        screenX);

                outDirection.Y =
                    static_cast<float>(
                        screenY);

                return true;
            }
    bool WorldToScreen(
                double worldX,
                double worldY,
                ScreenPoint& outPoint) const
            {
                outPoint = {};

                if (
                    !std::isfinite(worldX) ||
                    !std::isfinite(worldY))
                {
                    return false;
                }

                const double dx =
                    worldX -
                    PlayerWorldX;

                const double dy =
                    worldY -
                    PlayerWorldY;

                ScreenPoint direction = {};

                if (!DirectionToScreen(
                    dx,
                    dy,
                    direction))
                {
                    return false;
                }

                const double screenX =
                    static_cast<double>(
                        CenterX) +
                    PixelsPerWorldUnit *
                    static_cast<double>(
                        direction.X);

                const double screenY =
                    static_cast<double>(
                        CenterY) +
                    PixelsPerWorldUnit *
                    static_cast<double>(
                        direction.Y);

                if (!std::isfinite(screenX) ||
                    !std::isfinite(screenY))
                {
                    return false;
                }

                outPoint.X =
                    static_cast<float>(
                        screenX);

                outPoint.Y =
                    static_cast<float>(
                        screenY);

                return true;
            }
    };
    bool ProjectWorldQuad(const std::array<G::XY,4>& corners, const MiniMapMap::Transform& transform,
        std::array<MiniMapBuildingAtlasAlienX::Point,4>& points, C::Bounds& bounds)
    {
        if (!transform.Valid) return false;
        PreparedTransform prepared(transform);
        bounds={};
        for (size_t i=0;i<corners.size();++i)
        {
            MiniMapMap::ScreenPoint p;
            if (!prepared.WorldToScreen(corners[i].X,corners[i].Y,p) || !std::isfinite(p.X) || !std::isfinite(p.Y)) return false;
            points[i]={p.X,p.Y};
            if (!bounds.Valid) bounds={p.X,p.Y,p.X,p.Y,true};
            else { bounds.MinX=(std::min)(bounds.MinX,double(p.X)); bounds.MinY=(std::min)(bounds.MinY,double(p.Y));
                bounds.MaxX=(std::max)(bounds.MaxX,double(p.X)); bounds.MaxY=(std::max)(bounds.MaxY,double(p.Y)); }
        }
        // Reject float-collapsed/degenerate projections, but keep mirrored winding
        // attached to the same TL/TR/BR/BL UVs (the backend does not cull faces).
        double winding=0;
        for (size_t i=0;i<points.size();++i)
        {
            const auto& a=points[i]; const auto& b=points[(i+1)%4]; const auto& c=points[(i+2)%4];
            const double cross=(double(b.X)-a.X)*(double(c.Y)-b.Y)-(double(b.Y)-a.Y)*(double(c.X)-b.X);
            if (!std::isfinite(cross) || cross==0 || (i && (cross>0)!=(winding>0))) return false;
            winding=cross;
        }
        return true;
    }
    bool AtlasQuad(const C::Record& record, const MiniMapSpriteStatus::AtlasEntry& entry,
        const MiniMapMap::Transform& transform,
        std::array<MiniMapBuildingAtlasAlienX::Point,4>& points, C::Bounds& bounds, WorldQuad* cached)
    {
        if (!transform.Valid) return false;
        if (!cached)
        {
            WorldQuad local; // Allocate fallback storage only when the slot vector is unavailable.
            return AtlasQuad(record,entry,transform,points,bounds,&local);
        }
        auto& quad=*cached;
        if (quad.State==WorldQuad::Status::Unknown)
            quad.State=BuildWorldQuad(record,entry,quad.Corners)?WorldQuad::Status::Valid:WorldQuad::Status::Invalid;
        return quad.State==WorldQuad::Status::Valid && ProjectWorldQuad(quad.Corners,transform,points,bounds);
    }
    void ReportSplineInputs()
    {
        const auto now=Clock::now();if(now<g_nextSplineDebug)return;
        std::shared_ptr<const Snapshot> snapshot;
        {std::scoped_lock lock(g_mutex);snapshot=g_published;}
        if(!snapshot)return;
        g_nextSplineDebug=now+kDebugInterval;
        try
        {
            MiniMapBuildingSplineDiagnostics::Summary summary;
            for(const auto& instance:snapshot->Instances)if(instance.Spline)
            {
                summary.Add(instance.SpriteSource,instance.PreparedSpline.get(),instance.PreparedEnvelope.get(),
                    instance.PreparedEnvelope && MiniMapBuildingVisuals::Detail::SelectSpline(instance.SpriteSource,*instance.PreparedEnvelope));
            }
            const auto text=summary.Text();
            const auto acquisition=summary.AcquisitionText();
            RenderStats rendered;size_t reuses=0,rebuilds=0;
            {std::scoped_lock lock(g_mutex);rendered=g_render;reuses=g_planReuses;rebuilds=g_planRebuilds;g_planReuses=0;g_planRebuilds=0;}
            LOG_INFO("MiniMap: spline occlusion: submittedRecords=%zu fullyOccluded=%zu failedSections=%zu invalidSections=%zu plannerFailures=%zu allocationFailures=%zu stale=%zu invalidOccluders=%zu fragmentTriangles=%zu planReuse=%zu planRebuild=%zu",
                rendered.Splines,rendered.FullyOccluded,rendered.FailedSections,rendered.InvalidSections,rendered.PlannerFailures,
                rendered.AllocationFailures,rendered.StalePlans,rendered.InvalidOccluders,rendered.Triangles,reuses,rebuilds);
            const auto* coverage=rendered.CoverageOwner?&rendered.CoverageOwner->Coverage:nullptr;
            LOG_INFO("MiniMap: spline visibility: occludedSections=%zu fullyRemovedSections=%zu removedPolygons=%zu removedWorldXYArea=%.6g coverageExamplesAllocationFailed=%u plannerExamplesAllocationFailed=%u outsideViewportRecords=%zu outsideViewportTriangles=%zu rejectedRecords=%zu projectionRejectedTriangles=%zu transparentRecords=%zu",
                coverage?coverage->Sections:0,coverage?coverage->FullyRemovedSections:0,coverage?coverage->Polygons:0,
                coverage?double(coverage->RemovedArea):0.0,unsigned(coverage && coverage->AllocationFailed),
                unsigned(coverage && coverage->PlannerExamplesAllocationFailed),rendered.OutsideViewportRecords,rendered.OutsideViewportTriangles,
                rendered.RejectedSplineRecords,rendered.ProjectionRejectedTriangles,rendered.TransparentSplineRecords);
            if(coverage && rendered.CoverageOwner->Binding.Snapshot)
            {
                const auto& owner=*rendered.CoverageOwner;
                const auto& records=owner.Binding.Snapshot->Records;
                for(const auto& record:owner.Records)
                {
                    if(!OR::FullyOccluded(record) || record.RecordIndex>=records.size())continue;
                    const auto& spline=records[record.RecordIndex];
                    LOG_INFO("MiniMap: spline fully occluded: generation=%llu collectorRecord=%zu entity=%d:%d category=%u",
                        static_cast<unsigned long long>(owner.Generation),record.RecordIndex,spline.Index,spline.Serial,unsigned(record.Category));
                }
            }
            if(rendered.CoverageOwner)
            {
                const auto& diagnostic=rendered.CoverageOwner->Diagnostic;
                LOG_INFO("MiniMap: spline occlusion diagnostic: failureExamples=%zu failureExamplesAllocationFailed=%u",
                    diagnostic.Examples.size(),unsigned(diagnostic.AllocationFailed));
                for(const auto& example:diagnostic.Examples)
                    LOG_INFO("MiniMap: spline occlusion example: record=%zu section=%zu cause=%u planner=%u",
                        example.Record,example.Section,unsigned(example.Cause),unsigned(example.Planner));
            }
            LOG_INFO("MiniMap: spline acquisition aggregate: %s",acquisition.c_str());
            for(size_t i=0;i<summary.LookupExampleCount;++i)
                LOG_INFO("MiniMap: spline resident lookup example: %s",summary.LookupExamples[i].c_str());
#if MINIMAP_DEBUG_UI
            {std::scoped_lock lock(g_mutex);g_splineDebugText=text;}
#endif
            if(summary.Records!=summary.Counts[size_t(MiniMapBuildingSplineDiagnostics::Cause::Drawable)])
            {
                LOG_ERROR("MiniMap: %s",text.c_str());
            }
#if MINIMAP_DEBUG_UI
            else {LOG_INFO("MiniMap: %s",text.c_str());}
#endif
            for(size_t i=0;i<summary.ExampleCount;++i)
            {
                LOG_ERROR("MiniMap: spline envelope example: %s",summary.Examples[i].c_str());
            }
        }
        catch(...){} // Diagnostics must not interrupt preparation or publication.
    }
    void OnTick(float)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::VisualsTick,true);
        if(!MiniMapMap::GameplayReady())return;
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return;
        ReportSplineInputs(); // Also runs when the immutable collector snapshot is unchanged.
        const auto input=C::GetSnapshot();
        if(!input) { if(g_lastInput) Clear(); return; }
        if(input->Generation!=g_generation) { Clear(); g_generation=input->Generation; }
        if(input==g_lastInput) return;
        std::shared_ptr<const Snapshot> previous;
        {std::scoped_lock lock(g_mutex);previous=g_published;}
        // Snapshot-local reuse only; no collector, native acquisition or refresh
        // policy is owned by the geometry preparer.
        std::unordered_map<uint64_t,const Instance*> previousSplines;
        if(previous && previous->Generation==input->Generation)
            for(const auto& instance:previous->Instances)
                if(instance.Spline)previousSplines.emplace(TraceKey(instance.EntityIndex,instance.EntitySerial),&instance);
        const auto start=Clock::now(),now=start;
        auto result=std::make_shared<Snapshot>(); result->Generation=input->Generation;result->Source=input;
        result->Instances.reserve(input->Records.size());
        // Borrow input-owned strings only during preparation; equality is string value, not shared_ptr identity.
        std::unordered_map<std::string_view,size_t> canonicalTypes;
        if(!input->Records.empty())canonicalTypes.reserve(64);
        // Prepare native visuals for every current-generation spline in this collector snapshot.
        for(const auto& r:input->Records)
        {
            if(r.Generation!=input->Generation) continue;
            Instance instance; instance.Spline=r.HasSpline; instance.Origin={r.Position[0],r.Position[1]}; instance.Tint=r.Tint;
            instance.Elevation=r.Position[2];instance.EntityIndex=r.Index;instance.EntitySerial=r.Serial;
            instance.SpriteSource=r;
            instance.RecordIndex=size_t(&r-input->Records.data());
            if(!r.HasSpline && r.CanonicalBlueprint)
            {
                const auto [type,inserted]=canonicalTypes.try_emplace(std::string_view(*r.CanonicalBlueprint),result->CanonicalTypeCount);
                if(inserted)++result->CanonicalTypeCount;
                instance.CanonicalType=type->second;
            }
            if(r.HasSpline)
            {
                instance.SplineCategory=MiniMapBuildingSplinePolicy::Classify(r.DefinitionValid,r.BuildingId);
                instance.VisualAcquisition=MiniMapBuildingSplinePolicy::VisualAcquisitionState::NativeAccessPending;
                instance.NativeVisuals=std::make_shared<const MiniMapVisualAccess::Observation>(C::ObserveNativeVisuals(r));
                instance.NativeVisualSections=std::make_shared<const MiniMapBuildingSplineVisualSections::Result>(
                    MiniMapBuildingSplineVisualSections::Prepare(*instance.NativeVisuals));
                instance.VisualAcquisition=instance.NativeVisuals->State==MiniMapVisualAccess::Status::Ready?
                    MiniMapBuildingSplinePolicy::VisualAcquisitionState::NativeAccessAvailable:
                    MiniMapBuildingSplinePolicy::VisualAcquisitionState::NativeAccessIncomplete;
                instance.Curve=r.Curve; instance.Bounds=r.Extent;
                instance.Unresolved=!r.Curve || r.Curve->Issue!=C::GeometryIssue::None || r.Curve->Segments.empty();
                if(r.Curve)
                {
                    S::SamplingPolicy policy;
                    // Rotation-invariant XY error at the map's finest supported
                    // density. Z is independently bounded to 1 copied unit.
                    policy.PixelsPerUnit=1/(100*MiniMapMap::MinimumMetersPerPixel);
                    const auto found=previousSplines.find(TraceKey(r.Index,r.Serial));
                    const auto* old=found==previousSplines.end()?nullptr:found->second;
                    instance.PreparedSpline=old && old->Curve==r.Curve?old->PreparedSpline:
                        std::make_shared<const S::PreparedCurve>(S::Prepare(*r.Curve,policy));
                    instance.Unresolved=instance.Unresolved || !instance.PreparedSpline->Complete();
                }
                const auto found=previousSplines.find(TraceKey(r.Index,r.Serial));
                const auto* old=found==previousSplines.end()?nullptr:found->second;
                instance.PreparedEnvelope=MiniMapBuildingSplineEnvelope::PrepareOwned(r,instance.PreparedSpline,
                    old?&old->SpriteSource:nullptr,old?old->PreparedEnvelope:nullptr);
                // Surface submission consumes only PreparedEnvelope. Admission
                // bounds and native observations do not establish visual widths.
            }
            result->Instances.push_back(std::move(instance));
        }
        const bool sameOrderInputs=previous&&previous->Generation==result->Generation&&
            previous->Instances.size()==result->Instances.size()&&
            std::equal(result->Instances.begin(),result->Instances.end(),previous->Instances.begin(),
                [](const Instance& a,const Instance& b){return DrawKey(a)==DrawKey(b);});
        if(sameOrderInputs)result->DrawOrder=previous->DrawOrder;
        else
        {
            result->DrawOrder.resize(result->Instances.size());
            std::iota(result->DrawOrder.begin(),result->DrawOrder.end(),size_t(0));
            std::sort(result->DrawOrder.begin(),result->DrawOrder.end(),[&](size_t a,size_t b)
                {return DrawKey(result->Instances[a])<DrawKey(result->Instances[b]);});
        }
        const double prepareMs=Ms(start);
        { std::scoped_lock lock(g_mutex); g_published=std::move(result); }
        g_lastInput=input;
#if MINIMAP_DEBUG_UI
        g_prepareMax=(std::max)(g_prepareMax,Ms(start)); ++g_prepareCount;
        if(now>=g_nextDebug)
        {
            RenderStats render; double renderMax; size_t renderCount;
            { std::scoped_lock lock(g_mutex); render=g_render; renderMax=g_renderMax; renderCount=g_renderCount;
                g_renderMax=0; g_renderCount=0; }
            std::ostringstream text; text<<std::fixed<<std::setprecision(2)
                <<"\n\nBuildingVisuals (atlas sprites)"
                <<"\nDrawn ordinary/spline: "<<render.Ordinary<<'/'<<render.Splines
                <<"\nSpline submissions walkway/rail/zipline/other: "<<render.SplineWalkways<<'/'
                    <<render.SplineRails<<'/'<<render.SplineZiplines<<'/'<<render.SplineOther
                <<"\nDrawn ordinary atlas: "<<render.OrdinaryAtlas
                <<"\nCulled/unresolved: "<<render.Culled<<'/'<<render.Unresolved
                <<"\nProjected spline vertices/triangles: "<<render.Points<<'/'<<render.Triangles
                <<"\nLast prepare: "<<prepareMs<<" ms"
                <<"\nLast render: "<<render.Ms<<" ms"
                <<"\nInterval max prepare/render: "<<g_prepareMax<<'/'<<renderMax<<" ms";
            { std::scoped_lock lock(g_mutex); g_debugText=text.str(); }
            LOG_INFO("MiniMap: BuildingVisuals timing: prepareLast=%.3fms prepareMax=%.3fms prepares=%zu renderLast=%.3fms renderMax=%.3fms frames=%zu instances=%zu",
                prepareMs,g_prepareMax,g_prepareCount,render.Ms,renderMax,renderCount,input->Records.size());
            g_prepareMax=0; g_prepareCount=0;
            g_nextDebug=now+kDebugInterval;
        }
#endif
    }

}

namespace MiniMapBuildingVisuals
{
    bool Initialize(IPluginSelf* self)
    {
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return true;
        if(g_initialized) return true;
        if(!self || !self->hooks || !self->hooks->Engine) return false;
        Reset(); self->hooks->Engine->RegisterOnTick(&OnTick); g_initialized=true; return true;
    }
    void Reset() { Clear(); }
    void Shutdown()
    {
        if(!g_initialized) return;
        auto* self=GetSelf();
        self->hooks->Engine->UnregisterOnTick(&OnTick);
        Reset(); g_initialized=false;
    }
    std::string GetDebugText()
    {
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return "\nBuildingVisuals: OFF (isolation build)\n";
        std::scoped_lock lock(g_mutex);
#if MINIMAP_DEBUG_UI
        return "\nBuildingVisuals: ON\n"+g_debugText+"\n"+g_splineDebugText;
#else
        return "\nBuildingVisuals: ON\n"+g_debugText;
#endif
    }
    void Render(IModLoaderImGui* ui,const MiniMapMap::Transform& transform)
    {
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return;
        if(!ui || !transform.Valid) return;
        static WorldQuadCache worldQuads; // Single UI callback owner; no borrowed atlas pointers.
        static std::shared_ptr<const A::RenderPlan> occlusionPlan; // Same UI-thread lifetime as quad cache.
        static thread_local std::vector<OR::BoundaryEdge> droneRailOutlineScratch;
        static thread_local OR::WalkwayInsetScratch walkwayInsetScratch;
        std::shared_ptr<const Snapshot> snapshot; { std::scoped_lock lock(g_mutex); snapshot=g_published; }
        const auto current=C::GetSnapshot();
        if(!snapshot || !current || snapshot->Generation!=current->Generation)
        {worldQuads={};occlusionPlan.reset();TraceFrame(current,snapshot,false);return;}
        const auto dl=ui->GetWindowDrawList(); if(!dl) return;
        const auto start=Clock::now(); RenderStats stats;
        MiniMapCaptureTiming::Record transition;
        const double renderBegin=MiniMapCaptureTiming::Now();
        struct FinishTransition
        {
            MiniMapCaptureTiming::Record& Data;
            ~FinishTransition()
            {
                if(!Data.Published)return;
                Data.End=MiniMapCaptureTiming::Now();
                MiniMapCaptureTiming::Add(Data.Samples[size_t(MiniMapCaptureTiming::Stage::Service)],Data.Begin,Data.End,Data.Thread);
                static uint64_t session=0;static unsigned outliers=0;
                if(session!=Data.Session){session=Data.Session;outliers=0;}
                if(Data.End-Data.Begin>16 && outliers<8)
                {
                    ++outliers;
                    MiniMapCaptureTiming::Defer(Data,"atlas-render-outlier"); // After lifecycle lock destruction.
                }
            }
        } finishTransition{transition};
        const C::Bounds viewport={transform.CenterX-transform.Width/2,transform.CenterY-transform.Height/2,
            transform.CenterX+transform.Width/2,transform.CenterY+transform.Height/2,true};
        ui->DL_PushClipRect(dl,float(viewport.MinX),float(viewport.MinY),float(viewport.MaxX),float(viewport.MaxY),true);
        // Ordinary occluders, clipped splines, then Base Core overlays share
        // the same draw-list clip, scheduler pass and lifecycle admission.
        {
        // Snapshot locks are already released; keep lifecycle reset out of this pass.
        const double lockBegin=MiniMapCaptureTiming::Now();
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        const double acquired=MiniMapCaptureTiming::Now();
        uint64_t session=0,revision=0;
        MiniMapBuildingAtlas::ObservePublication(session,revision);
        if(occlusionPlan && (occlusionPlan->Binding.Snapshot!=snapshot->Source ||
            occlusionPlan->Generation!=snapshot->Generation || occlusionPlan->Binding.AtlasSession!=session ||
            occlusionPlan->Binding.AtlasRevision!=revision))occlusionPlan.reset();
        static uint64_t lastSession=0,lastRevision=0;
        static unsigned transitions=0; // UI callback owns these diagnostics; session serial never repeats.
        if(session!=lastSession){lastSession=session;lastRevision=0;transitions=0;}
        if(revision && revision!=lastRevision)
        {
            lastRevision=revision;
            if(transitions++<64)
            {
                transition.Session=session;transition.Published=revision;transition.Id=revision;
                transition.Thread=MiniMapCaptureTiming::Thread();transition.Begin=renderBegin;transition.Ok=true;
                if(ui->GetFrameCount)transition.Frame=uint64_t(ui->GetFrameCount());
                MiniMapCaptureTiming::Add(transition.Samples[size_t(MiniMapCaptureTiming::Stage::CommitWait)],lockBegin,acquired,transition.Thread);
            }
        }
        const auto currentSession = C::GetSnapshot();
        const bool schedulerValid =
            MiniMapMap::GameplayReady() &&
            currentSession &&
            currentSession->Generation == snapshot->Generation;
        const auto traceTargets=TraceFrame(currentSession,snapshot,schedulerValid);
        // The locked publication fixes Region/SourceRect/Min/Max for every resolved entry.
        // Snapshot identity fixes copied transforms, membership and instance indices.
        if(schedulerValid)worldQuads.Bind(snapshot,session,revision);
        else worldQuads={};
        struct FinishDraw
        {
            bool Admitted;
            ~FinishDraw() { if (Admitted) MiniMapBuildingScheduler::EndDraw(); }
        } finishDraw{schedulerValid}; // Ends before the outer lifecycle lock, including unwinding.
        if (schedulerValid)
            MiniMapBuildingScheduler::StartDraw();
        A::Request request;request.Snapshot=snapshot->Source;request.AtlasSession=session;request.AtlasRevision=revision;
        bool observationsComplete=true;
        if constexpr(!MiniMapBuildingVisuals::SplineRenderingEnabled)occlusionPlan.reset();
        // Pass-local borrowed entries: destroyed before FinishDraw and LifecycleMutex unlock.
        // A positive Find proves Atlas status; discovery cannot replace/erase that status here.
        // Do not memoize misses: Current() also reads GameplayReady, which may close outside this mutex.
        std::vector<const MiniMapSpriteStatus::AtlasEntry*> atlasByType;
        try{atlasByType.resize(snapshot->CanonicalTypeCount,nullptr);}
        catch(...){} // Preserve the original per-instance path on allocation failure.
        const auto baseCore=[](const Instance& instance)
        {
            const auto& path=instance.SpriteSource.CanonicalBlueprint;
            return !instance.Spline && path &&
                *path=="/Game/Chimera/Buildings/BaseCore/BP_BaseCore.BP_BaseCore_C";
        };
        auto submitOrdinary=[&](size_t index,bool collectOccluder)
        {
            const auto& instance=snapshot->Instances[index];const bool spline=instance.Spline;
            if(spline)return;
            auto* resolution=instance.CanonicalType<atlasByType.size()?&atlasByType[instance.CanonicalType]:nullptr;
            // Discover before proxy culling: the authored assembly may extend
            // beyond its proxy. Native capture is serviced on the game thread.
            if(!spline && schedulerValid)
            {
                // Only an already-positive atlas resolution proves this call is an effect-free known-type return.
                // Unknown/missing types still call on EVERY occurrence, preserving allocation-failure retries.
                if(resolution && *resolution)
                {}
                else
                {
                    MiniMapBuildingScheduler::DrawBuilding(instance.SpriteSource);
                }
            }
            auto visibility=instance.SpriteSource.CanonicalBlueprint?V::State::AtlasMissing:V::State::CanonicalUnavailable;
            if(!ui->DL_AddImageQuad)visibility=V::State::SubmitRejected;
            if(!schedulerValid)visibility=V::State::SessionUnavailable;
            if(!spline && schedulerValid && ui->DL_AddImageQuad && instance.SpriteSource.CanonicalBlueprint)
            {
                const MiniMapSpriteStatus::AtlasEntry* entry;
                if(resolution && *resolution)
                {
                    // Match Find's dynamic admission predicate even while the atlas publication is locked.
                    entry=MiniMapMap::GameplayReady()?*resolution:nullptr;
                }
                else
                {
                    entry=MiniMapBuildingAtlas::Find(*instance.SpriteSource.CanonicalBlueprint);
                    if(resolution && entry)*resolution=entry;
                }
                if (entry)
                {
                    visibility=V::State::QuadRejected;
                    std::array<MiniMapBuildingAtlasAlienX::Point,4> points;
                    C::Bounds spriteBounds;
                    WorldQuad fallbackQuad;
                    auto* worldQuad=index<worldQuads.Slots.size()?&worldQuads.Slots[index]:&fallbackQuad;
                    if (AtlasQuad(instance.SpriteSource,*entry,transform,points,spriteBounds,worldQuad))
                    {
                        // A captured assembly may extend beyond the targeting
                        // proxy, including instances whose proxy is unresolved.
                        if (!G::Intersects(spriteBounds,viewport))
                        {TraceRender(traceTargets,instance.SpriteSource,V::State::Culled);++stats.Culled;return;}
                        visibility=V::State::SubmitRejected;
                        if (MiniMapBuildingAtlas::SubmitQuad(ui,dl,*entry,points,Color(instance.Tint,255)))
                        {
                            TraceRender(traceTargets,instance.SpriteSource,V::State::Submitted);++stats.Ordinary;++stats.OrdinaryAtlas;
                            if constexpr(MiniMapBuildingVisuals::SplineRenderingEnabled)
                            {
                                if(collectOccluder && observationsComplete)
                                {
                                    try
                                    {
                                        A::OrdinarySubmission observed;observed.RecordIndex=instance.RecordIndex;
                                        observed.Min=entry->Min;observed.Max=entry->Max;observed.Submitted=true;
                                        for(size_t i=0;i<4;++i)observed.Quad[i]={worldQuad->Corners[i].X,worldQuad->Corners[i].Y};
                                        request.Ordinary.push_back(observed);
                                    }
                                    catch(const std::bad_alloc&){observationsComplete=false;++stats.AllocationFailures;}
                                    catch(const std::length_error&){observationsComplete=false;++stats.AllocationFailures;}
                                }
                            }
                            return;
                        }
                    }
                }
            }
            // Ordinary buildings without a ready atlas sprite stay invisible.
            // Keep discovery above so the existing scheduler can load or capture them.
            TraceRender(traceTargets,instance.SpriteSource,visibility);
        };
        // Rescan immutable DrawOrder rather than allocate a deferred list. Each
        // group retains its original relative order, including missing sprites.
        for(size_t index:snapshot->DrawOrder)
            if(!snapshot->Instances[index].Spline && !baseCore(snapshot->Instances[index]))submitOrdinary(index,true);
        if constexpr(MiniMapBuildingVisuals::SplineRenderingEnabled)
        {
            // Missing observations cannot be treated as an empty active set.
            // In that allocation-failure case retain ordinary draws, omit ribbons.
            auto currentPlanContext=[&]
            {
                uint64_t observedSession=0,observedRevision=0;
                MiniMapBuildingAtlas::ObservePublication(observedSession,observedRevision);
                const auto collector=C::GetSnapshot();
                std::scoped_lock ownerLock(g_mutex);
                return schedulerValid && MiniMapMap::GameplayReady() && g_published==snapshot && request.Snapshot &&
                    collector==request.Snapshot && collector->Generation==snapshot->Generation &&
                    observedSession==session && observedRevision==revision;
            };
            if(!observationsComplete || !currentPlanContext())
            {occlusionPlan.reset();if(observationsComplete)++stats.StalePlans;}
            else try
            {
                for(size_t index:snapshot->DrawOrder)
                {
                    const auto& instance=snapshot->Instances[index];if(!instance.Spline)continue;
                    if(!instance.PreparedEnvelope || !Detail::Drawable(instance.PreparedEnvelope->Status))
                    {++stats.Unresolved;continue;}
                    request.Splines.push_back({instance.RecordIndex,instance.PreparedEnvelope,uint8_t(instance.SplineCategory)});
                }
                const bool reuse=occlusionPlan && A::Compatible(*occlusionPlan,request);
                auto planned=A::Prepare(request,reuse?occlusionPlan:nullptr);
                if(reuse)++stats.PlanReuses;else ++stats.PlanRebuilds;
                if(!currentPlanContext()) {occlusionPlan.reset();++stats.StalePlans;}
                else
                {
                    occlusionPlan=planned;stats.CoverageOwner=planned;
                    stats.InvalidSections=planned->Diagnostic.Counts[size_t(A::Failure::SectionData)]+planned->Diagnostic.PlannerInvalidInputs;
                    stats.InvalidOccluders=planned->Diagnostic.Counts[size_t(A::Failure::InvalidOccluder)];
                    stats.PlannerFailures=planned->Diagnostic.Counts[size_t(A::Failure::Planner)]-
                        planned->Diagnostic.PlannerAllocationFailures-planned->Diagnostic.PlannerInvalidInputs;
                    stats.AllocationFailures+=planned->Diagnostic.Counts[size_t(A::Failure::AllocationFailure)]+planned->Diagnostic.PlannerAllocationFailures;
                    stats.Unresolved+=planned->Diagnostic.Counts[size_t(A::Failure::Selection)];
                    if(planned->AllocationFailed && !stats.AllocationFailures)++stats.AllocationFailures;
                    auto splineSource=request.Splines.begin();
                    for(const auto& record:planned->Records)
                    {
                        const auto& source=request.Snapshot->Records[record.RecordIndex];
                        const bool droneRail=P::IsDroneRailDefinition(source.DefinitionValid,source.BuildingId);
                        const bool walkway=P::IsWalkwayDefinition(source.DefinitionValid,source.BuildingId);
                        const float extraWidthPixels=droneRail?OR::DroneRailExtraWidthPixels(transform.MetersPerPixel):0.0f;
                        const A::SplineSource* sourceEnvelope=nullptr;
                        if(P::kDroneRailOutlineEnabled || extraWidthPixels>0.0f || walkway)
                        {
                            while(splineSource!=request.Splines.end() && splineSource->RecordIndex!=record.RecordIndex)++splineSource;
                            if(splineSource!=request.Splines.end())sourceEnvelope=&*splineSource++;
                        }
                        const MiniMapBuildingSplineEnvelope::Profile* renderProfile=nullptr;
                        if(sourceEnvelope && sourceEnvelope->Envelope && record.SourceProfile<sourceEnvelope->Envelope->Profiles.size())
                            renderProfile=&sourceEnvelope->Envelope->Profiles[record.SourceProfile];
                        stats.FailedSections+=record.FailedSections;
                        if(OR::FullyOccluded(record)){++stats.FullyOccluded;continue;}
                        OR::SubmissionDiagnostics visibility;
                        const auto baseRgb=P::RepresentativeRgb(source.DefinitionValid,source.BuildingId);
                        if(P::kDroneRailOutlineEnabled && P::IsDroneRailDefinition(source.DefinitionValid,source.BuildingId) && sourceEnvelope &&
                            renderProfile)
                            OR::SubmitDroneRailOutline(ui,dl,transform,record,
                                *renderProfile,*sourceEnvelope->Envelope,
                                P::ApplyTint(P::kDroneRailOutlineRgb,source.Tint,kSplineFillOpacity),
                                P::kDroneRailOutlineThicknessPixels,droneRailOutlineScratch);
                        const auto submitted=OR::Submit(ui,dl,transform,record,viewport,
                            P::ApplyTint(baseRgb,source.Tint,kSplineFillOpacity),&visibility,renderProfile,extraWidthPixels);
                        stats.OutsideViewportTriangles+=visibility.OutsideViewportTriangles;
                        stats.ProjectionRejectedTriangles+=visibility.ProjectionRejectedTriangles;
                        stats.RejectedSplineRecords+=visibility.Rejected || visibility.ProjectionRejectedTriangles!=0;
                        stats.TransparentSplineRecords+=visibility.Transparent;
                        if(!submitted.Triangles && visibility.OutsideViewportTriangles && !visibility.Rejected &&
                            !visibility.ProjectionRejectedTriangles && !visibility.Transparent)++stats.OutsideViewportRecords;
                        stats.Points+=submitted.ProjectedVertices;stats.Triangles+=submitted.Triangles;
                        if(walkway && sourceEnvelope && sourceEnvelope->Envelope && renderProfile)
                        {
                            const auto inset=OR::SubmitWalkwayInsets(ui,dl,transform,record,*renderProfile,
                                viewport,P::ApplyTint(P::kWalkwayInsetRgb,source.Tint,kSplineFillOpacity),
                                P::kWalkwayInsetFractionPerSide,walkwayInsetScratch);
                            stats.Points+=inset.ProjectedVertices;stats.Triangles+=inset.Triangles;
                        }
                        if(submitted.Triangles)
                        {++stats.Splines;CountSplineSubmission(stats,MiniMapBuildingSplinePolicy::SplineCategory(record.Category));}
                        else if(!record.FailedSections)++stats.Culled;
                    }
                }
            }
            catch(const std::bad_alloc&){occlusionPlan.reset();++stats.AllocationFailures;}
            catch(const std::length_error&){occlusionPlan.reset();++stats.AllocationFailures;}
        }
        // The captured canopy alpha composes over rails. This exact authored
        // type is deliberately absent from Request::Ordinary, even on success.
        // Keep this pass when spline rendering is disabled or planning fails.
        for(size_t index:snapshot->DrawOrder)
            if(baseCore(snapshot->Instances[index]))submitOrdinary(index,false);
        }
        ui->DL_PopClipRect(dl); stats.Ms=Ms(start);
        { std::scoped_lock lock(g_mutex); if(g_published==snapshot)
            { g_render=stats;g_planReuses+=stats.PlanReuses;g_planRebuilds+=stats.PlanRebuilds;
              g_renderMax=(std::max)(g_renderMax,stats.Ms); ++g_renderCount; } }
    }
}
#endif
