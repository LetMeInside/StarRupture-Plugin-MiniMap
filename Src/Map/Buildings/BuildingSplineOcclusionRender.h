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
            std::all_of(record.Sections.begin(),record.Sections.end(),[](const auto& section){return section->Geometry->Visible.empty();});
    }
    // Reproject final plan-owned inset triangles in section/visible/left/right/fan order.
    inline MiniMapBuildingVisuals::Detail::SplineSubmission SubmitWalkwayInsets(
        IModLoaderImGui* ui,PluginDrawList list,const MiniMapMap::Transform& transform,
        const A::RecordPlan& record,const MiniMapBuildingCollector::Bounds& viewport,uint32_t color)
    {
        MiniMapBuildingVisuals::Detail::SplineSubmission submitted;
        if(!ui || !list || !ui->DL_AddTriangleFilled || !transform.Valid || !(color>>24) || !viewport.Valid)return submitted;
        if(!std::isfinite(viewport.MinX) || !std::isfinite(viewport.MinY) || !std::isfinite(viewport.MaxX) ||
            !std::isfinite(viewport.MaxY) || viewport.MinX>=viewport.MaxX || viewport.MinY>=viewport.MaxY)return submitted;
        namespace O=A::O;
        for(const auto& section:record.Sections)for(size_t first=0;first<section->Geometry->WalkwayInsets.size();++first)
        {
            const auto& triangle=section->Geometry->WalkwayInsets[first];
            const auto& a=triangle.Vertices[0];const auto& b=triangle.Vertices[1];const auto& c=triangle.Vertices[2];
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
            catch(...)
            {
                // The original helper stopped only this band after a possibly recorded call.
                // Skip its remaining fan triangles, then continue with the next band.
                while(first+1<section->Geometry->WalkwayInsets.size() && section->Geometry->WalkwayInsets[first+1].Key==triangle.Key)++first;
                continue;
            }
            ++submitted.Triangles;
        }
        return submitted;
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
        for(const auto& section:record.Sections)for(const auto& triangle:section->Geometry->Visible)
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
            catch(...){
                if(diagnostic)diagnostic->Rejected=true;return submitted;} // Never retry a possibly recorded call.
            ++submitted.Triangles;
        }
        return submitted;
    }
}
#endif
