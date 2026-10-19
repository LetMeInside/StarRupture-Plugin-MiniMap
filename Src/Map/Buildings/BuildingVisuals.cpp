#include "Map/Map.h"
#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingVisuals.h"
#include "BuildingScheduler.h"
#include "BuildingAtlas.h"
#include "BuildingVisualGeometry.h"
#include "BuildingGeometry.h"
#include "DebugText.h"
#include "plugin_helpers.h"
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <numeric>
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
    // Generation, object index, BuildingID, FName comparison index, FName number.
    using DefinitionKey = std::array<uint64_t,5>;
    constexpr auto kRetry = std::chrono::seconds(1);
    constexpr auto kResolvedRetry = std::chrono::seconds(5);
    constexpr size_t kDefinitionsPerTick = 16;
    constexpr double kDefinitionBudgetMs = 1.0;
    constexpr double kPlaceholderHalfSize = 200; // 4m symbol, NOT a footprint/admission bound.
    enum class Source { TargetingProxy, Placeholder };
    struct Visual
    {
        C::LocalBounds Local;
        Source Kind=Source::Placeholder;
        // Bounds carry local center/aspect. Local +X is UE forward; instances
        // apply the full signed scale/quaternion. No texture/resource ownership.
    };
    struct Entry
    {
        Visual Data;
        std::shared_ptr<const C::DefinitionGeometry> Input;
        Clock::time_point RetryAt={};
        double SourceTime=0;
        bool Ready=false, Queued=true;
    };
    struct Instance
    {
        C::Record SpriteSource;
        G::Polygon Shape;
        C::Bounds Bounds;
        G::XY Origin;
        std::shared_ptr<const C::SplineGeometry> Curve;
        std::array<float,4> Tint={1,1,1,1};
        double HalfWidth=0;
        double Elevation=0;
        int32_t EntityIndex=0,EntitySerial=0;
        bool Spline=false, Proxy=false, WidthFallback=false, Unresolved=false;
    };
    struct Snapshot
    {
        uint64_t Generation=0;
        std::vector<Instance> Instances; // Same stable identity order as collector.
        std::vector<size_t> DrawOrder; // Prepared on snapshot changes, never sorted in Render.
    };
    struct RenderStats
    {
        size_t Ordinary=0,Splines=0,Culled=0,Unresolved=0,Points=0,Limited=0;
        size_t OrdinaryProxy=0,OrdinaryPlaceholder=0,OrdinaryAtlas=0,WidthSource=0,WidthFallback=0;
        double Ms=0;
    };
    IPluginSelf* g_visualsSelf=nullptr;
    bool g_initialized=false;
    uint64_t g_generation=0;
    std::map<DefinitionKey,Entry> g_definitions; // Game-thread only; copied metadata only.
    std::shared_ptr<const C::Snapshot> g_lastInput;
    std::mutex g_mutex;
    std::shared_ptr<const Snapshot> g_published;
    RenderStats g_render;
    std::string g_debugText;
    Clock::time_point g_nextDebug={};
    double g_prepareMax=0, g_renderMax=0;
    size_t g_prepareCount=0, g_renderCount=0;

    double Ms(Clock::time_point start)
    { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
    auto DrawKey(const Instance& i)
    {return std::tuple(i.Spline?0:1,i.Elevation,i.Origin.Y,i.Origin.X,i.EntitySerial,i.EntityIndex);}
    C::LocalBounds Placeholder()
    { return {{-kPlaceholderHalfSize,-kPlaceholderHalfSize,0},{kPlaceholderHalfSize,kPlaceholderHalfSize,0},true}; }
    void Clear()
    {
        g_generation=0; g_definitions.clear(); g_lastInput.reset(); g_nextDebug={};
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
    bool AtlasQuad(const C::Record& record, const MiniMapSpriteStatus::AtlasEntry& entry,
        const MiniMapMap::Transform& transform,
        std::array<MiniMapBuildingAtlasAlienX::Point,4>& points, C::Bounds& bounds)
    {
        if (!record.TransformValid || !transform.Valid) return false;
        for (double value:entry.Region) if (!std::isfinite(value) || value<0 || value>1) return false;
        if (!(entry.Region[2]>entry.Region[0] && entry.Region[3]>entry.Region[1])) return false;
        double norm=0;
        for (double q:record.Rotation) { if (!std::isfinite(q)) return false; norm+=q*q; }
        if (!std::isfinite(norm) || std::abs(norm-1)>0.001) return false;
        const double inverse=1/std::sqrt(norm);
        const double x=record.Rotation[0]*inverse,y=record.Rotation[1]*inverse,
            z=record.Rotation[2]*inverse,w=record.Rotation[3]*inverse;
        // The source is a top view, so tilt/upside-down changes visibility in a
        // way a flat sprite cannot reproduce. q and -q have identical eligibility.
        if (std::abs(x)>1e-6 || std::abs(y)>1e-6 || !(record.Scale[2]>0) ||
            record.Scale[0]==0 || record.Scale[1]==0) return false;
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
        bounds={};
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
            MiniMapMap::ScreenPoint p;
            if (!transform.WorldToScreen(world[0],world[1],p) || !std::isfinite(p.X) || !std::isfinite(p.Y)) return false;
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
        size_t built=0; double cacheMs=0;
        for(const auto& r:input->Records)
        {
            if(r.Generation!=input->Generation) continue;
            Entry* cached=nullptr;
            // Spline definitions already share copied width metadata in B.2;
            // their curves are instance-specific. Only ordinary shapes need this cache.
            if(!r.HasSpline)
            {
                const DefinitionKey key={r.Generation,uint32_t(r.DefinitionObjectIndex),r.BuildingId,uint32_t(r.DefinitionIndex),r.DefinitionNumber};
                auto& entry=g_definitions[key];
                // New copied source versions are picked up on the next eligible
                // preparation. Unresolved definitions retry; no UObject lookup here.
                const bool newer=r.RefreshedAtSeconds>=entry.SourceTime;
                entry.Queued=entry.Queued || !entry.Ready || (newer && entry.Input!=r.DefinitionShape) || now>=entry.RetryAt;
                if(entry.Queued && built<kDefinitionsPerTick && cacheMs<kDefinitionBudgetMs)
                {
                    const auto cacheStart=Clock::now();
                    if(newer || !entry.Ready) { entry.Input=r.DefinitionShape; entry.SourceTime=r.RefreshedAtSeconds; }
                    entry.Data={Placeholder(),Source::Placeholder};
                    if(entry.Input && G::Valid(entry.Input->TargetingBox))
                        entry.Data={entry.Input->TargetingBox,Source::TargetingProxy};
                    entry.Ready=true; entry.Queued=false; ++built;
                    entry.RetryAt=now+(entry.Data.Kind==Source::TargetingProxy?kResolvedRetry:kRetry);
                    cacheMs+=Ms(cacheStart);
                }
                cached=&entry;
            }
            Instance instance; instance.Spline=r.HasSpline; instance.Origin={r.Position[0],r.Position[1]}; instance.Tint=r.Tint;
            instance.Elevation=r.Position[2];instance.EntityIndex=r.Index;instance.EntitySerial=r.Serial;
            instance.SpriteSource=r;
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
            else
            {
                const auto& entry=*cached;
                // A per-entity copied proxy can differ from definition defaults.
                const auto local=G::Valid(r.LocalFootprint)?r.LocalFootprint:
                    (entry.Ready?entry.Data.Local:Placeholder());
                instance.Proxy=G::Valid(r.LocalFootprint) || (entry.Ready && entry.Data.Kind==Source::TargetingProxy);
                instance.Shape=G::Project(local,r); instance.Bounds=G::Bounds(instance.Shape);
                instance.Unresolved=instance.Shape.Count<3;
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
            size_t proxy=0,fallback=0,queued=0;
            for(const auto& [key,e]:g_definitions)
            { proxy+=e.Ready && e.Data.Kind==Source::TargetingProxy; fallback+=e.Ready && e.Data.Kind==Source::Placeholder; queued+=e.Queued; }
            RenderStats render; double renderMax; size_t renderCount;
            { std::scoped_lock lock(g_mutex); render=g_render; renderMax=g_renderMax; renderCount=g_renderCount;
                g_renderMax=0; g_renderCount=0; }
            std::ostringstream text; text<<std::fixed<<std::setprecision(2)
                <<"\n\nBuildingVisuals (atlas + geometry fallback)"
                <<"\nOrdinary definitions: "<<g_definitions.size()<<" ready: "<<proxy+fallback<<" queued: "<<queued
                <<"\nDefinition proxy/placeholder: "<<proxy<<'/'<<fallback
                <<" (fallback retries: 1s)"
                <<"\nDrawn ordinary/spline: "<<render.Ordinary<<'/'<<render.Splines
                <<"\nDrawn ordinary proxy/placeholder: "<<render.OrdinaryProxy<<'/'<<render.OrdinaryPlaceholder
                <<"\nDrawn ordinary atlas: "<<render.OrdinaryAtlas
                <<"\nDrawn spline width source/fallback: "<<render.WidthSource<<'/'<<render.WidthFallback
                <<"\nCulled/unresolved: "<<render.Culled<<'/'<<render.Unresolved
                <<"\nCurve points/depth-limited: "<<render.Points<<'/'<<render.Limited
                <<"\nLast resolve/prepare: "<<cacheMs<<'/'<<prepareMs<<" ms"
                <<"\nLast render: "<<render.Ms<<" ms"
                <<"\nInterval max prepare/render: "<<g_prepareMax<<'/'<<renderMax<<" ms";
            { std::scoped_lock lock(g_mutex); g_debugText=text.str(); }
            LOG_INFO("MiniMap: BuildingVisuals timing: prepareLast=%.3fms prepareMax=%.3fms prepares=%zu renderLast=%.3fms renderMax=%.3fms frames=%zu instances=%zu",
                prepareMs,g_prepareMax,g_prepareCount,render.Ms,renderMax,renderCount,input->Records.size());
            g_prepareMax=0; g_prepareCount=0;
            g_nextDebug=now+kResolvedRetry;
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
        std::shared_ptr<const Snapshot> snapshot; { std::scoped_lock lock(g_mutex); snapshot=g_published; }
        const auto current=C::GetSnapshot();
        if(!snapshot || !current || snapshot->Generation!=current->Generation) return;
        const auto dl=ui->GetWindowDrawList(); if(!dl) return;
        const auto start=Clock::now(); RenderStats stats;
        const C::Bounds viewport={transform.CenterX-transform.Width/2,transform.CenterY-transform.Height/2,
            transform.CenterX+transform.Width/2,transform.CenterY+transform.Height/2,true};
        ui->DL_PushClipRect(dl,float(viewport.MinX),float(viewport.MinY),float(viewport.MaxX),float(viewport.MaxY),true);
        // Preserve the separate spline pass and existing prepared drawing order.
        {
        // Snapshot locks are already released; keep lifecycle reset out of this pass.
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        const auto currentSession = C::GetSnapshot();
        const bool schedulerValid =
            MiniMapMap::GameplayReady() &&
            currentSession &&
            currentSession->Generation == snapshot->Generation;
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
        for(auto it=first;it!=snapshot->DrawOrder.end();++it)
        {
            const auto index=*it;
            const auto& instance=snapshot->Instances[index];const bool spline=instance.Spline;
            // Discover before proxy culling: the authored assembly may extend
            // beyond its proxy. Native capture is serviced on the game thread.
            if(!spline && schedulerValid)MiniMapBuildingScheduler::DrawBuilding(instance.SpriteSource);
            if(!spline && schedulerValid && ui->DL_AddImageQuad && instance.SpriteSource.CanonicalBlueprint)
            {
                if (const auto* entry=MiniMapBuildingAtlas::Find(*instance.SpriteSource.CanonicalBlueprint))
                {
                    std::array<MiniMapBuildingAtlasAlienX::Point,4> points;
                    C::Bounds spriteBounds;
                    if (AtlasQuad(instance.SpriteSource,*entry,transform,points,spriteBounds))
                    {
                        // A captured assembly may extend beyond the targeting
                        // proxy, including instances whose proxy is unresolved.
                        if (!G::Intersects(spriteBounds,viewport)) { ++stats.Culled; continue; }
                        if (MiniMapBuildingAtlas::SubmitQuad(ui,dl,*entry,points,Color(instance.Tint,255)))
                        { ++stats.Ordinary; ++stats.OrdinaryAtlas; continue; }
                    }
                }
            }
            if(instance.Unresolved)
            {
                ++stats.Unresolved; MiniMapMap::ScreenPoint p;
                if(transform.WorldToScreen(instance.Origin.X,instance.Origin.Y,p) && p.X>=viewport.MinX && p.X<=viewport.MaxX && p.Y>=viewport.MinY && p.Y<=viewport.MaxY)
                {
                    ui->DL_AddCircle(dl,p.X,p.Y,3,Color(instance.Tint,170),8,1);
                }
                continue;
            }
            // B.2 bounds include spline width/caps; ordinary bounds are the
            // projected proxy, not the 50m discovery allowance. Pad for AA/minimum stroke.
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
            std::array<float,16> xy; C::Bounds screenBounds; bool valid=true;
            for(size_t i=0;i<instance.Shape.Count;++i)
            {
                MiniMapMap::ScreenPoint p;
                if(!transform.WorldToScreen(instance.Shape.Points[i].X,instance.Shape.Points[i].Y,p)) { valid=false; break; }
                xy[i*2]=p.X; xy[i*2+1]=p.Y;
                if(!screenBounds.Valid) screenBounds={p.X,p.Y,p.X,p.Y,true};
                else { screenBounds.MinX=(std::min)(screenBounds.MinX,double(p.X)); screenBounds.MinY=(std::min)(screenBounds.MinY,double(p.Y));
                    screenBounds.MaxX=(std::max)(screenBounds.MaxX,double(p.X)); screenBounds.MaxY=(std::max)(screenBounds.MaxY,double(p.Y)); }
            }
            if(!valid || !G::Intersects(screenBounds,viewport)) { ++stats.Culled; continue; }
            if(instance.Proxy) ui->DL_AddConvexPolyFilled(dl,xy.data(),int(instance.Shape.Count),Color(instance.Tint,70));
            ui->DL_AddPolyline(dl,xy.data(),int(instance.Shape.Count),Color(instance.Tint,instance.Proxy?150:95),PluginDrawFlags_Closed,1);
            ++stats.Ordinary;
            stats.OrdinaryProxy+=instance.Proxy; stats.OrdinaryPlaceholder+=!instance.Proxy;
        }
        }
        ui->DL_PopClipRect(dl); stats.Ms=Ms(start);
        { std::scoped_lock lock(g_mutex); if(g_published==snapshot)
            { g_render=stats; g_renderMax=(std::max)(g_renderMax,stats.Ms); ++g_renderCount; } }
    }
}
#endif
