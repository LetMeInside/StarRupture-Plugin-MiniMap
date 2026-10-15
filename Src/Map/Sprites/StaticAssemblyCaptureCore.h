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
    // Optional one-shot evidence; never contributes to static admission or publication.
    struct SkeletalDiagnostic
    {
        Instance Input;
        uintptr_t Template{},ComponentClass{};
        bool Present=false;
    };
    struct Recipe
    {
        std::array<Instance,Limit> Instances{};
        size_t Count{};
        SkeletalDiagnostic SkeletalTest;
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
    inline bool Equivalent(A::Transform a,A::Transform b)
    {
        if(!A::Valid(a)||!A::Valid(b)||!A::Normalize(a.R)||!A::Normalize(b.R))return false;
        double dot=0;for(size_t n=0;n<4;++n)dot+=a.R[n]*b.R[n];
        if(std::abs(std::abs(dot)-1)>1e-8)return false;
        for(size_t n=0;n<3;++n)if(std::abs(a.T[n]-b.T[n])>1e-7||std::abs(a.S[n]-b.S[n])>1e-8)return false;
        return true;
    }
    struct SubsetSelection {size_t Omitted{},Duplicates{};bool Conflict=false;};
    // Bounded, allocation-free selection after every hierarchy transform has been
    // resolved. Never invent a transform or treat an unknown subclass as rigid geometry.
    inline SubsetSelection SelectSupportedSubset(Recipe& recipe)
    {
        SubsetSelection result;size_t kept=0;
        if(recipe.Count>Limit){result.Conflict=true;return result;}
        for(size_t n=0;n<recipe.Count;++n)
        {
            const auto input=recipe.Instances[n];Footprint bounds;
            bool valid=input.Mesh&&input.Verified&&input.Visible&&!input.Hidden&&A::Valid(input.Transform)&&
                input.MaterialCount&&input.MaterialCount<=Slots&&Add(bounds,input);
            for(size_t m=0;valid&&m<input.MaterialCount;++m)valid=input.Materials[m]!=0;
            if(!valid){++result.Omitted;continue;}
            bool duplicate=false;
            for(size_t p=0;p<kept;++p)
            {
                const auto& prior=recipe.Instances[p];if(prior.ComponentIndex!=input.ComponentIndex)continue;
                if(prior.Mesh!=input.Mesh||!Equivalent(prior.Transform,input.Transform)||
                    prior.LocalBounds.Origin!=input.LocalBounds.Origin||prior.LocalBounds.Extent!=input.LocalBounds.Extent||
                    prior.MaterialCount!=input.MaterialCount||prior.Materials!=input.Materials||prior.Overrides!=input.Overrides)
                {result.Conflict=true;return result;}
                duplicate=true;break;
            }
            if(duplicate){++result.Duplicates;continue;}
            recipe.Instances[kept++]=input;
        }
        recipe.Count=kept;recipe.VisualIncomplete|=result.Omitted!=0;
        return result;
    }
    inline bool CanCleanBeforeCommit(bool associationUncertain,bool registered,bool currentWorld)
    {return !associationUncertain&&(!registered||currentWorld);}
    // One bounded ledger; cleanup callbacks execute only after the external
    // capture/readback owner has granted retirement. Never owns native memory.
    struct Ledger
    {
        struct Entry {uintptr_t Component{};bool Registered=false,Member=false;};
        std::array<Entry,Limit+1> Entries{};size_t Count{};
        bool Complete(size_t expected=Expected) const
        {
            if(!expected||expected>Entries.size()||Count!=expected)return false;std::set<uintptr_t> unique;
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
