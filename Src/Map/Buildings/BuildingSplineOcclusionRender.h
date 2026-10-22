#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineOcclusionAdapter.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace MiniMapBuildingSplineOcclusionRender
{
    namespace A=MiniMapBuildingSplineOcclusionAdapter;
    inline double DroneRailZoomInFactor(double metersPerPixel,double minimum,double maximum)
    {
        if(!std::isfinite(metersPerPixel) || !std::isfinite(minimum) || !std::isfinite(maximum) || !(maximum>minimum))return 0.0;
        return std::clamp((maximum-metersPerPixel)/(maximum-minimum),0.0,1.0);
    }
    inline float DroneRailExtraWidthPixels(double metersPerPixel)
    {
        const double zoomIn=DroneRailZoomInFactor(metersPerPixel,
            MiniMapMap::MinimumMetersPerPixel,MiniMapMap::MaximumMetersPerPixel);
        return static_cast<float>(2.0*zoomIn);
    }
    struct SubmissionDiagnostics
    {
        size_t OutsideViewportTriangles=0,ProjectionRejectedTriangles=0;
        bool Rejected=false,Transparent=false;
    };
    struct BoundaryEdge
    {
        A::O::Point A{},B{};
        size_t Section=0;
    };
    inline bool SampleHeight(const MiniMapBuildingSplineEnvelope::Result& envelope,size_t sourceSegment,
        double t,double& height)
    {
        if(!envelope.Sampling || sourceSegment>=envelope.Sampling->Sections.size())return false;
        const auto& sampled=envelope.Sampling->Sections[sourceSegment];
        const auto& samples=envelope.Sampling->Samples;
        if(sampled.First>samples.size() || sampled.Count>samples.size()-sampled.First)return false;
        const auto begin=samples.begin()+sampled.First,end=begin+sampled.Count;
        const auto found=std::lower_bound(begin,end,t,[](const auto& sample,double key){return sample.T<key;});
        if(found==end || found->T!=t || !std::isfinite(found->Position[2]))return false;
        height=found->Position[2];return true;
    }
    inline bool OnCapLine(const A::O::Point& point,const A::O::Point& a,const A::O::Point& b)
    {
        long double direction[3],offset[3],magnitude=1;
        for(size_t axis=0;axis<3;++axis)
        {
            direction[axis]=static_cast<long double>(b[axis])-a[axis];
            offset[axis]=static_cast<long double>(point[axis])-a[axis];
            magnitude=(std::max)(magnitude,std::abs(static_cast<long double>(a[axis])));
            magnitude=(std::max)(magnitude,std::abs(static_cast<long double>(b[axis])));
            magnitude=(std::max)(magnitude,std::abs(static_cast<long double>(point[axis])));
        }
        const long double length2=direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2];
        if(!(length2>0) || !std::isfinite(length2))return false;
        const long double along=offset[0]*direction[0]+offset[1]*direction[1]+offset[2]*direction[2];
        const long double cx=offset[1]*direction[2]-offset[2]*direction[1];
        const long double cy=offset[2]*direction[0]-offset[0]*direction[2];
        const long double cz=offset[0]*direction[1]-offset[1]*direction[0];
        const long double tolerance=64*std::numeric_limits<double>::epsilon()*magnitude;
        return along>=-tolerance*std::sqrt(length2) && along<=length2+tolerance*std::sqrt(length2) &&
            cx*cx+cy*cy+cz*cz<=tolerance*tolerance*length2;
    }
    inline bool IsSectionCapEdge(const BoundaryEdge& edge,const MiniMapBuildingSplineEnvelope::Profile& profile,
        const MiniMapBuildingSplineEnvelope::Result& envelope,bool& isCap)
    {
        isCap=false;
        if(edge.Section>=profile.Sections.size())return false;
        const auto& section=profile.Sections[edge.Section];double startZ=0,endZ=0;
        if(!SampleHeight(envelope,section.SourceSegment,section.StartT,startZ) ||
            !SampleHeight(envelope,section.SourceSegment,section.EndT,endZ))return false;
        const A::O::Point startA{section.Start.Edges[0][0],section.Start.Edges[0][1],startZ};
        const A::O::Point startB{section.Start.Edges[1][0],section.Start.Edges[1][1],startZ};
        const A::O::Point endA{section.End.Edges[0][0],section.End.Edges[0][1],endZ};
        const A::O::Point endB{section.End.Edges[1][0],section.End.Edges[1][1],endZ};
        isCap=(OnCapLine(edge.A,startA,startB) && OnCapLine(edge.B,startA,startB)) ||
            (OnCapLine(edge.A,endA,endB) && OnCapLine(edge.B,endA,endB));
        return true;
    }
    // Extract only longitudinal sides from the already-clipped triangle soup.
    // Exact shared edges cancel within a section (fan/diagonal/subtraction
    // partitions) and across consecutive source sections (the ribbon seam).
    // Cross-section head/tail edges are filtered using the envelope's owned
    // section endpoints. No coordinate snapping or connectivity is inferred.
    // Ambiguous duplicates and missing cap metadata fail closed.
    inline size_t VisibleBoundary(const A::RecordPlan& record,const MiniMapBuildingSplineEnvelope::Profile& profile,
        const MiniMapBuildingSplineEnvelope::Result& envelope,std::vector<BoundaryEdge>& scratch)
    {
        scratch.clear();
        try
        {
            for(const auto& section:record.Sections)for(const auto& triangle:section.Visible)
                for(size_t i=0;i<3;++i)
                {
                    auto a=triangle.Vertices[i],b=triangle.Vertices[(i+1)%3];
                    if(b<a)std::swap(a,b);
                    scratch.push_back({a,b,section.SectionIndex});
                }
        }
        catch(const std::bad_alloc&){scratch.clear();return 0;}
        catch(const std::length_error&){scratch.clear();return 0;}
        std::sort(scratch.begin(),scratch.end(),[](const BoundaryEdge& a,const BoundaryEdge& b)
        {
            if(a.A!=b.A)return a.A<b.A;
            if(a.B!=b.B)return a.B<b.B;
            return a.Section<b.Section;
        });
        size_t output=0;
        for(size_t first=0;first<scratch.size();)
        {
            size_t end=first+1;
            while(end<scratch.size() && scratch[end].A==scratch[first].A && scratch[end].B==scratch[first].B)++end;
            const size_t count=end-first;
            if(count==1)
            {
                bool cap=false;
                if(!IsSectionCapEdge(scratch[first],profile,envelope,cap)){scratch.clear();return 0;}
                if(!cap)scratch[output++]=scratch[first];
            }
            // Exact duplicates cancel internal triangle edges and adjoining
            // section seams. Higher multiplicity or same-direction duplicates
            // are ambiguous; suppress them instead of inventing connectivity.
            first=end;
        }
        scratch.resize(output);
        return output;
    }
    // The parent draw list already owns the viewport scissor. Drawing only
    // extracted mesh edges means the scissor clips strokes without inventing
    // an outline edge along the viewport boundary.
    inline size_t SubmitDroneRailOutline(IModLoaderImGui* ui,PluginDrawList list,
        const MiniMapMap::Transform& transform,const A::RecordPlan& record,
        const MiniMapBuildingSplineEnvelope::Profile& profile,const MiniMapBuildingSplineEnvelope::Result& envelope,
        uint32_t color,float thickness,std::vector<BoundaryEdge>& scratch)
    {
        if(!ui || !list || !ui->DL_AddLine || !transform.Valid || !(color>>24))return 0;
        const auto count=VisibleBoundary(record,profile,envelope,scratch);size_t submitted=0;
        for(size_t i=0;i<count;++i)
        {
            MiniMapMap::ScreenPoint a,b;
            const auto& edge=scratch[i];
            if(!transform.WorldToScreen(edge.A[0],edge.A[1],a) || !transform.WorldToScreen(edge.B[0],edge.B[1],b) ||
                !std::isfinite(a.X) || !std::isfinite(a.Y) || !std::isfinite(b.X) || !std::isfinite(b.Y) ||
                (a.X==b.X && a.Y==b.Y))continue;
            try{ui->DL_AddLine(list,a.X,a.Y,b.X,b.Y,color,thickness);}
            catch(...){return submitted;}
            ++submitted;
        }
        return submitted;
    }
    inline bool FullyOccluded(const A::RecordPlan& record)
    {
        return !record.Sections.empty() && !record.FailedSections &&
            std::all_of(record.Sections.begin(),record.Sections.end(),[](const auto& section){return section.Visible.empty();});
    }
    struct WidthExpansion
    {
        MiniMapMap::ScreenPoint Origin{};
        float NormalX=0.0f,NormalY=0.0f,HalfWidth=0.0f;
        bool Valid=false;
    };
    inline WidthExpansion SectionWidthExpansion(const MiniMapMap::Transform& transform,
        const MiniMapBuildingSplineEnvelope::Profile* profile,size_t sectionIndex,float extraWidthPixels)
    {
        WidthExpansion expansion;
        if(!profile || !(extraWidthPixels>0.0f) || !std::isfinite(extraWidthPixels) ||
            sectionIndex>=profile->Sections.size())return expansion;
        const auto& section=profile->Sections[sectionIndex];
        if(!transform.WorldToScreen(section.Start.Position[0],section.Start.Position[1],expansion.Origin))return expansion;
        MiniMapMap::ScreenPoint direction;
        if(!transform.DirectionToScreen(section.End.Position[0]-section.Start.Position[0],
            section.End.Position[1]-section.Start.Position[1],direction))return expansion;
        const double length=std::hypot(double(direction.X),double(direction.Y));
        if(!std::isfinite(length) || !(length>0.0))return expansion;
        expansion.NormalX=static_cast<float>(-double(direction.Y)/length);
        expansion.NormalY=static_cast<float>(double(direction.X)/length);
        expansion.HalfWidth=extraWidthPixels*0.5f;
        expansion.Valid=std::isfinite(expansion.Origin.X) && std::isfinite(expansion.Origin.Y) &&
            std::isfinite(expansion.NormalX) && std::isfinite(expansion.NormalY) &&
            std::isfinite(expansion.HalfWidth) && expansion.HalfWidth>0.0f;
        return expansion;
    }
    inline void ExpandTriangleTransversely(std::array<MiniMapMap::ScreenPoint,3>& points,
        const WidthExpansion& expansion)
    {
        if(!expansion.Valid)return;
        const auto& origin=expansion.Origin;
        const float tangentX=expansion.NormalY,tangentY=-expansion.NormalX;
        for(auto& point:points)
        {
            const double side=double(tangentX)*(double(point.Y)-origin.Y)-
                double(tangentY)*(double(point.X)-origin.X);
            if(side==0.0)continue;
            const float offset=side>0.0?expansion.HalfWidth:-expansion.HalfWidth;
            point.X+=expansion.NormalX*offset;
            point.Y+=expansion.NormalY*offset;
        }
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
        const MiniMapBuildingCollector::Bounds& viewport,uint32_t color,SubmissionDiagnostics* diagnostic=nullptr,
        const MiniMapBuildingSplineEnvelope::Profile* profile=nullptr,float extraWidthPixels=0.0f)
    {
        MiniMapBuildingVisuals::Detail::SplineSubmission submitted;
        if(diagnostic)*diagnostic={};
        if(!ui || !list || !ui->DL_AddTriangleFilled || !transform.Valid || !(color>>24) || !viewport.Valid)
        {if(diagnostic){diagnostic->Transparent=!(color>>24);diagnostic->Rejected=!diagnostic->Transparent;}return submitted;}
        if(!std::isfinite(viewport.MinX) || !std::isfinite(viewport.MinY) || !std::isfinite(viewport.MaxX) ||
            !std::isfinite(viewport.MaxY) || viewport.MinX>=viewport.MaxX || viewport.MinY>=viewport.MaxY)
        {if(diagnostic)diagnostic->Rejected=true;return submitted;}
        // Keep the original projection/submission path byte-for-byte for every
        // non-expanded category and at maximum zoom-out.
        if(!profile || !(extraWidthPixels>0.0f) || !std::isfinite(extraWidthPixels))
        {
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
        for(const auto& section:record.Sections)
        {
            const auto expansion=SectionWidthExpansion(transform,profile,section.SectionIndex,extraWidthPixels);
            for(const auto& triangle:section.Visible)
            {
                std::array<MiniMapMap::ScreenPoint,3> points;
                bool valid=true;
                for(size_t i=0;i<3;++i)
                {
                    const auto& world=triangle.Vertices[i];auto& p=points[i];
                    valid&=transform.WorldToScreen(world[0],world[1],p) && std::isfinite(p.X) && std::isfinite(p.Y);
                }
                submitted.ProjectedVertices+=3;if(!valid){if(diagnostic)++diagnostic->ProjectionRejectedTriangles;continue;}
                ExpandTriangleTransversely(points,expansion);
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
        }
        return submitted;
    }
}
#endif
