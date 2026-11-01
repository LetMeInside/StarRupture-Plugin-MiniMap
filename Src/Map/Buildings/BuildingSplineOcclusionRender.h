#pragma once
#include "Services/Viewport.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineOcclusionAdapter.h"
#include "BuildingVisualGeometry.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace MiniMapBuildingSplineOcclusionRender
{
    namespace A=MiniMapBuildingSplineOcclusionAdapter;
    namespace E=MiniMapBuildingSplineEnvelope;
    namespace G=MiniMapBuildingVisualGeometry;
    using Triangle=std::array<E::XY,3>; // World XY, either winding.
    // Bounds the work of one merge; each step rechecks every interior joint.
    constexpr size_t kMaximumMergedSections=64;
    inline bool FullyOccluded(const A::RecordPlan& record)
    {
        return !record.Sections.empty() && !record.FailedSections &&
            std::all_of(record.Sections.begin(),record.Sections.end(),[](const auto& section){return section->Geometry->Visible.empty();});
    }
    // One ribbon cell from start to end, split the way AppendRibbon splits it.
    inline bool Cell(const E::Ring& start,const E::Ring& end,std::array<Triangle,2>& cell)
    {
        const std::array<E::XY,4> p{start.Edges[0],end.Edges[0],end.Edges[1],start.Edges[1]};
        auto positive=[&](size_t a,size_t b,size_t c)
        {const double area=E::Area(p[a],p[b],p[c]);return std::isfinite(area) && area>0;};
        if(positive(0,1,2) && positive(0,2,3)){cell={Triangle{p[0],p[1],p[2]},Triangle{p[0],p[2],p[3]}};return true;}
        if(positive(0,1,3) && positive(1,2,3)){cell={Triangle{p[0],p[1],p[3]},Triangle{p[1],p[2],p[3]}};return true;}
        return false;
    }
    // Copy a planned body as world triangles. Runs of unclipped, adjacent
    // sections become one cell while every interior joint stays within
    // tolerance (world units) of the merged edges, so the triangle count
    // follows screen size rather than sampling density. Clipped sections are
    // copied exactly.
    inline void AppendBody(const A::RecordPlan& record,const E::Profile& profile,double tolerance,std::vector<Triangle>& out)
    {
        const auto& sections=record.Sections;
        auto copy=[&](const A::SectionPlan& section)
        {
            for(const auto& t:section.Geometry->Visible)
                out.push_back({E::XY{t.Vertices[0][0],t.Vertices[0][1]},E::XY{t.Vertices[1][0],t.Vertices[1][1]},
                    E::XY{t.Vertices[2][0],t.Vertices[2][1]}});
        };
        auto clean=[&](size_t i)
        {
            const auto& s=*sections[i];
            return s.Removed.Polygons==0 && s.Geometry->Visible.size()==2 && s.SectionIndex<profile.Sections.size();
        };
        auto ring=[&](size_t i,bool end)->const E::Ring&
        {const auto& s=profile.Sections[sections[i]->SectionIndex];return end?s.End:s.Start;};
        auto inside=[&](const E::XY& p,const E::XY& a,const E::XY& b)
        {return G::DistanceSquared({p[0],p[1]},{a[0],a[1]},{b[0],b[1]})<=tolerance*tolerance;};
        for(size_t i=0;i<sections.size();)
        {
            if(!clean(i) || !(tolerance>0)){copy(*sections[i++]);continue;}
            std::array<Triangle,2> merged{};
            size_t last=i;
            for(size_t next=i+1;next<sections.size() && next-i<kMaximumMergedSections && clean(next) &&
                sections[next]->SectionIndex==sections[next-1]->SectionIndex+1 && ring(next-1,true).Edges==ring(next,false).Edges;++next)
            {
                std::array<Triangle,2> candidate;
                if(!Cell(ring(i,false),ring(next,true),candidate))break;
                bool within=true;
                for(size_t joint=i;within && joint<next;++joint)
                    for(size_t side=0;within && side<2;++side)
                        within=inside(ring(joint,true).Edges[side],ring(i,false).Edges[side],ring(next,true).Edges[side]);
                if(!within)break;
                merged=candidate;last=next;
            }
            if(last==i)copy(*sections[i]);
            else out.insert(out.end(),merged.begin(),merged.end());
            i=last+1;
        }
    }
    inline void AppendInsets(const A::RecordPlan& record,std::vector<Triangle>& out)
    {
        for(const auto& section:record.Sections)for(const auto& t:section->Geometry->WalkwayInsets)
            out.push_back({E::XY{t.Vertices[0][0],t.Vertices[0][1]},E::XY{t.Vertices[1][0],t.Vertices[1][1]},
                E::XY{t.Vertices[2][0],t.Vertices[2][1]}});
    }
    // Reproject prepared world triangles without allocation or native reads.
    // Rendering counters count calls that returned from ImGui successfully.
    inline MiniMapBuildingVisuals::Detail::SplineSubmission Submit(MiniMap::Services::ICanvas& ui,MiniMap::Services::DrawLayer list,
        const MiniMapMap::Transform& transform,const Triangle* triangles,size_t count,
        const MiniMapBuildingCollector::Bounds& viewport,uint32_t color)
    {
        MiniMapBuildingVisuals::Detail::SplineSubmission submitted;
        if(!transform.Valid || !(color>>24) || !viewport.Valid)return submitted;
        if(!std::isfinite(viewport.MinX) || !std::isfinite(viewport.MinY) || !std::isfinite(viewport.MaxX) ||
            !std::isfinite(viewport.MaxY) || viewport.MinX>=viewport.MaxX || viewport.MinY>=viewport.MaxY)return submitted;
        for(size_t t=0;t<count;++t)
        {
            std::array<MiniMapMap::ScreenPoint,3> points;
            bool valid=true;
            for(size_t i=0;i<3;++i)
            {
                const auto& world=triangles[t][i];auto& p=points[i];
                valid&=transform.WorldToScreen(world[0],world[1],p) && std::isfinite(p.X) && std::isfinite(p.Y);
            }
            submitted.ProjectedVertices+=3;if(!valid)continue;
            auto p=points[0],q=points[1],r=points[2];
            const double area=(double(q.X)-p.X)*(double(r.Y)-p.Y)-(double(q.Y)-p.Y)*(double(r.X)-p.X);
            if(!std::isfinite(area) || area==0)continue;
            if((std::max)({p.X,q.X,r.X})<viewport.MinX || (std::min)({p.X,q.X,r.X})>viewport.MaxX ||
                (std::max)({p.Y,q.Y,r.Y})<viewport.MinY || (std::min)({p.Y,q.Y,r.Y})>viewport.MaxY)continue;
            if(area<0)std::swap(q,r);
            try{ui.FilledTriangle(list, {{{p.X, p.Y}, {q.X, q.Y}, {r.X, r.Y}}}, color);}
            catch(...){return submitted;} // Never retry a possibly recorded call.
            ++submitted.Triangles;
        }
        return submitted;
    }
}
#endif
