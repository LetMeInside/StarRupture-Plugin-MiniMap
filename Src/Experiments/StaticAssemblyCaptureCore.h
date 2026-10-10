#pragma once
#include "AssemblyRecipeCore.h"

namespace MiniMapStaticAssembly
{
    namespace A=MiniMapAssemblyRecipe;
    constexpr size_t Limit=16,Expected=10,UniqueExpected=7,Slots=32,Assignments=256;
    struct Bounds { A::V Origin{},Extent{}; };
    struct Instance
    {
        uintptr_t Mesh{};
        uint32_t ComponentIndex{};
        A::Transform Transform;
        Bounds LocalBounds;
        std::array<uintptr_t,Slots> Materials{};
        std::array<bool,Slots> Overrides{};
        size_t MaterialCount{};
        bool Verified=false,Visible=false,Hidden=true;
    };
    struct Recipe
    {
        std::array<Instance,Limit> Instances{};
        size_t Count{};
        uintptr_t ActorClass{};
        uint64_t Generation{};
        bool RootVerified=false,Complete=false,HighDetail=false;
    };
    struct Footprint { A::V Min{},Max{};bool Initialized=false; };
    inline bool Add(Footprint& f,const Instance& i)
    {
        if(!A::Valid(i.Transform)||!A::Finite(i.LocalBounds.Origin)||!A::Finite(i.LocalBounds.Extent))return false;
        for(double e:i.LocalBounds.Extent)if(e<=0||e>10000)return false;
        auto q=i.Transform.R;if(!A::Normalize(q))return false;
        const A::Q inv{-q[0],-q[1],-q[2],q[3]};
        for(size_t corner=0;corner<8;++corner)
        {
            A::Q p{};for(size_t a=0;a<3;++a)p[a]=(i.LocalBounds.Origin[a]+((corner&(1<<a))?1:-1)*i.LocalBounds.Extent[a])*i.Transform.S[a];
            const auto rotated=A::Multiply(A::Multiply(q,p),inv);
            A::V v{};for(size_t a=0;a<3;++a)v[a]=rotated[a]+i.Transform.T[a];
            if(!A::Finite(v))return false;
            if(!f.Initialized){f.Min=f.Max=v;f.Initialized=true;}
            else for(size_t a=0;a<3;++a){f.Min[a]=(std::min)(f.Min[a],v[a]);f.Max[a]=(std::max)(f.Max[a],v[a]);}
        }
        return true;
    }
    inline const char* Validate(const Recipe& r,Footprint& f)
    {
        f={};
        if(!r.ActorClass||!r.RootVerified||!r.Complete||!r.HighDetail)return "unverified_root_or_upstream";
        if(r.Count!=Expected)return "static_instance_count";
        std::set<uintptr_t> meshes;std::set<uint32_t> components;size_t assigned=0;
        for(size_t n=0;n<r.Count;++n)
        {
            const auto& i=r.Instances[n];
            if(!i.Mesh||!components.insert(i.ComponentIndex).second)return "missing_mesh_or_duplicate_instance";
            meshes.insert(i.Mesh);
            if(!i.Verified||!i.Visible||i.Hidden||!A::Valid(i.Transform))return "unsupported_transform_or_visibility";
            if(!i.MaterialCount||i.MaterialCount>Slots||i.MaterialCount>Assignments-assigned)return "material_count";
            assigned+=i.MaterialCount;
            for(size_t s=0;s<i.MaterialCount;++s)if(!i.Materials[s])return "missing_material";
            if(!Add(f,i))return "invalid_mesh_bounds";
        }
        if(meshes.size()!=UniqueExpected)return "unique_mesh_count";
        for(size_t a=0;a<3;++a)if(!std::isfinite(f.Max[a]-f.Min[a])||f.Max[a]<=f.Min[a])return "invalid_aggregate_bounds";
        return nullptr;
    }
    inline double OrthoWidth(const Footprint& f)
    {return (std::max)(f.Max[0]-f.Min[0],f.Max[1]-f.Min[1])*1.1;}
    inline bool Equivalent(A::Transform a,A::Transform b)
    {
        if(!A::Valid(a)||!A::Valid(b)||!A::Normalize(a.R)||!A::Normalize(b.R))return false;
        double dot=0;for(size_t n=0;n<4;++n)dot+=a.R[n]*b.R[n];
        if(std::abs(std::abs(dot)-1)>1e-8)return false;
        for(size_t n=0;n<3;++n)if(std::abs(a.T[n]-b.T[n])>1e-7||std::abs(a.S[n]-b.S[n])>1e-8)return false;
        return true;
    }
    inline bool CanCleanBeforeCommit(bool associationUncertain,bool registered,bool currentWorld)
    {return !associationUncertain&&(!registered||currentWorld);}
    // One bounded ledger; cleanup callbacks execute only after the external
    // capture/readback owner has granted retirement. Never owns native memory.
    struct Ledger
    {
        struct Entry {uintptr_t Component{};bool Registered=false,Member=false;};
        std::array<Entry,Limit> Entries{};size_t Count{};
        bool Complete() const
        {
            if(Count!=Expected)return false;std::set<uintptr_t> unique;
            for(size_t i=0;i<Count;++i)if(!Entries[i].Component||!Entries[i].Registered||!Entries[i].Member||!unique.insert(Entries[i].Component).second)return false;
            return true;
        }
        bool Detached() const {for(size_t i=0;i<Count;++i)if(Entries[i].Registered)return false;return true;}
        template<class Fn> void Detach(Fn fn)
        {for(size_t i=Count;i>0;--i){auto& e=Entries[i-1];if(e.Registered){fn(e.Component);e.Registered=false;}}}
        template<class Fn> bool Release(bool retired,Fn fn)
        {
            if(!retired||!Detached())return false;
            for(size_t i=Count;i>0;--i){auto& e=Entries[i-1];if(e.Component){fn(e.Component);e={};}}
            Count=0;return true;
        }
    };
}
