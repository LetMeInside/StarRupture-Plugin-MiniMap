#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineOcclusionAdapter.h"

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
