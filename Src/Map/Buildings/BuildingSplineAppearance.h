#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Diagnostic values only. No native address, borrowed container or eligibility
// decision can escape the synchronous authored-resource acquisition scope.
namespace MiniMapBuildingSplineAppearance
{
    struct Row
    {
        std::string Source, Object, Detail;
        bool operator==(const Row&) const = default;
    };
    struct Report
    {
        uint64_t Generation=0;
        std::string AuthoredClass;
        std::vector<Row> Rows;
        // Latest sampled texture metadata/streaming evidence, not authored equality.
        std::vector<Row> Residency;
        bool SameAuthored(const Report& other) const
        {return Generation==other.Generation && AuthoredClass==other.AuthoredClass && Rows==other.Rows;}
        bool operator==(const Report&) const = default;
    };
    enum class Kind { Class, Defaults, Component, Script, Node, Handler, Mesh, Material, Texture };
    // These work items exist only on the acquisition stack, never in Report.
    struct Work
    {
        Kind Type=Kind::Class;
        uintptr_t Object=0, Owner=0;
        std::string Source;
        std::vector<std::pair<Kind,uintptr_t>> Ancestors;
        Work(Kind kind,uintptr_t object,uintptr_t owner,std::string source)
            :Type(kind),Object(object),Owner(owner),Source(std::move(source)){}
    };
    struct Slot {uintptr_t Default=0,Override=0,Selected=0;};
    inline std::vector<Slot> SelectSlots(const std::vector<uintptr_t>& defaults,const std::vector<uintptr_t>& overrides)
    {
        std::vector<Slot> result((std::max)(defaults.size(),overrides.size()));
        for(size_t i=0;i<result.size();++i)
        {
            auto& slot=result[i];
            if(i<defaults.size())slot.Default=defaults[i];
            if(i<overrides.size())slot.Override=overrides[i];
            slot.Selected=slot.Override?slot.Override:slot.Default;
        }
        return result;
    }
    // The native adapter and standalone fixture use this exact traversal. Ops
    // validates/holds objects and owns all releases, including exception exits.
    template<class Ops> std::shared_ptr<const Report> Acquire(Ops& ops,uintptr_t type,uint64_t generation,
        const std::string& path,const std::shared_ptr<const Report>& previous={})
    {
        auto report=std::make_shared<Report>();report->Generation=generation;report->AuthoredClass=path;
        std::vector<Work> pending;pending.emplace_back(Kind::Class,type,0,"authored_class");
        std::set<std::pair<Kind,uintptr_t>> visited; // Operation-local cycle/shared-resource detection only.
        while(!pending.empty())
        {
            auto item=std::move(pending.back());pending.pop_back();
            const auto key=std::pair{item.Type,item.Object};
            if(std::find(item.Ancestors.begin(),item.Ancestors.end(),key)!=item.Ancestors.end())
            {report->Rows.push_back({item.Source,"unavailable","authored_reference_cycle; traversal incomplete"});continue;}
            if(!visited.insert(key).second)continue;
            std::vector<Work> children;
            ops.Inspect(item,*report,children);
            for(auto it=children.rbegin();it!=children.rend();++it)
            {
                it->Ancestors=item.Ancestors;it->Ancestors.push_back(key);
                pending.push_back(std::move(*it));
            }
        }
        if(!ops.Finish())return {}; // No partial observation after lifecycle/identity/release failure.
        if(previous && *previous==*report)return previous;
        return report;
    }
}
