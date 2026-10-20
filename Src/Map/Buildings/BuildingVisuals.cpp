#include "Map/Map.h"
#include "CallbackTiming.h"
#include "CaptureTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingVisuals.h"
#include "BuildingScheduler.h"
#include "BuildingAtlas.h"
#include "BuildingVisualGeometry.h"
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
    using Clock = std::chrono::steady_clock;
    constexpr auto kDebugInterval = std::chrono::seconds(5);
    struct Instance
    {
        C::Record SpriteSource;
        size_t CanonicalType=size_t(-1); // Immutable value-identity slot; no atlas state.
        C::Bounds Bounds;
        G::XY Origin;
        std::shared_ptr<const C::SplineGeometry> Curve;
        std::array<float,4> Tint={1,1,1,1};
        double HalfWidth=0;
        double Elevation=0;
        int32_t EntityIndex=0,EntitySerial=0;
        bool Spline=false, WidthFallback=false, Unresolved=false;
    };
    struct Snapshot
    {
        uint64_t Generation=0;
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
        size_t Ordinary=0,Splines=0,Culled=0,Unresolved=0,Points=0,Limited=0;
        size_t OrdinaryAtlas=0,WidthSource=0,WidthFallback=0;
        double Ms=0;
    };
    IPluginSelf* g_visualsSelf=nullptr;
    bool g_initialized=false;
    uint64_t g_generation=0;
    std::shared_ptr<const C::Snapshot> g_lastInput;
    std::mutex g_mutex;
    std::shared_ptr<const Snapshot> g_published;
    RenderStats g_render;
    std::string g_debugText;
    Clock::time_point g_nextDebug={};
    double g_prepareMax=0, g_renderMax=0;
    size_t g_prepareCount=0, g_renderCount=0;

    namespace V = MiniMapBuildingVisibilityTrace;
    std::mutex g_traceMutex;
    V::Trace g_trace;
    std::weak_ptr<const C::Snapshot> g_traceInput;
    uint64_t TraceKey(int32_t index,int32_t serial)
    { return (uint64_t(uint32_t(serial))<<32)|uint32_t(index); }
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
    void TraceFrame(const std::shared_ptr<const C::Snapshot>& current,
        const std::shared_ptr<const Snapshot>& prepared,bool admitted)
    {
        if(!current)return;
        std::lock_guard<std::mutex> lock(g_traceMutex);
        if(g_trace.Generation!=current->Generation) {g_trace.Reset(current->Generation);g_traceInput.reset();}
        if(g_trace.Events>=V::Trace::MaximumEvents)return;
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
    }
    void TraceRender(const C::Record& record,V::State state)
    {
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
        g_renderMax=0; g_renderCount=0;
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
    bool ProjectWorldQuad(const std::array<G::XY,4>& corners, const MiniMapMap::Transform& transform,
        std::array<MiniMapBuildingAtlasAlienX::Point,4>& points, C::Bounds& bounds)
    {
        if (!transform.Valid) return false;
        bounds={};
        for (size_t i=0;i<corners.size();++i)
        {
            MiniMapMap::ScreenPoint p;
            if (!transform.WorldToScreen(corners[i].X,corners[i].Y,p) || !std::isfinite(p.X) || !std::isfinite(p.Y)) return false;
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
    void OnTick(float)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::VisualsTick,true);
        if(!MiniMapMap::GameplayReady())return;
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return;
        const auto input=C::GetSnapshot();
        if(!input) { if(g_lastInput) Clear(); return; }
        if(input->Generation!=g_generation) { Clear(); g_generation=input->Generation; }
        if(input==g_lastInput) return;
        const auto start=Clock::now(),now=start;
        auto result=std::make_shared<Snapshot>(); result->Generation=input->Generation;
        result->Instances.reserve(input->Records.size());
        // Borrow input-owned strings only during preparation; equality is string value, not shared_ptr identity.
        std::unordered_map<std::string_view,size_t> canonicalTypes;
        if(!input->Records.empty())canonicalTypes.reserve(64);
        for(const auto& r:input->Records)
        {
            if(r.Generation!=input->Generation) continue;
            Instance instance; instance.Spline=r.HasSpline; instance.Origin={r.Position[0],r.Position[1]}; instance.Tint=r.Tint;
            instance.Elevation=r.Position[2];instance.EntityIndex=r.Index;instance.EntitySerial=r.Serial;
            instance.SpriteSource=r;
            if(!r.HasSpline && r.CanonicalBlueprint)
            {
                const auto [type,inserted]=canonicalTypes.try_emplace(std::string_view(*r.CanonicalBlueprint),result->CanonicalTypeCount);
                if(inserted)++result->CanonicalTypeCount;
                instance.CanonicalType=type->second;
            }
            if(r.HasSpline)
            {
                instance.Curve=r.Curve; instance.Bounds=r.Extent;
                instance.Unresolved=!r.Curve || r.Curve->Issue!=C::GeometryIssue::None || r.Curve->Segments.empty();
                const double scale=(std::max)({std::abs(r.Scale[0]),std::abs(r.Scale[1]),std::abs(r.Scale[2]),1.0});
                instance.WidthFallback=!r.DefinitionShape || !r.DefinitionShape->HasSplineMesh;
                // Separate cross-section from admission inflation: cap extension
                // is not lateral width. Unknown width retains the named 10m fallback.
                instance.HalfWidth=instance.WidthFallback?MiniMapBuildingGeometry::kSplineSafetyAllowance:
                    r.DefinitionShape->SplineMeshCrossSection*scale;
                if(!std::isfinite(instance.HalfWidth) || instance.HalfWidth<=0)
                { instance.HalfWidth=MiniMapBuildingGeometry::kSplineSafetyAllowance; instance.WidthFallback=true; }
            }
            result->Instances.push_back(std::move(instance));
        }
        std::shared_ptr<const Snapshot> previous;
        {std::scoped_lock lock(g_mutex);previous=g_published;}
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
                <<"\nDrawn ordinary atlas: "<<render.OrdinaryAtlas
                <<"\nDrawn spline width source/fallback: "<<render.WidthSource<<'/'<<render.WidthFallback
                <<"\nCulled/unresolved: "<<render.Culled<<'/'<<render.Unresolved
                <<"\nCurve points/depth-limited: "<<render.Points<<'/'<<render.Limited
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

    bool DrawSpline(IModLoaderImGui* ui,PluginDrawList dl,const MiniMapMap::Transform& transform,
        const Instance& instance,const C::Bounds& viewport,RenderStats& stats)
    {
        const float width=float((std::max)(1.0,2*instance.HalfWidth*transform.PixelsPerWorldUnit));
        const C::Bounds padded={viewport.MinX-width/2,viewport.MinY-width/2,viewport.MaxX+width/2,viewport.MaxY+width/2,true};
        bool drawn=false;
        std::array<G::XY,G::kMaximumCurvePoints> points;
        std::array<float,G::kMaximumCurvePoints*2> xy;
        for(const auto& segment:instance.Curve->Segments)
        {
            std::array<G::XY,4> controls; C::Bounds hull; bool valid=true;
            for(size_t i=0;i<4;++i)
            {
                MiniMapMap::ScreenPoint p;
                if(!transform.WorldToScreen(segment.Controls[i][0],segment.Controls[i][1],p)) { valid=false; break; }
                controls[i]={p.X,p.Y};
                if(!hull.Valid) hull={p.X,p.Y,p.X,p.Y,true};
                else { hull.MinX=(std::min)(hull.MinX,double(p.X)); hull.MinY=(std::min)(hull.MinY,double(p.Y));
                    hull.MaxX=(std::max)(hull.MaxX,double(p.X)); hull.MaxY=(std::max)(hull.MaxY,double(p.Y)); }
            }
            if(!valid || !G::Intersects(hull,padded)) continue;
            if(segment.Mode==uint8_t(SDK::EInterpCurveMode::CIM_Constant)) // A jump, never a connecting strip.
            {
                for(auto p:{controls[0],controls[3]})
                    if(p.X>=padded.MinX && p.X<=padded.MaxX && p.Y>=padded.MinY && p.Y<=padded.MaxY)
                    { ui->DL_AddCircleFilled(dl,float(p.X),float(p.Y),width/2,Color(instance.Tint,100),12); drawn=true; }
                continue;
            }
            size_t count=1; points[0]=controls[0];
            G::Subdivide(controls,points,count,stats.Limited);
            for(size_t i=0;i<count;++i) { xy[i*2]=float(points[i].X); xy[i*2+1]=float(points[i].Y); }
            ui->DL_AddPolyline(dl,xy.data(),int(count),Color(instance.Tint,instance.WidthFallback?28:80),0,width);
            ui->DL_AddPolyline(dl,xy.data(),int(count),Color(instance.Tint,150),0,1.0f);
            stats.Points+=count; drawn=true;
        }
        return drawn;
    }
}

namespace MiniMapBuildingVisuals
{
    bool Initialize(IPluginSelf* self)
    {
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return true;
        if(g_initialized) return true;
        if(!self || !self->hooks || !self->hooks->Engine) return false;
        g_visualsSelf=self; Reset(); self->hooks->Engine->RegisterOnTick(&OnTick); g_initialized=true; return true;
    }
    void Reset() { Clear(); }
    void Shutdown()
    {
        if(!g_initialized) return;
        g_visualsSelf->hooks->Engine->UnregisterOnTick(&OnTick); Reset(); g_visualsSelf=nullptr; g_initialized=false;
    }
    std::string GetDebugText()
    {
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return "\nBuildingVisuals: OFF (isolation build)\n";
        std::scoped_lock lock(g_mutex); return "\nBuildingVisuals: ON\n"+g_debugText;
    }
    void Render(IModLoaderImGui* ui,const MiniMapMap::Transform& transform)
    {
        if(!MINIMAP_STAGE_C_VISUALS_ENABLED) return;
        if(!ui || !transform.Valid) return;
        static WorldQuadCache worldQuads; // Single UI callback owner; no borrowed atlas pointers.
        std::shared_ptr<const Snapshot> snapshot; { std::scoped_lock lock(g_mutex); snapshot=g_published; }
        const auto current=C::GetSnapshot();
        if(!snapshot || !current || snapshot->Generation!=current->Generation)
        {worldQuads={};TraceFrame(current,snapshot,false);return;}
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
        // Preserve the separate spline pass and existing prepared drawing order.
        {
        // Snapshot locks are already released; keep lifecycle reset out of this pass.
        const double lockBegin=MiniMapCaptureTiming::Now();
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        const double acquired=MiniMapCaptureTiming::Now();
        uint64_t session=0,revision=0;
        MiniMapBuildingAtlas::ObservePublication(session,revision);
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
        TraceFrame(currentSession,snapshot,schedulerValid);
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
        auto first=snapshot->DrawOrder.begin();
        if constexpr(!MiniMapBuildingVisuals::SplineRenderingEnabled)
            // DrawKey sorts splines first. Skip the entire spline layer,
            // including unresolved markers, without per-segment checks.
            first=std::partition_point(first,snapshot->DrawOrder.end(),[&](size_t index)
                {return snapshot->Instances[index].Spline;});
        // Pass-local borrowed entries: destroyed before FinishDraw and LifecycleMutex unlock.
        // A positive Find proves Atlas status; discovery cannot replace/erase that status here.
        // Do not memoize misses: Current() also reads GameplayReady, which may close outside this mutex.
        std::vector<const MiniMapSpriteStatus::AtlasEntry*> atlasByType;
        try{atlasByType.resize(snapshot->CanonicalTypeCount,nullptr);}
        catch(...){} // Preserve the original per-instance path on allocation failure.
        for(auto it=first;it!=snapshot->DrawOrder.end();++it)
        {
            const auto index=*it;
            const auto& instance=snapshot->Instances[index];const bool spline=instance.Spline;
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
                    auto* worldQuad=index<worldQuads.Slots.size()?&worldQuads.Slots[index]:nullptr;
                    if (AtlasQuad(instance.SpriteSource,*entry,transform,points,spriteBounds,worldQuad))
                    {
                        // A captured assembly may extend beyond the targeting
                        // proxy, including instances whose proxy is unresolved.
                        if (!G::Intersects(spriteBounds,viewport))
                        {TraceRender(instance.SpriteSource,V::State::Culled);++stats.Culled;continue;}
                        visibility=V::State::SubmitRejected;
                        if (MiniMapBuildingAtlas::SubmitQuad(ui,dl,*entry,points,Color(instance.Tint,255)))
                        {TraceRender(instance.SpriteSource,V::State::Submitted);++stats.Ordinary;++stats.OrdinaryAtlas;continue;}
                    }
                }
            }
            // Ordinary buildings without a ready atlas sprite stay invisible.
            // Keep discovery above so the existing scheduler can load or capture them.
            if(!spline) {TraceRender(instance.SpriteSource,visibility);continue;}
            if(instance.Unresolved)
            {
                ++stats.Unresolved; MiniMapMap::ScreenPoint p;
                if(transform.WorldToScreen(instance.Origin.X,instance.Origin.Y,p) && p.X>=viewport.MinX && p.X<=viewport.MaxX && p.Y>=viewport.MinY && p.Y<=viewport.MaxY)
                {
                    ui->DL_AddCircle(dl,p.X,p.Y,3,Color(instance.Tint,170),8,1);
                }
                continue;
            }
            // B.2 bounds include spline width/caps. Pad for AA/minimum stroke.
            const C::Bounds paddedViewport={viewport.MinX-1,viewport.MinY-1,viewport.MaxX+1,viewport.MaxY+1,true};
            if(!G::Intersects(G::ScreenBounds(instance.Bounds,transform),paddedViewport))
            { ++stats.Culled; continue; }
            if(spline)
            {
                if(DrawSpline(ui,dl,transform,instance,viewport,stats))
                { ++stats.Splines; stats.WidthFallback+=instance.WidthFallback; stats.WidthSource+=!instance.WidthFallback; }
                else ++stats.Culled;
                continue;
            }
        }
        }
        ui->DL_PopClipRect(dl); stats.Ms=Ms(start);
        { std::scoped_lock lock(g_mutex); if(g_published==snapshot)
            { g_render=stats; g_renderMax=(std::max)(g_renderMax,stats.Ms); ++g_renderCount; } }
    }
}
#endif
