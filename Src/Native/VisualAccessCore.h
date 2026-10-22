#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <vector>
#include <optional>

namespace MiniMapVisualAccess
{
    constexpr size_t MaximumComponents=256, MaximumMaterials=64;
    using Point=std::array<double,3>;
    struct Identity
    {
        int32_t Index=-1,Serial=0;
        bool operator==(const Identity&) const=default;
    };
    struct Token
    {
        uint64_t Generation=0;
        Identity Entity;
        bool operator==(const Token&) const=default;
    };
    enum class Status : uint8_t
    {
        NotObserved, Ready, Unavailable, WrongThread, WorldChanged, StaleEntity,
        UnbuiltEntity, RepresentationUnavailable, NoActorRepresentation,
        UnsupportedRepresentation, ActorUnavailable, AssociationChanged,
        IncompleteActor, EnumerationLimit, EnumerationIncomplete,
        InvalidComponent, InvalidTransform, Interrupted
    };
    struct Transform
    {
        Point Translation={},Scale={1,1,1};
        std::array<double,4> Rotation={0,0,0,1};
    };
    inline bool Finite(const Point& p)
    {return std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2]);}
    inline bool Valid(const Transform& t)
    {
        double norm=0;for(double q:t.Rotation){if(!std::isfinite(q))return false;norm+=q*q;}
        return Finite(t.Translation)&&Finite(t.Scale)&&std::isfinite(norm)&&std::abs(norm-1)<=1e-6;
    }
    // Native component-local -> world only. Never apply this to a Mass copied
    // curve without a separate alignment contract. Vectors omit translation.
    inline bool ToWorld(const Transform& t,const Point& local,Point& out,bool vector=false)
    {
        if(!Valid(t)||!Finite(local))return false;
        Point v;for(size_t i=0;i<3;++i)v[i]=local[i]*t.Scale[i];
        const auto& q=t.Rotation;
        const Point c{q[1]*v[2]-q[2]*v[1],q[2]*v[0]-q[0]*v[2],q[0]*v[1]-q[1]*v[0]};
        const Point d{q[1]*c[2]-q[2]*c[1],q[2]*c[0]-q[0]*c[2],q[0]*c[1]-q[1]*c[0]};
        Point result;for(size_t i=0;i<3;++i)result[i]=v[i]+2*(q[3]*c[i]+d[i])+(vector?0:t.Translation[i]);
        if(!Finite(result))return false;out=result;return true;
    }
    struct SplineMeshParameters
    {
        Point Start={},End={},StartTangent={},EndTangent={},Up={};
        std::array<double,2> StartScale={},EndScale={},StartOffset={},EndOffset={};
        double StartRoll=0,EndRoll=0,BoundaryMin=0,BoundaryMax=0,NaniteBoundsScale=0;
        uint8_t ForwardAxis=0;
        bool SmoothRollScale=false,MeshDirty=false;
    };
    inline bool Valid(const SplineMeshParameters& p)
    {
        if(!Finite(p.Start)||!Finite(p.End)||!Finite(p.StartTangent)||!Finite(p.EndTangent)||!Finite(p.Up)||p.ForwardAxis>2)return false;
        for(const auto* pair:{&p.StartScale,&p.EndScale,&p.StartOffset,&p.EndOffset})
            for(double v:*pair)if(!std::isfinite(v))return false;
        return std::isfinite(p.StartRoll)&&std::isfinite(p.EndRoll)&&std::isfinite(p.BoundaryMin)&&std::isfinite(p.BoundaryMax)&&std::isfinite(p.NaniteBoundsScale);
    }
    enum class Kind : uint8_t { Other, Spline, SplineMesh, StaticMesh, InstancedStaticMesh, UnsupportedPrimitive, UnsupportedScene };
    struct MaterialSlot
    {
        Identity MeshDefault,Override,Effective;
    };
    struct Component
    {
        Identity Object,Class,Mesh;
        Kind Type=Kind::Other;
        Transform ComponentToWorld;
        std::optional<SplineMeshParameters> SplineMesh;
        std::vector<MaterialSlot> Materials;
        bool Registered=false,Visible=false,HiddenInGame=false;
        // Enumeration/frame/identity access is separate from complete visual
        // geometry: spline curves, ISM instances and shader state need adapters.
    };
    struct Observation
    {
        Token Source;
        Identity World,Actor;
        Status State=Status::NotObserved;
        uint8_t Representation=0xff;
        bool ActorHiddenInGame=false;
        bool EnumerationComplete=false;
        std::vector<Component> Components;
    };
    // Same orchestration is used by the native adapter and permanent fixture
    // tests. Ops owns synchronous borrowed native data; only values escape.
    template<class Ops> Observation Acquire(Ops& ops,const Token& token)
    {
        Observation out;out.Source=token;
        auto fail=[&](Status s){out.State=s;out.Components.clear();out.EnumerationComplete=false;return out;};
        auto status=ops.Admit(token);if(status!=Status::Ready)return fail(status);
        status=ops.Associate(out);if(status!=Status::Ready)return fail(status);
        size_t count=0;status=ops.Enumerate(count);if(status!=Status::Ready)return fail(status);
        if(count>MaximumComponents)return fail(Status::EnumerationLimit);
        out.Components.reserve(count);
        for(size_t i=0;i<count;++i)
        {
            Component component;status=ops.Copy(i,component);if(status!=Status::Ready)return fail(status);
            if(component.Type!=Kind::Other)
            {
                if(!Valid(component.ComponentToWorld))return fail(Status::InvalidTransform);
                if(component.SplineMesh && !Valid(*component.SplineMesh))return fail(Status::InvalidComponent);
                out.Components.push_back(std::move(component));
            }
        }
        status=ops.Admit(token);if(status!=Status::Ready)return fail(status);
        if(!ops.Unchanged(out))return fail(Status::AssociationChanged);
        status=ops.Admit(token);if(status!=Status::Ready)return fail(status);
        out.State=Status::Ready;out.EnumerationComplete=true;return out;
    }
}
