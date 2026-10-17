#pragma once
#include "AssemblyRecipeCore.h"

namespace MiniMapStaticAssembly
{
    namespace A=MiniMapAssemblyRecipe;
    constexpr size_t Limit=16,Expected=10,UniqueExpected=7,Slots=32,Assignments=256;
    enum class DrawableMembership { Drawable,Noncontributing,Unresolved };
    inline DrawableMembership AuthoredStaticMembership(bool exactStatic,bool visibilityKnown,bool visible,bool hidden)noexcept
    {
        if(!exactStatic||!visibilityKnown)return DrawableMembership::Unresolved;
        return !visible||hidden?DrawableMembership::Noncontributing:DrawableMembership::Drawable;
    }
    enum class RecipeFailure { None,RootOrUpstream,InstanceCount,MeshOrDuplicate,TransformOrVisibility,
        MaterialCount,MissingMaterial,MeshBounds,UniqueMeshCount,AggregateBounds };
    struct RecipeValidation {RecipeFailure Failure=RecipeFailure::None;size_t Instance=SIZE_MAX;};
    inline const char* Label(RecipeFailure failure)noexcept
    {
        switch(failure){case RecipeFailure::None:return nullptr;case RecipeFailure::RootOrUpstream:return "unverified_root_or_upstream";
        case RecipeFailure::InstanceCount:return "static_instance_count";case RecipeFailure::MeshOrDuplicate:return "missing_mesh_or_duplicate_instance";
        case RecipeFailure::TransformOrVisibility:return "unsupported_transform_or_visibility";case RecipeFailure::MaterialCount:return "material_count";
        case RecipeFailure::MissingMaterial:return "missing_material";case RecipeFailure::MeshBounds:return "invalid_mesh_bounds";
        case RecipeFailure::UniqueMeshCount:return "unique_mesh_count";default:return "invalid_aggregate_bounds";}
    }
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
        bool RootVerified=false,Complete=false,HighDetail=false,General=false;
        // Complete certifies the included inputs, not equivalence to all gameplay geometry.
        bool VisualIncomplete=false;
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
    inline RecipeValidation ValidateTyped(const Recipe& r,Footprint& f)
    {
        f={};
        if(!r.ActorClass||!r.RootVerified||!r.Complete||(!r.HighDetail&&!r.General))return {RecipeFailure::RootOrUpstream};
        if(!r.Count||r.Count>Limit||(!r.General&&r.Count!=Expected))return {RecipeFailure::InstanceCount};
        std::set<uintptr_t> meshes;std::set<uint32_t> components;size_t assigned=0;
        for(size_t n=0;n<r.Count;++n)
        {
            const auto& i=r.Instances[n];
            if(!i.Mesh||!components.insert(i.ComponentIndex).second)return {RecipeFailure::MeshOrDuplicate,n};
            meshes.insert(i.Mesh);
            if(!i.Verified||!i.Visible||i.Hidden||!A::Valid(i.Transform))return {RecipeFailure::TransformOrVisibility,n};
            if(!i.MaterialCount||i.MaterialCount>Slots||i.MaterialCount>Assignments-assigned)return {RecipeFailure::MaterialCount,n};
            assigned+=i.MaterialCount;
            for(size_t s=0;s<i.MaterialCount;++s)if(!i.Materials[s])return {RecipeFailure::MissingMaterial,n};
            if(!Add(f,i))return {RecipeFailure::MeshBounds,n};
        }
        if(!r.General&&meshes.size()!=UniqueExpected)return {RecipeFailure::UniqueMeshCount};
        for(size_t a=0;a<3;++a)if(!std::isfinite(f.Max[a]-f.Min[a])||f.Max[a]<=f.Min[a])return {RecipeFailure::AggregateBounds};
        return {};
    }
    inline const char* Validate(const Recipe& r,Footprint& f){return Label(ValidateTyped(r,f).Failure);}
    inline double OrthoWidth(const Footprint& f)
    {return (std::max)(f.Max[0]-f.Min[0],f.Max[1]-f.Min[1])*1.1;}
}
