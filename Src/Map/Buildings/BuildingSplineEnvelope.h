#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineGeometry.h"
#include "BuildingSplineProfiles.h"
#include <limits>

// Pure copied-data preparation. These are bounds envelopes of logical paths,
// not native deformed mesh surfaces or conservative collector admission bounds.
namespace MiniMapBuildingSplineEnvelope
{
    namespace C=MiniMapBuildingCollector;
    namespace S=MiniMapBuildingSplineGeometry;
    namespace P=MiniMapBuildingSplineProfiles;
    namespace V=MiniMapBuildingGeometryTypes;
    using XY=std::array<double,2>;
    inline constexpr uint32_t NumericalPolicy=1;
    enum class State : uint8_t { Pending, Partial, Complete, Unsupported, Invalid };
    enum Reason : uint32_t
    {
        None=0, MissingCurve=1, MissingMetadata=2, Unselected=4, UnverifiedActiveUse=8,
        MissingFields=16, UnsupportedConstruction=32, InvalidSource=64, InvalidRoot=128,
        DegenerateFrame=256, Discontinuity=512, NativeSectionsUnknown=1024,
        RailEndpointsUnknown=2048, AdditionalCoverageUnknown=4096, SamplingLimited=8192,
        ConservativeFrameBounds=16384, DegenerateProjection=32768, JoinsAndCapsUnknown=65536,
        RibbonApproximation=131072, UnsupportedProjection=262144
    };
    struct Interval {double Low=0,High=0;};
    struct Ring
    {
        XY Position{};
        Interval Lateral; // Signed displacement from world-space path, never recentered.
        XY LateralDirection{};
        std::array<XY,2> Edges{}; // Low/high normal projections, shared at adjacent samples.
    };
    struct Section
    {
        size_t SourceSegment=0,FirstVertex=0,VertexCount=0,FirstIndex=0,IndexCount=0;
        double StartT=0,EndT=1;
        Ring Start,End;
    };
    // Published through const Result only. Copies share storage; every mutable
    // operation detaches first. No mutable array borrow escapes preparation.
    // This keeps the existing read interface for selection/render consumers.
    template<class T> class SharedValues
    {
        std::shared_ptr<std::vector<T>> Values;
        const std::vector<T>& Read() const
        {static const std::vector<T> empty;return Values?*Values:empty;}
        std::vector<T>& Write()
        {
            if(!Values)Values=std::make_shared<std::vector<T>>();
            else if(Values.use_count()!=1)Values=std::make_shared<std::vector<T>>(*Values);
            return *Values;
        }
    public:
        using const_iterator=typename std::vector<T>::const_iterator;
        SharedValues()=default;
        SharedValues(std::initializer_list<T> values):Values(std::make_shared<std::vector<T>>(values)){}
        size_t size() const noexcept {return Values?Values->size():0;}
        bool empty() const noexcept {return !size();}
        const T* data() const noexcept {return Values?Values->data():nullptr;}
        const T& operator[](size_t i) const {return Read()[i];}
        T& operator[](size_t i) {return Write()[i];}
        const_iterator begin() const {return Read().begin();}
        const_iterator end() const {return Read().end();}
        void clear() noexcept {Values.reset();}
        void push_back(const T& value) {Write().push_back(value);}
        template<class It> void insert(const_iterator position,It first,It last)
        {
            const auto offset=position-begin();auto& values=Write();
            values.insert(values.begin()+offset,first,last);
        }
        void insert(const_iterator position,size_t count,const T& value)
        {
            const auto offset=position-begin();auto& values=Write();
            values.insert(values.begin()+offset,count,value);
        }
        bool Shares(const SharedValues& other) const noexcept {return Values==other.Values;}
    };
    struct Profile
    {
        bool SyntheticThin=false; // Owned numerical policy, never a native observation.
        size_t SourceProfile=0;
        State Status=State::Unsupported;
        uint32_t Reasons=None;
        SharedValues<XY> Vertices; // World XY; no root or map transform remains to apply.
        SharedValues<uint32_t> Indices; // CCW triangles in independently validated ribbon cells.
        SharedValues<Section> Sections; // No inferred joins, caps or run connections.
    };
    struct Result
    {
        uint64_t Generation=0;
        State Status=State::Pending;
        uint32_t Reasons=None;
        std::vector<Profile> Profiles;
        std::shared_ptr<const S::PreparedCurve> Sampling;
        C::Record Input; // Owned copied input witness; never a native pointer/borrow.
        bool SyntheticThin=false;
        uint32_t Policy=NumericalPolicy; // Sampling output is compared separately.
    };
    inline std::shared_ptr<const P::Set> Metadata(const C::Record& record)
    {return record.DefinitionShape?record.DefinitionShape->Profiles:nullptr;}
    inline bool NumericalEquivalent(const C::Record& a,const S::PreparedCurve* sampled,const Result& previous,bool thin=false)
    {
        const auto& b=previous.Input;
        return previous.Policy==NumericalPolicy && previous.SyntheticThin==thin && a.Generation==b.Generation && a.Index==b.Index && a.Serial==b.Serial &&
            a.BuildingId==b.BuildingId && a.HasSpline==b.HasSpline && a.DefinitionValid==b.DefinitionValid &&
            a.Curve==b.Curve && a.DefinitionShape==b.DefinitionShape && a.Position==b.Position && a.Scale==b.Scale &&
            a.Rotation==b.Rotation && sampled==previous.Sampling.get();
    }
    inline V::Point Cross(const V::Point& a,const V::Point& b)
    {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
    inline bool Normalize(V::Point& value)
    {
        const double length=std::hypot(value[0],value[1],value[2]);
        if(!std::isfinite(length) || length==0)return false;
        for(auto& v:value)v/=length;
        return V::Finite(value);
    }
    inline bool Inverse(const V::Transform& root,const V::Point& world,V::Point& local,bool vector)
    {
        if(!V::Valid(root) || !V::Finite(world))return false;
        for(double scale:root.Scale)if(scale==0 || !std::isfinite(1/scale))return false;
        V::Transform inverseRotation;
        inverseRotation.Rotation={-root.Rotation[0],-root.Rotation[1],-root.Rotation[2],root.Rotation[3]};
        V::Point delta=world;
        if(!vector)for(size_t i=0;i<3;++i)delta[i]-=root.Translation[i];
        if(!V::ToWorld(inverseRotation,delta,local,true))return false;
        for(size_t i=0;i<3;++i)local[i]/=root.Scale[i];
        return V::Finite(local);
    }
    inline Interval Add(Interval a,Interval b){return {a.Low+b.Low,a.High+b.High};}
    inline Interval Negate(Interval a){return {-a.High,-a.Low};}
    inline Interval Multiply(Interval a,Interval b)
    {
        const std::array<double,4> values{a.Low*b.Low,a.Low*b.High,a.High*b.Low,a.High*b.High};
        return {*(std::min_element)(values.begin(),values.end()),*(std::max_element)(values.begin(),values.end())};
    }
    inline bool Valid(Interval value)
    {return std::isfinite(value.Low) && std::isfinite(value.High) && value.Low<=value.High;}
    inline double MinimumMagnitude(Interval a){return a.Low>0?a.Low:a.High<0?-a.High:0;}
    inline double MaximumMagnitude(Interval a){return (std::max)(std::abs(a.Low),std::abs(a.High));}
    inline Interval UnitComponent(Interval value)
    {return {(std::max)(-1.0,value.Low),(std::min)(1.0,value.High)};}
    inline Interval Dimension(const P::Bounds& bounds,size_t axis,double scale)
    {return Multiply({bounds.Origin[axis]-bounds.Extent[axis],bounds.Origin[axis]+bounds.Extent[axis]},{scale,scale});}
    inline bool Frame(V::Point tangent,V::Point& y,V::Point& z,const V::Point& up={0,0,1})
    {
        V::Point reference=up;
        if(!Normalize(tangent) || !Normalize(reference))return false;
        y=Cross(reference,tangent);
        if(!Normalize(y))return false;
        z=Cross(tangent,y);return Normalize(z);
    }
    inline bool Project(const V::Transform& root,const S::Evaluation& value,const P::Bounds& bounds,
        const P::CrossSection& cross,std::array<XY,4>& corners,Ring& ring,const V::Transform& component={})
    {
        V::Point localTangent,y,z;
        if(!value.HasTangent || !Inverse(root,value.Tangent,localTangent,true) ||
            !Inverse(component,localTangent,localTangent,true) || !Frame(localTangent,y,z,cross.Up))return false;
        const double length=std::hypot(value.Tangent[0],value.Tangent[1]);
        if(!std::isfinite(length) || length==0)return false;
        const XY lateral{-value.Tangent[1]/length,value.Tangent[0]/length};
        const auto rangeY=Dimension(bounds,1,cross.StartScale[0]),rangeZ=Dimension(bounds,2,cross.StartScale[1]);
        if(!Valid(rangeY) || !Valid(rangeZ))return false;
        ring.Position={value.Position[0],value.Position[1]};
        ring.LateralDirection=lateral;
        ring.Lateral={std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
        for(size_t i=0;i<4;++i)
        {
            const double cy=(i&1)?rangeY.High:rangeY.Low,cz=(i&2)?rangeZ.High:rangeZ.Low;
            V::Point local{},offset;
            for(size_t axis=0;axis<3;++axis)
                local[axis]=y[axis]*(cy+cross.StartOffset[0])+z[axis]*(cz+cross.StartOffset[1]);
            if(!V::ToWorld(component,local,local))return false;
            if(!V::ToWorld(root,local,offset,true))return false;
            // Stored controls already are world-space. Apply the root LINEAR
            // transform to cross-section offsets only, then add world position.
            corners[i]={value.Position[0]+offset[0],value.Position[1]+offset[1]};
            const double displacement=offset[0]*lateral[0]+offset[1]*lateral[1];
            ring.Lateral.Low=(std::min)(ring.Lateral.Low,displacement);
            ring.Lateral.High=(std::max)(ring.Lateral.High,displacement);
            if(!std::isfinite(corners[i][0]) || !std::isfinite(corners[i][1]))return false;
        }
        if(!Valid(ring.Lateral))return false;
        for(size_t i=0;i<2;++i)
        {
            const double displacement=i?ring.Lateral.High:ring.Lateral.Low;
            ring.Edges[i]={ring.Position[0]+lateral[0]*displacement,ring.Position[1]+lateral[1]*displacement};
            if(!std::isfinite(ring.Edges[i][0]) || !std::isfinite(ring.Edges[i][1]))return false;
        }
        return true;
    }
    inline void Split(const std::array<C::Point,4>& p,double t,std::array<C::Point,4>& left,std::array<C::Point,4>& right)
    {
        const auto a=S::Mix(p[0],p[1],t),b=S::Mix(p[1],p[2],t),c=S::Mix(p[2],p[3],t);
        const auto d=S::Mix(a,b,t),e=S::Mix(b,c,t),m=S::Mix(d,e,t);
        left={p[0],a,d,m};right={m,e,c,p[3]};
    }
    inline std::array<C::Point,4> Subcurve(const C::CurveSegment& source,double a,double b)
    {
        std::array<C::Point,4> left,right,result;
        Split(source.Controls,b,left,right);
        if(a==0)return left;
        Split(left,a/b,right,result);return result;
    }
    // Conservative COVERAGE analysis only. Neither these bounds nor subcurve
    // control points supply drawable vertices. Visual cells use sampled rings.
    // Derivative Bezier convex-hull intervals bound every frame inside a cell.
    // A cone that cannot exclude a vertical/zero tangent is rejected, not given
    // an invented frame. Correlation loss widens the envelope explicitly.
    inline bool FrameBounds(const V::Transform& root,const std::array<C::Point,4>& controls,
        const P::Bounds& bounds,const P::CrossSection& cross,std::array<Interval,2>& offset)
    {
        std::array<Interval,3> d;
        for(auto& interval:d)interval={std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
        for(size_t i=0;i<3;++i)
        {
            V::Point world{},local;
            for(size_t axis=0;axis<3;++axis)world[axis]=controls[i+1][axis]-controls[i][axis];
            if(!Inverse(root,world,local,true))return false;
            for(size_t axis=0;axis<3;++axis)
            {d[axis].Low=(std::min)(d[axis].Low,local[axis]);d[axis].High=(std::max)(d[axis].High,local[axis]);}
        }
        const double xyLow=std::hypot(MinimumMagnitude(d[0]),MinimumMagnitude(d[1]));
        const double xyHigh=std::hypot(MaximumMagnitude(d[0]),MaximumMagnitude(d[1]));
        const double lengthLow=std::hypot(xyLow,MinimumMagnitude(d[2]));
        const double lengthHigh=std::hypot(xyHigh,MaximumMagnitude(d[2]));
        if(xyLow==0 || !std::isfinite(xyHigh) || !std::isfinite(lengthHigh) || !std::isfinite(1/xyLow) ||
            !std::isfinite(1/lengthLow))return false;
        const Interval inverseXY{1/xyHigh,1/xyLow},inverseLength{1/lengthHigh,1/lengthLow};
        const std::array<Interval,3> y{UnitComponent(Multiply(Negate(d[1]),inverseXY)),UnitComponent(Multiply(d[0],inverseXY)),Interval{}};
        std::array<Interval,3> tangent,z;
        for(size_t axis=0;axis<3;++axis)tangent[axis]=UnitComponent(Multiply(d[axis],inverseLength));
        z[0]=Negate(Multiply(tangent[2],y[1]));z[1]=Multiply(tangent[2],y[0]);
        z[2]=Add(Multiply(tangent[0],y[1]),Negate(Multiply(tangent[1],y[0])));
        // Each exact frame axis is unit length. Intersecting these mathematical
        // bounds avoids interval blow-up near a vertical cone, without a width cap.
        for(auto& component:z)component=UnitComponent(component);
        const auto cy=Dimension(bounds,1,cross.StartScale[0]),cz=Dimension(bounds,2,cross.StartScale[1]);
        std::array<V::Point,3> columns;
        for(size_t i=0;i<3;++i)
        {V::Point unit{};unit[i]=1;if(!V::ToWorld(root,unit,columns[i],true))return false;}
        for(size_t axis=0;axis<2;++axis)
        {
            offset[axis]={};
            for(size_t i=0;i<3;++i)
                offset[axis]=Add(offset[axis],Multiply(Add(Multiply(y[i],cy),Multiply(z[i],cz)),{columns[i][axis],columns[i][axis]}));
            if(!Valid(offset[axis]))return false;
        }
        return true;
    }
    inline double Area(XY a,XY b,XY c)
    {return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);}
    inline bool Intersects(XY a,XY b,XY c,XY d)
    {
        const double ac=Area(a,b,c),ad=Area(a,b,d),ca=Area(c,d,a),cb=Area(c,d,b);
        if(!std::isfinite(ac) || !std::isfinite(ad) || !std::isfinite(ca) || !std::isfinite(cb))return true;
        auto opposite=[](double x,double y){return (x<0 && y>0) || (x>0 && y<0);};
        auto contains=[](XY x,XY y,XY p)
        {
            return p[0]>=(std::min)(x[0],y[0]) && p[0]<=(std::max)(x[0],y[0]) &&
                p[1]>=(std::min)(x[1],y[1]) && p[1]<=(std::max)(x[1],y[1]);
        };
        return (opposite(ac,ad) && opposite(ca,cb)) || (ac==0 && contains(a,b,c)) ||
            (ad==0 && contains(a,b,d)) || (ca==0 && contains(c,d,a)) || (cb==0 && contains(c,d,b));
    }
    inline bool AppendRibbon(Section section,Profile& output)
    {
        const auto& start=section.Start;const auto& end=section.End;
        if(!Valid(start.Lateral) || !Valid(end.Lateral) || start.Lateral.Low==start.Lateral.High ||
            end.Lateral.Low==end.Lateral.High || start.Position==end.Position)return false;
        const std::array<XY,4> points{start.Edges[0],end.Edges[0],end.Edges[1],start.Edges[1]};
        for(const auto& point:points)if(!std::isfinite(point[0]) || !std::isfinite(point[1]))return false;
        // Each cell stays forward of its start cross-section and behind its end
        // cross-section. Thus consecutive cells sharing a sample cannot overlap
        // across that edge. A tight inner fold is split out, never hull-filled.
        auto forward=[](const Ring& ring,XY point)
        {return (point[0]-ring.Position[0])*ring.LateralDirection[1]-(point[1]-ring.Position[1])*ring.LateralDirection[0];};
        for(size_t side=0;side<2;++side)
        {
            const double atStart=forward(start,end.Edges[side]),atEnd=forward(end,start.Edges[side]);
            if(!std::isfinite(atStart) || !std::isfinite(atEnd) || atStart<=0 || atEnd>=0)return false;
        }
        if(Intersects(points[0],points[1],points[2],points[3]) ||
            Intersects(points[1],points[2],points[3],points[0]))return false;
        auto positive=[&](size_t a,size_t b,size_t c)
        {const double area=Area(points[a],points[b],points[c]);return std::isfinite(area) && area>0;};
        std::array<uint32_t,6> indices;
        if(positive(0,1,2) && positive(0,2,3))indices={0,1,2,0,2,3};
        else if(positive(0,1,3) && positive(1,2,3))indices={0,1,3,1,2,3};
        else return false;
        if(output.Vertices.size()>UINT32_MAX-points.size())return false;
        section.FirstVertex=output.Vertices.size();section.VertexCount=points.size();section.FirstIndex=output.Indices.size();
        const auto base=static_cast<uint32_t>(section.FirstVertex);
        output.Vertices.insert(output.Vertices.end(),points.begin(),points.end());
        for(auto index:indices)output.Indices.push_back(base+index);
        section.IndexCount=output.Indices.size()-section.FirstIndex;output.Sections.push_back(section);return true;
    }
    struct NumericalInputs
    {
        uint8_t ForwardAxis=0;
        P::RelativeTransform Transform;
        P::CrossSection CrossSection;
        V::Transform Component;
    };
    inline NumericalInputs Inputs(const P::Profile& profile)
    {return {profile.ForwardAxis,profile.Transform,profile.Section,{}};}
    inline bool ComponentTransform(const P::RelativeTransform& source,V::Transform& target)
    {
        if(!V::Finite(source.Translation) || !V::Finite(source.RotationDegrees) || !V::Finite(source.Scale))return false;
        // FRotator pitch/yaw/roll -> quaternion: yaw about +Z, pitch about -Y,
        // roll about -X. Apply signed scale, rotation, then relative translation.
        constexpr double halfRadians=3.14159265358979323846/360;
        const double p=std::remainder(source.RotationDegrees[0],360)*halfRadians;
        const double y=std::remainder(source.RotationDegrees[1],360)*halfRadians;
        const double r=std::remainder(source.RotationDegrees[2],360)*halfRadians;
        const double sp=std::sin(p),cp=std::cos(p),sy=std::sin(y),cy=std::cos(y),sr=std::sin(r),cr=std::cos(r);
        target={source.Translation,source.Scale,{cr*sp*sy-sr*cp*cy,-cr*sp*cy-sr*cp*sy,
            cr*cp*sy-sr*sp*cy,cr*cp*cy+sr*sp*sy}};
        V::Point check;return Inverse(target,{},check,true);
    }
    inline uint32_t Eligibility(const P::Profile& profile,NumericalInputs& inputs)
    {
        const auto& cross=inputs.CrossSection;
        for(const auto* pair:{&cross.StartScale,&cross.EndScale,&cross.StartOffset,&cross.EndOffset})
            for(double value:*pair)if(!std::isfinite(value))return InvalidSource;
        if(!V::Finite(cross.Up) || !std::isfinite(cross.StartRoll) || !std::isfinite(cross.EndRoll) ||
            !ComponentTransform(inputs.Transform,inputs.Component))return InvalidSource;
        if(!P::Valid(profile.LocalBounds))return InvalidSource;
        if(inputs.ForwardAxis>2)return InvalidSource;
        // The implemented projector extrudes a fixed, unrolled X-forward Y/Z
        // rectangle. Other axes/roll conventions and per-native-section varying
        // parameters still need mathematics, not construction certification.
        if(inputs.ForwardAxis!=0 || cross.StartRoll!=0 || cross.EndRoll!=0 ||
            cross.StartOffset!=cross.EndOffset || cross.StartScale!=cross.EndScale)return UnsupportedProjection;
        V::Point up=cross.Up;if(!Normalize(up))return InvalidSource;
        return None;
    }
    inline uint32_t Eligibility(const P::Profile& profile)
    {auto inputs=Inputs(profile);return Eligibility(profile,inputs);}
    inline bool FrameValid(const V::Transform& root,const V::Transform& component,const V::Point& up,
        const std::array<C::Point,4>& controls)
    {
        // A derivative cone test only, not another sampler. Exclude interior
        // zero tangents and tangents parallel to the supplied up direction.
        V::Point reference=up;if(!Normalize(reference))return false;
        std::array<Interval,3> cone;
        for(auto& interval:cone)interval={INFINITY,-INFINITY};
        for(size_t i=0;i<3;++i)
        {
            V::Point tangent,local;
            for(size_t axis=0;axis<3;++axis)tangent[axis]=controls[i+1][axis]-controls[i][axis];
            if(!Inverse(root,tangent,local,true) || !Inverse(component,local,local,true))return false;
            const auto normal=Cross(reference,local);
            if(!V::Finite(normal))return false;
            for(size_t axis=0;axis<3;++axis)
            {cone[axis].Low=(std::min)(cone[axis].Low,normal[axis]);cone[axis].High=(std::max)(cone[axis].High,normal[axis]);}
        }
        const double lower=std::hypot(MinimumMagnitude(cone[0]),MinimumMagnitude(cone[1]),MinimumMagnitude(cone[2]));
        return std::isfinite(lower) && lower>0 && std::isfinite(1/lower);
    }
    inline Result Prepare(const C::Record& record,const S::PreparedCurve* sampled)
    {
        Result result;result.Input=record;result.Generation=record.Generation;
        if(!record.HasSpline){result.Status=State::Unsupported;result.Reasons=InvalidSource;return result;}
        if(!record.Curve || !sampled){result.Reasons=MissingCurve;return result;}
        const auto metadata=Metadata(record);
        if(!metadata){result.Reasons=MissingMetadata;return result;}
        if(metadata->Profiles.empty()) {result.Reasons=MissingMetadata;return result;}
        const V::Transform root{record.Position,record.Scale,record.Rotation};V::Point inverseCheck;
        if(!record.TransformValid || !Inverse(root,record.Position,inverseCheck,false))
        {result.Reasons=InvalidRoot;result.Status=State::Invalid;return result;}
        if(record.Curve->Issue!=C::GeometryIssue::None || sampled->SourceIssue!=C::GeometryIssue::None ||
            sampled->PreparationIssue!=C::GeometryIssue::None || sampled->Sections.size()!=record.Curve->Segments.size())
        {result.Reasons=InvalidSource;result.Status=State::Invalid;return result;}
        bool geometry=false,invalid=false,pending=false;
        for(size_t i=0;i<metadata->Profiles.size();++i)
        {
            const auto& input=metadata->Profiles[i];Profile profile;
            profile.SourceProfile=i;
            auto inputs=Inputs(input);profile.Reasons=Eligibility(input,inputs);
            if(profile.Reasons)
            {
                profile.Status=State::Unsupported;result.Reasons|=profile.Reasons;
                result.Profiles.push_back(std::move(profile));continue;
            }
            profile.Reasons=NativeSectionsUnknown|JoinsAndCapsUnknown|RibbonApproximation;
            if(!sampled->Complete())profile.Reasons|=SamplingLimited;
            for(size_t segment=0;segment<sampled->Sections.size();++segment)
            {
                const auto& section=sampled->Sections[segment];const auto& source=record.Curve->Segments[segment];
                if(section.Discontinuous || source.Mode==S::Constant){profile.Reasons|=Discontinuity;continue;}
                if(section.First>sampled->Samples.size() || section.Count>sampled->Samples.size()-section.First || section.Count<2)
                {profile.Reasons|=InvalidSource;continue;}
                for(size_t j=1;j<section.Count;++j)
                {
                    const auto& a=sampled->Samples[section.First+j-1];const auto& b=sampled->Samples[section.First+j];
                    if(a.T>=b.T || a.T<0 || b.T>1){profile.Reasons|=InvalidSource;continue;}
                    S::Evaluation start,end;std::array<XY,4> first,last;Section cell;cell.SourceSegment=segment;cell.StartT=a.T;cell.EndT=b.T;
                    if(!S::Evaluate(source,a.T,start) || !S::Evaluate(source,b.T,end) ||
                        !Project(root,start,input.LocalBounds,inputs.CrossSection,first,cell.Start,inputs.Component) ||
                        !Project(root,end,input.LocalBounds,inputs.CrossSection,last,cell.End,inputs.Component))
                    {profile.Reasons|=DegenerateFrame;continue;}
                    if(source.Mode!=S::Linear)
                    {
                        // Analysis only: do not bridge a cell whose interior
                        // frame cannot exclude a zero/up-parallel tangent.
                        // Cone bounds never supply drawable vertices.
                        if(!FrameValid(root,inputs.Component,inputs.CrossSection.Up,Subcurve(source,a.T,b.T)))
                        {profile.Reasons|=DegenerateFrame;continue;}
                    }
                    if(!AppendRibbon(cell,profile))profile.Reasons|=DegenerateProjection;
                }
            }
            profile.Status=profile.Indices.empty()?State::Unsupported:State::Partial;
            geometry=geometry || !profile.Indices.empty();result.Reasons|=profile.Reasons;
            result.Profiles.push_back(std::move(profile));
        }
        result.Status=geometry?State::Partial:pending?State::Pending:invalid?State::Invalid:State::Unsupported;
        return result;
    }
    inline constexpr double ThinHalfWidth=5.0; // 10 cm full world width; bump NumericalPolicy if changed.
    inline bool ThinIdentity(const C::Record& record)
    {
        return record.Generation!=0 && record.Index>=0 && record.Serial!=0 && record.HasSpline && record.DefinitionValid &&
            record.TransformValid &&
            V::Valid(V::Transform{record.Position,record.Scale,record.Rotation});
    }
    // Shared synthetic thin-profile facility, available to either thin category.
    // Dispatch is owned by the authoritative BuildingID policy in BuildingVisuals.
    // Native Zipline preparation remains unchanged until its numerical width is equivalent.
    inline Result PrepareThin(const C::Record& record,const S::PreparedCurve* sampled)
    {
        Result result;result.Input=record;result.Generation=record.Generation;
        result.SyntheticThin=true;
        auto reject=[&](State state,uint32_t reason)
        {result.Status=state;result.Reasons=reason;result.Profiles.clear();return result;};
        if(!ThinIdentity(record))return reject(State::Invalid,InvalidSource);
        if(!record.Curve || !sampled)return reject(State::Pending,MissingCurve);
        if(record.Curve->Issue!=C::GeometryIssue::None || sampled->SourceIssue!=C::GeometryIssue::None ||
            sampled->PreparationIssue!=C::GeometryIssue::None || sampled->Sections.size()!=record.Curve->Segments.size())
            return reject(State::Invalid,InvalidSource);
        if(!sampled->Complete())return reject(State::Unsupported,SamplingLimited);
        Profile profile;profile.SyntheticThin=true;
        profile.Reasons=RibbonApproximation;
        // Numerical cross-section values only.
        const P::Bounds bounds{{0,0,0},{0,ThinHalfWidth,0},ThinHalfWidth};
        const P::CrossSection cross{{1,1},{1,1},{0,0},{0,0},0,0,{0,0,1}};
        for(size_t segment=0;segment<sampled->Sections.size();++segment)
        {
            const auto& section=sampled->Sections[segment];const auto& source=record.Curve->Segments[segment];
            if(section.Discontinuous || source.Mode==S::Constant){profile.Reasons|=Discontinuity;continue;}
            if(section.First>sampled->Samples.size() || section.Count>sampled->Samples.size()-section.First || section.Count<2)
                return reject(State::Invalid,InvalidSource);
            for(size_t j=1;j<section.Count;++j)
            {
                const auto& a=sampled->Samples[section.First+j-1];const auto& b=sampled->Samples[section.First+j];
                if(a.T>=b.T || a.T<0 || b.T>1)return reject(State::Invalid,InvalidSource);
                S::Evaluation start,end;std::array<XY,4> first,last;
                Section cell;cell.SourceSegment=segment;cell.StartT=a.T;cell.EndT=b.T;
                if(!S::Evaluate(source,a.T,start) || !S::Evaluate(source,b.T,end))return reject(State::Invalid,InvalidSource);
                // Controls already are world-space. Identity root/component
                // projects the synthetic cross-section without reapplying TRS.
                if(!Project({},start,bounds,cross,first,cell.Start) || !Project({},end,bounds,cross,last,cell.End) ||
                    (source.Mode!=S::Linear && !FrameValid({}, {},cross.Up,Subcurve(source,a.T,b.T))))
                {profile.Reasons|=DegenerateFrame;continue;}
                if(!AppendRibbon(cell,profile))profile.Reasons|=DegenerateProjection;
            }
        }
        if(profile.Indices.empty())return reject(State::Unsupported,profile.Reasons|DegenerateProjection);
        profile.Status=State::Partial;result.Status=State::Partial;result.Reasons=profile.Reasons;
        result.Profiles.push_back(std::move(profile));return result;
    }
    inline std::shared_ptr<const Result> PrepareOwned(const C::Record& record,std::shared_ptr<const S::PreparedCurve> sampled,
        std::shared_ptr<const Result> previous={},bool thin=false)
    {
        if(previous && NumericalEquivalent(record,sampled.get(),*previous,thin))return previous;
        auto result=thin?PrepareThin(record,sampled.get()):Prepare(record,sampled.get());result.Sampling=std::move(sampled);
        return std::make_shared<const Result>(std::move(result));
    }
}
#endif
