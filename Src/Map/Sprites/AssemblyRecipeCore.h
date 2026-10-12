#pragma once
#include "RepresentationMetadataCore.h"
#include <array>
#include <map>
#include <set>

namespace MiniMapAssemblyRecipe
{
    namespace C=MiniMapRepresentationCore;
    inline constexpr size_t StaticLimit=16,NodeLimit=32,DepthLimit=16,SkeletalLimit=16,VisitLimit=4096,OutputLimit=256*1024;
    using V=std::array<double,3>;
    using Q=std::array<double,4>;
    struct Transform { V T{0,0,0};Q R{0,0,0,1};V S{1,1,1}; };
    template<size_t N> bool Finite(const std::array<double,N>& a)
    {return std::all_of(a.begin(),a.end(),[](double v){return std::isfinite(v);});}
    inline bool Normalize(Q& q)
    {
        if(!Finite(q))return false;
        double n=0;for(double v:q)n+=v*v;
        if(!std::isfinite(n)||n<1e-24)return false;
        n=std::sqrt(n);for(double& v:q)v/=n;return true;
    }
    inline Q Multiply(const Q& a,const Q& b)
    {return {a[3]*b[0]+b[3]*a[0]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]+b[3]*a[1]+a[2]*b[0]-a[0]*b[2],
        a[3]*b[2]+b[3]*a[2]+a[0]*b[1]-a[1]*b[0],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
    inline Q FromRotator(V p)
    {
        // UE FRotator3d::Quaternion, not a right-handed Euler library.
        constexpr double factor=3.14159265358979323846/360;
        for(double& v:p)v=std::remainder(v,360.0)*factor;
        const double sp=std::sin(p[0]),cp=std::cos(p[0]),sy=std::sin(p[1]),cy=std::cos(p[1]),sr=std::sin(p[2]),cr=std::cos(p[2]);
        Q q{cr*sp*sy-sr*cp*cy,-cr*sp*cy-sr*cp*sy,cr*cp*sy-sr*sp*cy,cr*cp*cy+sr*sp*sy};return q;
    }
    inline bool Valid(Transform t)
    {return Finite(t.T)&&Finite(t.S)&&Normalize(t.R)&&std::all_of(t.S.begin(),t.S.end(),[](double v){return v>1e-8;});}
    inline bool Compose(Transform local,Transform parent,Transform& out)
    {
        if(!Valid(local)||!Valid(parent)||!Normalize(local.R)||!Normalize(parent.R))return false;
        Q point{local.T[0]*parent.S[0],local.T[1]*parent.S[1],local.T[2]*parent.S[2],0};
        Q inverse{-parent.R[0],-parent.R[1],-parent.R[2],parent.R[3]};const auto rotated=Multiply(Multiply(parent.R,point),inverse);
        out.R=Multiply(parent.R,local.R);
        for(size_t i=0;i<3;++i){out.T[i]=rotated[i]+parent.T[i];out.S[i]=local.S[i]*parent.S[i];}
        return Normalize(out.R)&&Valid(out);
    }
    template<size_t N> std::string Numbers(const std::array<double,N>& a)
    {C::JsonArray j;for(double v:a)j.Add(C::Number(v));return j.String();}
    inline std::string Json(const Transform& t)
    {return C::Object({{"translation",Numbers(t.T)},{"rotation_xyzw",Numbers(t.R)},{"scale",Numbers(t.S)}});}
    struct Name {int32_t Index{};uint32_t Number{};bool operator==(const Name&)const=default;};
    struct NamedCandidate {std::string Id;Name Value;};
    inline std::string Match(Name name,const std::vector<NamedCandidate>& candidates,bool& ambiguous)
    {
        std::set<std::string> ids;for(const auto& c:candidates)if(c.Value==name)ids.insert(c.Id);
        ambiguous=ids.size()>1;return ids.size()==1?*ids.begin():"";
    }
    struct Node
    {
        std::string Id,Component,Parent,NativeParent,Mesh,Kind,Metadata="{}";
        Transform Local;
        bool Scene=false,InputsValid=false,Absolute=false,Socket=false,ParentConflict=false,TemplateAttachment=false;
    };
    struct Result {Transform Effective;std::string Status="unresolved",Reason="missing_parent";};
    inline Result Resolve(const std::string& id,const std::map<std::string,Node>& nodes,
        const std::map<std::string,Result>& native,std::set<std::string>& active,size_t depth=0,size_t* remaining=nullptr)
    {
        Result r;
        if(remaining){if(!*remaining){r.Status="truncated";r.Reason="graph_work_limit";return r;}--*remaining;}
        if(depth>=DepthLimit){r.Status="truncated";r.Reason="parent_depth_limit";return r;}
        if(!active.insert(id).second){r.Status="invalid";r.Reason="parent_cycle";return r;}
        struct Leave {std::set<std::string>& S;std::string Id;~Leave(){S.erase(Id);}} leave{active,id};
        const auto found=nodes.find(id);if(found==nodes.end())return r;
        const auto& n=found->second;
        if(n.ParentConflict){r.Status="invalid";r.Reason="ambiguous_scs_parent";return r;}
        if(n.TemplateAttachment){r.Status="unsupported";r.Reason="template_attachment_not_reconciled_with_scs";return r;}
        if(n.Absolute||n.Socket){r.Status="unsupported";r.Reason=n.Absolute?"absolute_transform_requires_world_context":"socket_transform_not_verified";return r;}
        if(n.Scene&&(!n.InputsValid||!Valid(n.Local))){r.Status="invalid";r.Reason="nonfinite_negative_or_degenerate_transform";return r;}
        Result parent;
        if(!n.Parent.empty())parent=Resolve(n.Parent,nodes,native,active,depth+1,remaining);
        else if(!n.NativeParent.empty())
        {const auto p=native.find(n.NativeParent);if(p!=native.end())parent=p->second;}
        if(parent.Status!="verified")return parent;
        r=parent;
        if(n.Scene&&!Compose(n.Local,parent.Effective,r.Effective)){r.Status="invalid";r.Reason="composition_failed";}
        return r;
    }
    struct Slot {size_t Index{};std::string Material;bool Valid=false;};
    inline std::vector<Slot> ProposedMaterials(const std::vector<Slot>& defaults,const std::vector<Slot>& overrides)
    {
        std::map<size_t,Slot> slots;for(const auto& s:defaults)slots[s.Index]=s;
        for(const auto& s:overrides)if(s.Valid&&!s.Material.empty())slots[s.Index]=s;
        std::vector<Slot> out;for(const auto& [i,s]:slots){(void)i;out.push_back(s);}return out;
    }
    inline bool CaptureReady(bool transforms,bool materials,bool resident,bool cleanup,bool runtimeAdjustments,bool truncated)
    {return transforms&&materials&&resident&&cleanup&&runtimeAdjustments&&!truncated;}
    inline std::string Bound(std::string document)
    {
        if(document.size()<=OutputLimit)return document;
        return "{\"schema_version\":1,\"stage\":\"R2.5a\",\"status\":\"truncated\",\"truncation\":{\"output_byte_limit\":true},\"readiness\":{\"static_only_diagnostic_capture\":false,\"complete_visual_building\":false,\"blocking_reasons\":[\"output_byte_limit\"]}}";
    }
}
