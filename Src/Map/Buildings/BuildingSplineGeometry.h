#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include <algorithm>
#include <cmath>

// Mathematics in the collector's copied coordinate basis, in Unreal units.
// No entity transform is applied. This does not certify the native fragment's
// initialized/reconstructed basis as world space. Profiles/occlusion need that
// separate contract, plus a verified component frame, before consuming surfaces.
namespace MiniMapBuildingSplineGeometry
{
    namespace C = MiniMapBuildingCollector;
    inline constexpr size_t MaximumSegments=255, MaximumSamples=4096;
    inline constexpr unsigned MaximumDepth=8;
    // Native copied EInterpCurveMode values; checked against SDK in the consumer.
    inline constexpr uint8_t Linear=0, Constant=2;
    inline bool Finite(const C::Point& p)
    { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
    inline C::Point Mix(const C::Point& a,const C::Point& b,double t)
    {
        C::Point p;
        for(size_t i=0;i<3;++i)p[i]=a[i]*(1-t)+b[i]*t;
        return p;
    }
    inline C::GeometryIssue Validate(const C::CurveSegment& s)
    {
        if(s.Mode>5)return C::GeometryIssue::UnsupportedMode;
        if(!std::isfinite(s.StartKey) || !std::isfinite(s.EndKey))return C::GeometryIssue::NonFinite;
        const double dt=s.EndKey-s.StartKey;
        if(!std::isfinite(dt) || dt<=0)return C::GeometryIssue::InvalidCurve;
        for(const auto& p:s.Controls)if(!Finite(p))return C::GeometryIssue::NonFinite;
        return C::GeometryIssue::None;
    }
    // t is normalized within this segment. Tangent is dXYZ/d(native input key),
    // NOT a unit vector, arc-length derivative or mesh frame. Zero derivatives
    // are valid mathematical results but do not define an extrusion direction.
    struct Evaluation
    {
        C::Point Position={}, Tangent={};
        bool HasTangent=false;
    };
    inline bool Evaluate(const C::CurveSegment& s,double t,Evaluation& out)
    {
        if(!std::isfinite(t) || t<0 || t>1 || Validate(s)!=C::GeometryIssue::None)return false;
        Evaluation value;
        if(s.Mode==Constant)value.Position=s.Controls[t==1?3:0];
        else if(s.Mode==Linear)
        {
            value.Position=Mix(s.Controls[0],s.Controls[3],t);
            for(size_t i=0;i<3;++i)value.Tangent[i]=(s.Controls[3][i]-s.Controls[0][i])/(s.EndKey-s.StartKey);
            value.HasTangent=true;
        }
        else
        {
            const auto a=Mix(s.Controls[0],s.Controls[1],t),b=Mix(s.Controls[1],s.Controls[2],t),c=Mix(s.Controls[2],s.Controls[3],t);
            const auto d=Mix(a,b,t),e=Mix(b,c,t);
            value.Position=Mix(d,e,t);
            for(size_t i=0;i<3;++i)value.Tangent[i]=3*(e[i]-d[i])/(s.EndKey-s.StartKey);
            value.HasTangent=true;
        }
        // Preserve exact source endpoints, including signed zero.
        if(t==0)value.Position=s.Controls[0];
        if(t==1)value.Position=s.Controls[3];
        if(!Finite(value.Position) || !Finite(value.Tangent))return false;
        out=value;return true;
    }
    struct SamplingPolicy
    {
        double PixelsPerUnit=1, XYErrorPixels=0.75, HeightErrorUnits=1;
        unsigned Depth=MaximumDepth;
        size_t SampleLimit=MaximumSamples;
    };
    struct Sample
    {
        C::Point Position;
        double T=0; // Exact dyadic segment parameter; source keys remain in CurveSegment.
        bool operator==(const Sample&) const = default;
    };
    struct Section
    {
        size_t First=0, Count=0; // Index in Samples; same index as source Segments.
        bool Discontinuous=false, StartsNewRun=true;
        bool operator==(const Section&) const = default;
    };
    struct PreparedCurve
    {
        std::vector<Sample> Samples;
        std::vector<Section> Sections;
        C::GeometryIssue SourceIssue=C::GeometryIssue::None;
        C::GeometryIssue PreparationIssue=C::GeometryIssue::None;
        size_t LimitedLeaves=0;
        bool Reconstructed=false;
        // Mathematical sampling completeness only, never visual renderability.
        bool Complete() const { return !Sections.empty() && SourceIssue==C::GeometryIssue::None && PreparationIssue==C::GeometryIssue::None && !LimitedLeaves; }
    };
    namespace Detail
    {
        inline void Append(PreparedCurve& out,const C::Point& p,double t)
        {
            if(out.Samples.size()==out.Samples.capacity())
                out.Samples.reserve((std::min)(MaximumSamples,(std::max)(size_t(64),out.Samples.capacity()*2)));
            out.Samples.push_back({p,t});
        }
        inline bool Flat(const std::array<C::Point,4>& p,const SamplingPolicy& policy)
        {
            const double dx=p[3][0]-p[0][0],dy=p[3][1]-p[0][1];
            const double length=std::hypot(dx,dy);
            for(size_t i=1;i<=2;++i)
            {
                double fraction=0;
                if(length>0)fraction=std::clamp(((p[i][0]-p[0][0])*(dx/length)+(p[i][1]-p[0][1])*(dy/length))/length,0.0,1.0);
                const double distance=std::hypot(p[i][0]-(p[0][0]+fraction*dx),p[i][1]-(p[0][1]+fraction*dy));
                // Compare Z to the degree-elevated linear chord at the SAME t.
                // Convex-hull bounds then cover height excursions, even when XY
                // is straight or Z crosses the chord several times.
                const double height=std::abs(p[i][2]-(p[0][2]*(1-double(i)/3)+p[3][2]*(double(i)/3)));
                if(!std::isfinite(distance) || !std::isfinite(height) ||
                    distance*policy.PixelsPerUnit>policy.XYErrorPixels || height>policy.HeightErrorUnits)return false;
            }
            return true;
        }
        inline void Divide(const std::array<C::Point,4>& p,double t0,double t1,
            const SamplingPolicy& policy,unsigned depth,size_t leaves,PreparedCurve& out)
        {
            const bool flat=Flat(p,policy);
            if(flat || depth==policy.Depth || leaves==1)
            {
                out.LimitedLeaves+=!flat;
                Append(out,p[3],t1);return;
            }
            const auto a=Mix(p[0],p[1],0.5),b=Mix(p[1],p[2],0.5),c=Mix(p[2],p[3],0.5);
            const auto d=Mix(a,b,0.5),e=Mix(b,c,0.5),m=Mix(d,e,0.5);
            const double tm=(t0+t1)*0.5;
            Divide({p[0],a,d,m},t0,tm,policy,depth+1,leaves/2,out);
            Divide({m,e,c,p[3]},tm,t1,policy,depth+1,leaves-leaves/2,out);
        }
    }
    // Every admitted source segment gets a section and BOTH endpoints, even at
    // the budget. LimitedLeaves explicitly disclaims the requested tolerance;
    // source controls are retained by SpriteSource, never replaced by samples.
    inline PreparedCurve Prepare(const C::SplineGeometry& source,SamplingPolicy policy)
    {
        PreparedCurve out;out.SourceIssue=source.Issue;out.Reconstructed=source.Reconstructed;
        if(source.Issue!=C::GeometryIssue::None)return out;
        if(source.Segments.empty()) {out.PreparationIssue=C::GeometryIssue::InvalidCurve;return out;}
        if(source.Segments.size()>MaximumSegments) {out.PreparationIssue=C::GeometryIssue::Oversized;return out;}
        policy.Depth=(std::min)(policy.Depth,MaximumDepth);
        policy.SampleLimit=(std::min)(policy.SampleLimit,MaximumSamples);
        if(!std::isfinite(policy.PixelsPerUnit) || policy.PixelsPerUnit<=0 ||
            !std::isfinite(policy.XYErrorPixels) || policy.XYErrorPixels<=0 ||
            !std::isfinite(policy.HeightErrorUnits) || policy.HeightErrorUnits<=0 ||
            policy.SampleLimit<2*source.Segments.size())
        {out.PreparationIssue=C::GeometryIssue::InvalidCurve;return out;}
        for(const auto& s:source.Segments)
            if(const auto issue=Validate(s);issue!=C::GeometryIssue::None) {out.PreparationIssue=issue;return out;}
        // Small initial storage; explicit growth remains within MaximumSamples.
        // No temporary copy of the source vector.
        out.Samples.reserve((std::min)(size_t(64),policy.SampleLimit));out.Sections.reserve(source.Segments.size());
        for(size_t i=0;i<source.Segments.size();++i)
        {
            const auto& s=source.Segments[i];
            const size_t budget=(std::min)(size_t(257),(policy.SampleLimit-out.Samples.size())/(source.Segments.size()-i));
            Section section;section.First=out.Samples.size();section.Discontinuous=s.Mode==Constant;
            section.StartsNewRun=i==0 || section.Discontinuous || source.Segments[i-1].Mode==Constant ||
                source.Segments[i-1].EndKey!=s.StartKey || source.Segments[i-1].Controls[3]!=s.Controls[0];
            Detail::Append(out,s.Controls[0],0);
            if(s.Mode==Linear || s.Mode==Constant)Detail::Append(out,s.Controls[3],1);
            else Detail::Divide(s.Controls,0,1,policy,0,budget-1,out);
            section.Count=out.Samples.size()-section.First;out.Sections.push_back(section);
        }
        return out;
    }
}
#endif
