#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>
#include <span>
#include <stdexcept>
#include <limits>

#ifdef MINIMAP_OCCLUSION_BENCHMARK
#include <chrono>
#endif

// Standalone numerical planner; no renderer or runtime dependencies.
namespace MiniMapBuildingSplineOcclusion
{
    using Point = std::array<double, 3>;
    using XY = std::array<double, 2>;
    struct Triangle { std::array<Point, 3> Vertices{}; uint64_t Key = 0; };
    struct Occluder
    {
        std::array<XY, 4> Corners{}; // Perimeter order, either winding; strictly convex.
        double TopZ = 0;
        uint64_t Key = 0; // Numerical ordering only, not a native object identity.
        bool Active = true;
    };
    struct Input { std::vector<Triangle> Triangles; std::vector<Occluder> Occluders; };
    struct AABB { XY Min{},Max{}; };
    struct Removal
    {
        uint64_t OccluderKey=0,TriangleKey=0;
        double TopZ=0;
        long double Area=0;
        AABB HiddenBounds{}; // Bounds of the ACTUAL discarded polygon, not an AABB intersection.
        Point Witness{}; // Vertex average of that convex polygon, inside its hidden XY coverage.
    };
    struct RemovalDiagnostics
    {
        size_t Polygons=0;
        long double Area=0;
        std::vector<Removal> Examples;
        bool AllocationFailed=false; // Evidence failure never changes the clipping result.
    };
    enum class Status { Complete, InvalidInput, NumericalFailure, AllocationFailure };
    struct Result
    {
        Status State = Status::Complete;
        std::vector<Triangle> Visible; // Empty on ANY failure, including a later source triangle.
        RemovalDiagnostics Removed;
    };

#ifdef MINIMAP_OCCLUSION_BENCHMARK
    // Standalone-only phase measurement; absent from Client/Server builds.
    struct BenchmarkPhases { double QueryMs=0,SubtractionMs=0; };
    inline thread_local BenchmarkPhases MeasuredPhases;
    struct BenchmarkTimer
    {
        double& Elapsed;
        std::chrono::steady_clock::time_point Begin=std::chrono::steady_clock::now();
        ~BenchmarkTimer(){Elapsed+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-Begin).count();}
    };
#endif
    namespace Detail
    {
        // Binary64 storage; widened intermediates where supported (MSVC long
        // double remains binary64). No coordinate quantization, snapping, area
        // epsilon, height bias, or visual gap. Exact zero is a closed boundary.
        // Determinism is for a fixed floating-point implementation, not across
        // compilers. Compile without fast-math/reassociation.
        inline long double Turn(const XY& a, const XY& b, const XY& c)
        { return (static_cast<long double>(b[0])-a[0])*(static_cast<long double>(c[1])-a[1])-
                 (static_cast<long double>(b[1])-a[1])*(static_cast<long double>(c[0])-a[0]); }
        inline XY Position(const Point& p) { return {p[0], p[1]}; }
        inline long double Area(const Point& a, const Point& b, const Point& c)
        { return Turn(Position(a), Position(b), Position(c)); }
        inline void Require(bool value, Status failure) { if (!value) throw failure; }
        inline long double Checked(long double value)
        { Require(std::isfinite(value), Status::NumericalFailure); return value; }
        struct Edge
        {
            Point A{}, B{};
            bool operator==(const Edge& other) const { return A==other.A && B==other.B; }
        };
        inline Edge MakeEdge(Point a, Point b) { if(b<a)std::swap(a,b); return {a,b}; }
        struct Vertex { Point P{}; std::array<Edge,2> Supports{}; size_t Count=0; };
        using Polygon = std::vector<Vertex>;
        struct Plane
        {
            XY A{}, B{};
            double Top=0;
            bool Height=false;
            long double Distance(const Point& p) const
            { return Checked(Height ? static_cast<long double>(p[2])-Top : -Turn(A,B,Position(p))); }
        };
        inline Vertex Intersection(Vertex a, Vertex b, const Plane& plane)
        {
            // Original-edge provenance is carried through every split. Adjacent
            // triangles intersect the SAME original endpoints, even if previous
            // subtractions partitioned their shared edge differently. Canonical
            // endpoint order also makes reverse edge traversal bit-consistent.
            Vertex result;
            for(size_t i=0;i<a.Count;++i)for(size_t j=0;j<b.Count;++j)
                if(a.Supports[i]==b.Supports[j] && result.Count<2)
                    result.Supports[result.Count++]=a.Supports[i];
            Point first=a.P,last=b.P;
            if(result.Count) { first=result.Supports[0].A;last=result.Supports[0].B; }
            if(last<first)std::swap(first,last);
            const auto da=plane.Distance(first),db=plane.Distance(last);
            if(da==0)result.P=first;
            else if(db==0)result.P=last;
            else
            {
                const auto denominator=Checked(da-db);
                Require(denominator!=0,Status::NumericalFailure);
                const auto t=Checked(da/denominator);
                Require(t>=0 && t<=1,Status::NumericalFailure);
                for(size_t axis=0;axis<3;++axis)
                {
                    result.P[axis]=static_cast<double>(Checked((1-t)*first[axis]+t*last[axis]));
                    Require(std::isfinite(result.P[axis]),Status::NumericalFailure);
                }
                if(plane.Height)result.P[2]=plane.Top; // Exact prescribed height, never a bias.
            }
            return result;
        }
        inline void Append(Polygon& polygon, const Vertex& vertex)
        { if(polygon.empty() || polygon.back().P!=vertex.P)polygon.push_back(vertex); }
        inline void Close(Polygon& polygon)
        { if(polygon.size()>1 && polygon.front().P==polygon.back().P)polygon.pop_back(); }
        inline void Split(const Polygon& input,const Plane& plane,Polygon& inside,Polygon& outside)
        {
            bool positive=false,negative=false;
            for(const auto& v:input) { const auto d=plane.Distance(v.P);positive|=d>0;negative|=d<0; }
            if(!positive) { inside=input;return; } // Including an all-equal height face: hidden.
            if(!negative) { outside=input;return; }
            auto previous=input.back();auto dp=plane.Distance(previous.P);
            for(const auto& current:input)
            {
                const auto dc=plane.Distance(current.P);
                if((dp<0 && dc>0) || (dp>0 && dc<0))
                { const auto cut=Intersection(previous,current,plane);Append(inside,cut);Append(outside,cut); }
                if(dc<=0)Append(inside,current);
                if(dc>=0)Append(outside,current);
                previous=current;dp=dc;
            }
            Close(inside);Close(outside);
        }
        inline bool HasArea(const Polygon& polygon)
        {
            if(polygon.size()<3)return false;
            bool nonzero=false;
            for(size_t i=1;i+1<polygon.size();++i)
            {
                const auto area=Checked(Area(polygon[0].P,polygon[i].P,polygon[i+1].P));
                Require(area>=0,Status::NumericalFailure);nonzero|=area>0;
            }
            return nonzero;
        }
        inline void Canonical(Triangle& triangle)
        {
            for(const auto& p:triangle.Vertices)for(double v:p)
                Require(std::isfinite(v),Status::InvalidInput);
            const auto area=Area(triangle.Vertices[0],triangle.Vertices[1],triangle.Vertices[2]);
            Require(std::isfinite(area) && area!=0,Status::InvalidInput);
            if(area<0)std::swap(triangle.Vertices[1],triangle.Vertices[2]);
            std::rotate(triangle.Vertices.begin(),std::min_element(triangle.Vertices.begin(),triangle.Vertices.end()),triangle.Vertices.end());
        }
        inline void Canonical(Occluder& occluder)
        {
            Require(std::isfinite(occluder.TopZ),Status::InvalidInput);
            for(const auto& p:occluder.Corners)for(double v:p)Require(std::isfinite(v),Status::InvalidInput);
            auto& c=occluder.Corners;
            const auto turn=Turn(c[0],c[1],c[2]);
            Require(std::isfinite(turn) && turn!=0,Status::InvalidInput);
            if(turn<0)std::reverse(c.begin(),c.end());
            for(size_t i=0;i<4;++i)for(size_t j=0;j<4;++j)
                if(j!=i && j!=(i+1)%4)
                {const auto d=Turn(c[i],c[(i+1)%4],c[j]);Require(std::isfinite(d) && d>0,Status::InvalidInput);}
            std::rotate(c.begin(),std::min_element(c.begin(),c.end()),c.end());
        }
    }

    template<class Points> inline AABB Bounds(const Points& points)
    {
        AABB bounds{{INFINITY,INFINITY},{-INFINITY,-INFINITY}};
        for(const auto& p:points)for(size_t axis=0;axis<2;++axis)
        {bounds.Min[axis]=(std::min)(bounds.Min[axis],p[axis]);bounds.Max[axis]=(std::max)(bounds.Max[axis],p[axis]);}
        return bounds;
    }
    inline bool Disjoint(const AABB& a,const AABB& b)
    {return a.Max[0]<b.Min[0] || b.Max[0]<a.Min[0] || a.Max[1]<b.Min[1] || b.Max[1]<a.Min[1];}
    inline size_t AddSize(size_t a,size_t b)
    {
        if(b>(std::numeric_limits<size_t>::max)()-a)throw std::length_error("occlusion size overflow");
        return a+b;
    }
    inline void ObserveRemoval(RemovalDiagnostics& diagnostic,const Detail::Polygon& hidden,
        const Occluder& occluder,uint64_t triangleKey) noexcept
    {
        // Called only for the positive-area remainder already validated by the
        // existing five clipping planes. No extra clipping, allocations, checked
        // arithmetic, or predicates that can affect geometry acceptance.
        Removal evidence;evidence.OccluderKey=occluder.Key;evidence.TriangleKey=triangleKey;evidence.TopZ=occluder.TopZ;
        evidence.HiddenBounds={{INFINITY,INFINITY},{-INFINITY,-INFINITY}};
        for(size_t i=1;i+1<hidden.size();++i)
            evidence.Area+=Detail::Area(hidden[0].P,hidden[i].P,hidden[i+1].P)/2;
        for(const auto& vertex:hidden)
        {
            for(size_t axis=0;axis<3;++axis)evidence.Witness[axis]+=vertex.P[axis]/double(hidden.size());
            for(size_t axis=0;axis<2;++axis)
            {
                evidence.HiddenBounds.Min[axis]=(std::min)(evidence.HiddenBounds.Min[axis],vertex.P[axis]);
                evidence.HiddenBounds.Max[axis]=(std::max)(evidence.HiddenBounds.Max[axis],vertex.P[axis]);
            }
        }
        if(diagnostic.Polygons!=(std::numeric_limits<size_t>::max)())++diagnostic.Polygons;
        diagnostic.Area+=evidence.Area;
        if(!diagnostic.AllocationFailed)
        {
            try{diagnostic.Examples.push_back(evidence);}
            catch(const std::bad_alloc&){diagnostic.AllocationFailed=true;}
            catch(const std::length_error&){diagnostic.AllocationFailed=true;}
        }
    }
    struct BoundedOccluder { Occluder Value; AABB Box; };
    // Balanced median BVH. Each canonical ordinal occurs in exactly one leaf.
    // Bounds use endpoint min/max only: no overflowing extents, centers or bins.
    class OccluderIndex
    {
        struct Node { AABB Box; size_t Begin=0,End=0,Left=0,Right=0; };
        std::vector<Node> Nodes;
        std::vector<size_t> Ordinals;
        size_t BuildRange(const std::vector<BoundedOccluder>& values,size_t begin,size_t end,size_t axis)
        {
            AABB box=values[Ordinals[begin]].Box;
            for(size_t i=begin+1;i<end;++i)for(size_t a=0;a<2;++a)
            {
                box.Min[a]=(std::min)(box.Min[a],values[Ordinals[i]].Box.Min[a]);
                box.Max[a]=(std::max)(box.Max[a],values[Ordinals[i]].Box.Max[a]);
            }
            const size_t node=Nodes.size();Nodes.push_back({box,begin,end,0,0});
            if(end-begin>8) // Leaf granularity only, never a population/work limit.
            {
                const size_t middle=begin+(end-begin)/2;
                std::nth_element(Ordinals.begin()+begin,Ordinals.begin()+middle,Ordinals.begin()+end,
                    [&](size_t a,size_t b)
                    {
                        const auto& x=values[a].Box;const auto& y=values[b].Box;
                        if(x.Min[axis]!=y.Min[axis])return x.Min[axis]<y.Min[axis];
                        if(x.Max[axis]!=y.Max[axis])return x.Max[axis]<y.Max[axis];
                        return a<b;
                    });
                const size_t left=BuildRange(values,begin,middle,1-axis);
                const size_t right=BuildRange(values,middle,end,1-axis);
                Nodes[node].Left=left;Nodes[node].Right=right;
            }
            return node;
        }
        void QueryRange(size_t node,const AABB& box,const std::vector<BoundedOccluder>& values,
            std::vector<size_t>& candidates) const
        {
            const auto& n=Nodes[node];
            if(Disjoint(box,n.Box))return;
            if(n.Left)
            {QueryRange(n.Left,box,values,candidates);QueryRange(n.Right,box,values,candidates);}
            else for(size_t i=n.Begin;i<n.End;++i)
                if(!Disjoint(box,values[Ordinals[i]].Box))candidates.push_back(Ordinals[i]);
        }
    public:
        void Build(const std::vector<BoundedOccluder>& values)
        {
            Nodes.clear();Ordinals.resize(values.size());
            for(size_t i=0;i<values.size();++i)Ordinals[i]=i;
            if(!values.empty())
            {
                Nodes.reserve(AddSize(values.size(),values.size())-1);
                BuildRange(values,0,values.size(),0);
            }
        }
        // Throws allocation/length failure to the planner transaction. Never
        // returns a partial successful candidate set or falls back to scanning.
        void Query(const AABB& box,const std::vector<BoundedOccluder>& values,std::vector<size_t>& candidates) const
        {
            candidates.clear();
            if(!Nodes.empty())QueryRange(0,box,values,candidates);
            std::sort(candidates.begin(),candidates.end());
        }
    };
    class PreparedOccluders
    {
        std::vector<BoundedOccluder> Values;
        OccluderIndex Index;
        Status State=Status::Complete;
        template<bool Indexed> friend PreparedOccluders PrepareOccludersImpl(std::span<const Occluder>);
    public:
        Status GetStatus() const {return State;}
        const std::vector<BoundedOccluder>& Items() const {return Values;}
        void Candidates(const AABB& box,std::vector<size_t>& output) const
        {Detail::Require(State==Status::Complete,State);Index.Query(box,Values,output);}
    };
    // Validate/canonicalize and calculate each unchanged quad's AABB once, then
    // share this owned numerical set across all section transactions.
    template<bool Indexed> inline PreparedOccluders PrepareOccludersImpl(std::span<const Occluder> input)
    {
        PreparedOccluders result;
        try
        {
            for(auto o:input)if(o.Active)
            {Detail::Canonical(o);result.Values.push_back({o,Bounds(o.Corners)});}
            std::sort(result.Values.begin(),result.Values.end(),[](const auto& a,const auto& b)
            {const auto& x=a.Value;const auto& y=b.Value;
             if(x.Key!=y.Key)return x.Key<y.Key;if(x.Corners!=y.Corners)return x.Corners<y.Corners;return x.TopZ<y.TopZ;});
            if constexpr(Indexed)result.Index.Build(result.Values);
        }
        catch(Status state){result.State=state;result.Values.clear();}
        catch(const std::bad_alloc&){result.State=Status::AllocationFailure;result.Values.clear();}
        catch(const std::length_error&){result.State=Status::AllocationFailure;result.Values.clear();}
        return result;
    }
    inline PreparedOccluders PrepareOccluders(std::span<const Occluder> input)
    {return PrepareOccludersImpl<true>(input);}
    // Input triangle interiors must not overlap. Shared edges must have
    // identical XYZ endpoints. No population, work or fragment quotas exist.
    template<bool Indexed> inline Result PlanImpl(std::span<const Triangle> input,const PreparedOccluders& occluders)
    {
        Result result;
        try
        {
            Detail::Require(occluders.GetStatus()==Status::Complete,occluders.GetStatus());
            std::vector<Triangle> triangles(input.begin(),input.end());
            for(auto& triangle:triangles)Detail::Canonical(triangle);
            std::sort(triangles.begin(),triangles.end(),[](const Triangle& a,const Triangle& b)
                {return a.Key!=b.Key?a.Key<b.Key:a.Vertices<b.Vertices;});
            std::vector<size_t> candidates;
            for(const auto& triangle:triangles)
            {
                const auto box=Bounds(triangle.Vertices);
                Detail::Polygon original;
                for(size_t i=0;i<3;++i)
                    original.push_back({triangle.Vertices[i],{Detail::MakeEdge(triangle.Vertices[i],triangle.Vertices[(i+1)%3]),
                        Detail::MakeEdge(triangle.Vertices[i],triangle.Vertices[(i+2)%3])},2});
                std::vector<Detail::Polygon> frontier{std::move(original)};
                {
#ifdef MINIMAP_OCCLUSION_BENCHMARK
                    BenchmarkTimer queryTimer{MeasuredPhases.QueryMs};
#endif
                    if constexpr(Indexed)occluders.Candidates(box,candidates);
                }
                const size_t count=Indexed?candidates.size():occluders.Items().size();
                for(size_t candidate=0;candidate<count;++candidate)
                {
                    const auto& prepared=occluders.Items()[Indexed?candidates[candidate]:candidate];
                    // Strict AABB separation only; boundary contact and every
                    // potentially intersecting quad still reach exact clipping.
                    if(Disjoint(box,prepared.Box))continue;
#ifdef MINIMAP_OCCLUSION_BENCHMARK
                    BenchmarkTimer subtractionTimer{MeasuredPhases.SubtractionMs};
#endif
                    const auto& occluder=prepared.Value;
                    std::vector<Detail::Polygon> next;
                    for(auto& existing:frontier)
                    {
                        auto remaining=std::move(existing);
                        for(size_t planeIndex=0;planeIndex<5 && !remaining.empty();++planeIndex)
                        {
                            Detail::Plane plane;
                            plane.Height=planeIndex==4;plane.Top=occluder.TopZ;
                            if(!plane.Height){plane.A=occluder.Corners[planeIndex];plane.B=occluder.Corners[(planeIndex+1)%4];}
                            Detail::Polygon inside,outside;
                            Detail::Split(remaining,plane,inside,outside);
                            if(Detail::HasArea(outside))next.push_back(std::move(outside));
                            remaining=Detail::HasArea(inside)?std::move(inside):Detail::Polygon{};
                        }
                        if(!remaining.empty())ObserveRemoval(result.Removed,remaining,occluder,triangle.Key);
                    }
                    frontier=std::move(next);
                    if(frontier.empty())break; // Proven full subtraction, not a work limit.
                }
                for(const auto& polygon:frontier)for(size_t i=1;i+1<polygon.size();++i)
                {
                    const auto area=Detail::Checked(Detail::Area(polygon[0].P,polygon[i].P,polygon[i+1].P));
                    Detail::Require(area>=0,Status::NumericalFailure);
                    if(area>0)result.Visible.push_back({{polygon[0].P,polygon[i].P,polygon[i+1].P},triangle.Key});
                }
            }
        }
        catch(Status state) {result.State=state;result.Visible.clear();result.Removed={};}
        catch(const std::bad_alloc&) {result.State=Status::AllocationFailure;result.Visible.clear();result.Removed={};}
        catch(const std::length_error&) {result.State=Status::AllocationFailure;result.Visible.clear();result.Removed={};}
        return result;
    }
    inline Result Plan(std::span<const Triangle> input,const PreparedOccluders& occluders)
    {return PlanImpl<true>(input,occluders);}
#ifdef MINIMAP_OCCLUSION_REFERENCE_TESTS
    // Standalone reference only. Same canonical preparation and unchanged
    // subtraction, with the original all-occluder scan and no index allocation.
    inline PreparedOccluders PrepareOccludersReference(std::span<const Occluder> input)
    {return PrepareOccludersImpl<false>(input);}
    inline Result PlanReference(std::span<const Triangle> input,const PreparedOccluders& occluders)
    {return PlanImpl<false>(input,occluders);}
    inline Result PlanReference(const Input& input)
    {return PlanReference(input.Triangles,PrepareOccludersReference(input.Occluders));}
#endif
    inline Result Plan(const Input& input)
    {return Plan(input.Triangles,PrepareOccluders(input.Occluders));}
}
