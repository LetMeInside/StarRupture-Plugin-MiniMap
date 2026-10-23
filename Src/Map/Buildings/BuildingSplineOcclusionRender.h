#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineOcclusionAdapter.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace MiniMapBuildingSplineOcclusionRender
{
    namespace A=MiniMapBuildingSplineOcclusionAdapter;
    struct SubmissionDiagnostics
    {
        size_t OutsideViewportTriangles=0,ProjectionRejectedTriangles=0;
        bool Rejected=false,Transparent=false;
    };
    inline bool FullyOccluded(const A::RecordPlan& record)
    {
        return !record.Sections.empty() && !record.FailedSections &&
            std::all_of(record.Sections.begin(),record.Sections.end(),[](const auto& section){return section.Visible.empty();});
    }
    struct WalkwayInsetScratch
    {
        A::O::Detail::Polygon Subject,Inside,Outside;
        bool Prepare() noexcept
        {
            try
            {
                Subject.reserve(8);Inside.reserve(8);Outside.reserve(8);
                Subject.clear();Inside.clear();Outside.clear();return true;
            }
            catch(...){Clear();return false;}
        }
        void Clear() noexcept {Subject.clear();Inside.clear();Outside.clear();}
    };
    inline bool WalkwayInsetBands(const MiniMapBuildingSplineEnvelope::Section& section,double fraction,
        std::array<A::O::XY,4>& left,std::array<A::O::XY,4>& right)
    {
        if(!std::isfinite(fraction) || !(fraction>0.0) || !(fraction<0.5))return false;
        const auto inset=[&](const A::O::XY& outer,const A::O::XY& opposite)
        {return A::O::XY{outer[0]+(opposite[0]-outer[0])*fraction,outer[1]+(opposite[1]-outer[1])*fraction};};
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
                const auto turn=A::O::Detail::Turn((*band)[i],(*band)[(i+1)%4],(*band)[(i+2)%4]);
                if(!std::isfinite(turn) || !(turn>0.0L))return false;
            }
        }
        return true;
    }
    inline MiniMapBuildingVisuals::Detail::SplineSubmission ClipAndSubmitWalkwayInsetTriangle(
        IModLoaderImGui* ui,PluginDrawList list,const MiniMapMap::Transform& transform,
        const A::O::Triangle& source,const std::array<A::O::XY,4>& band,
        const MiniMapBuildingCollector::Bounds& viewport,uint32_t color,WalkwayInsetScratch& scratch)
    {
        namespace O=A::O;
        MiniMapBuildingVisuals::Detail::SplineSubmission submitted;
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
                if(!O::Detail::HasArea(scratch.Subject)){scratch.Clear();return submitted;}
            }
            for(size_t i=1;i+1<scratch.Subject.size();++i)
            {
                const auto& a=scratch.Subject[0].P;const auto& b=scratch.Subject[i].P;const auto& c=scratch.Subject[i+1].P;
                const auto worldArea=O::Detail::Area(a,b,c);
                if(!std::isfinite(worldArea) || !(worldArea>0.0L))continue;
                std::array<MiniMapMap::ScreenPoint,3> p{};
                bool valid=true;
                const std::array<const O::Point*,3> world{&a,&b,&c};
                for(size_t vertex=0;vertex<3;++vertex)
                    valid&=transform.WorldToScreen((*world[vertex])[0],(*world[vertex])[1],p[vertex]) &&
                        std::isfinite(p[vertex].X) && std::isfinite(p[vertex].Y);
                submitted.ProjectedVertices+=3;if(!valid)continue;
                const double area=(double(p[1].X)-p[0].X)*(double(p[2].Y)-p[0].Y)-
                    (double(p[1].Y)-p[0].Y)*(double(p[2].X)-p[0].X);
                if(!std::isfinite(area) || area==0.0)continue;
                if((std::max)({p[0].X,p[1].X,p[2].X})<viewport.MinX ||
                    (std::min)({p[0].X,p[1].X,p[2].X})>viewport.MaxX ||
                    (std::max)({p[0].Y,p[1].Y,p[2].Y})<viewport.MinY ||
                    (std::min)({p[0].Y,p[1].Y,p[2].Y})>viewport.MaxY)continue;
                if(area<0.0)std::swap(p[1],p[2]);
                try{ui->DL_AddTriangleFilled(list,p[0].X,p[0].Y,p[1].X,p[1].Y,p[2].X,p[2].Y,color);}
                catch(...){scratch.Clear();return submitted;}
                ++submitted.Triangles;
            }
        }
        catch(...){scratch.Clear();return submitted;}
        scratch.Clear();return submitted;
    }
    // Bands are intersected with each authoritative visible triangle. This
    // render-only operation cannot fill occlusion cuts or join separate soup.
    inline MiniMapBuildingVisuals::Detail::SplineSubmission SubmitWalkwayInsets(
        IModLoaderImGui* ui,PluginDrawList list,const MiniMapMap::Transform& transform,
        const A::RecordPlan& record,const MiniMapBuildingSplineEnvelope::Profile& profile,
        const MiniMapBuildingCollector::Bounds& viewport,
        uint32_t color,double fraction,WalkwayInsetScratch& scratch)
    {
        MiniMapBuildingVisuals::Detail::SplineSubmission submitted;
        if(!ui || !list || !ui->DL_AddTriangleFilled || !transform.Valid || !(color>>24) || !viewport.Valid || !scratch.Prepare())
            return submitted;
        if(!std::isfinite(viewport.MinX) || !std::isfinite(viewport.MinY) || !std::isfinite(viewport.MaxX) ||
            !std::isfinite(viewport.MaxY) || viewport.MinX>=viewport.MaxX || viewport.MinY>=viewport.MaxY)return submitted;
        try
        {
            for(const auto& plannedSection:record.Sections)
            {
                if(plannedSection.SectionIndex>=profile.Sections.size())continue;
                const auto& section=profile.Sections[plannedSection.SectionIndex];
                std::array<A::O::XY,4> left{},right{};
                if(!WalkwayInsetBands(section,fraction,left,right))continue;
                for(const auto& triangle:plannedSection.Visible)
                {
                    auto part=ClipAndSubmitWalkwayInsetTriangle(ui,list,transform,triangle,left,viewport,color,scratch);
                    submitted.Triangles+=part.Triangles;submitted.ProjectedVertices+=part.ProjectedVertices;
                    part=ClipAndSubmitWalkwayInsetTriangle(ui,list,transform,triangle,right,viewport,color,scratch);
                    submitted.Triangles+=part.Triangles;submitted.ProjectedVertices+=part.ProjectedVertices;
                }
            }
        }
        catch(...){scratch.Clear();return submitted;}
        scratch.Clear();return submitted;
    }
    // Complete, owned section plans only. Reproject without allocation or native
    // reads. Rendering counters count calls that returned from ImGui successfully.
    inline MiniMapBuildingVisuals::Detail::SplineSubmission Submit(IModLoaderImGui* ui,PluginDrawList list,
        const MiniMapMap::Transform& transform,const A::RecordPlan& record,
        const MiniMapBuildingCollector::Bounds& viewport,uint32_t color,SubmissionDiagnostics* diagnostic=nullptr)
    {
        MiniMapBuildingVisuals::Detail::SplineSubmission submitted;
        if(diagnostic)*diagnostic={};
        if(!ui || !list || !ui->DL_AddTriangleFilled || !transform.Valid || !(color>>24) || !viewport.Valid)
        {if(diagnostic){diagnostic->Transparent=!(color>>24);diagnostic->Rejected=!diagnostic->Transparent;}return submitted;}
        if(!std::isfinite(viewport.MinX) || !std::isfinite(viewport.MinY) || !std::isfinite(viewport.MaxX) ||
            !std::isfinite(viewport.MaxY) || viewport.MinX>=viewport.MaxX || viewport.MinY>=viewport.MaxY)
        {if(diagnostic)diagnostic->Rejected=true;return submitted;}
        for(const auto& section:record.Sections)for(const auto& triangle:section.Visible)
        {
            std::array<MiniMapMap::ScreenPoint,3> points;
            bool valid=true;
            for(size_t i=0;i<3;++i)
            {
                const auto& world=triangle.Vertices[i];auto& p=points[i];
                valid&=transform.WorldToScreen(world[0],world[1],p) && std::isfinite(p.X) && std::isfinite(p.Y);
            }
            submitted.ProjectedVertices+=3;if(!valid){if(diagnostic)++diagnostic->ProjectionRejectedTriangles;continue;}
            auto p=points[0],q=points[1],r=points[2];
            const double area=(double(q.X)-p.X)*(double(r.Y)-p.Y)-(double(q.Y)-p.Y)*(double(r.X)-p.X);
            if(!std::isfinite(area) || area==0){if(diagnostic)++diagnostic->ProjectionRejectedTriangles;continue;}
            if((std::max)({p.X,q.X,r.X})<viewport.MinX || (std::min)({p.X,q.X,r.X})>viewport.MaxX ||
                (std::max)({p.Y,q.Y,r.Y})<viewport.MinY || (std::min)({p.Y,q.Y,r.Y})>viewport.MaxY)
            {if(diagnostic)++diagnostic->OutsideViewportTriangles;continue;}
            if(area<0)std::swap(q,r);
            try{ui->DL_AddTriangleFilled(list,p.X,p.Y,q.X,q.Y,r.X,r.Y,color);}
            catch(...){if(diagnostic)diagnostic->Rejected=true;return submitted;} // Never retry a possibly recorded call.
            ++submitted.Triangles;
        }
        return submitted;
    }
}
#endif
