#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace MiniMapBlueprintIdentityCache
{
    // CPU metadata only. All access is serialized by the owning cache mutex.
    // No native pointer, borrowed string, or map iterator escapes a transaction.
    template<class Result> class Store
    {
        static_assert(std::is_nothrow_copy_assignable_v<Result>);
    public:
        using Token=std::array<int64_t,3>;
        struct Entry {Result Identity;};
        struct PathRecord {std::shared_ptr<const std::string> Path;bool AcquisitionReported=false,InventoryReported=false,NativeBodyReported=false;};
        using Definitions=std::map<Token,Entry>;
        using Paths=std::map<std::string,PathRecord,std::less<>>;
        struct State {Definitions Items;Paths Canonical;State* Next{};};
        struct Ticket {uint64_t Generation{},Epoch{};Token Definition{};};
        struct Prepared {Result Value;typename Paths::node_type Path;std::shared_ptr<const std::string> Displaced;};
        uint64_t Generation{},Epoch{};
        size_t AllocationFailures{};
    private:
        State* Active{};State* Head{};State* Tail{};
    public:
        Store()=default;Store(const Store&)=delete;
        void Reset(uint64_t generation=0) noexcept
        {
            if(Active){if(Tail)Tail->Next=Active;else Head=Active;Tail=Active;Active=nullptr;}
            Generation=generation;
            if(Epoch==UINT64_MAX)std::terminate();++Epoch;
        }
        size_t Count()const noexcept{return Active?Active->Items.size():0;}
        size_t PathCount()const noexcept{return Active?Active->Canonical.size():0;}
        Entry* Find(const Token& token)
        {if(!Active)return nullptr;auto i=Active->Items.find(token);return i==Active->Items.end()?nullptr:&i->second;}
        PathRecord* FindPath(const std::string& path)
        {if(!Active)return nullptr;auto i=Active->Canonical.find(path);return i==Active->Canonical.end()?nullptr:&i->second;}
        Ticket Ensure(uint64_t generation,const Token& token)
        {
            if(!generation||Generation!=generation)throw std::logic_error("identity generation not installed");
            if(!Active)Active=new State;
            Active->Items.try_emplace(token);return {Generation,Epoch,token};
        }
        bool Current(const Ticket& ticket)const noexcept
        {return Active&&ticket.Generation==Generation&&ticket.Epoch==Epoch;}
        // Allocate/copy a potential path node OUTSIDE the owner's sensitive lock.
        static Prepared Prepare(const Result& result)
        {
            Prepared p;p.Value=result;
            if(result.Path){Paths paths;paths.emplace(*result.Path,PathRecord{result.Path});p.Path=paths.extract(paths.begin());}
            return p;
        }
        Result Commit(const Ticket& ticket,Prepared& prepared)
        {
            if(!Current(ticket))return {{},"identity_generation_changed"};
            auto* entry=Find(ticket.Definition);if(!entry)return {{},"identity_token_missing"};
            if(entry->Identity.Path)return entry->Identity; // successful identity is final
            if(prepared.Value.Path)
            {
                if(prepared.Displaced)return {{},"identity_preparation_already_committed"};
                auto* existing=FindPath(*prepared.Value.Path);
                if(existing){prepared.Displaced=std::move(prepared.Value.Path);prepared.Value.Path=existing->Path;}
                else
                {
                    if(prepared.Path.empty())return {{},"identity_path_preparation_missing"};
                    auto inserted=Active->Canonical.insert(std::move(prepared.Path));
                    if(!inserted.inserted)throw std::logic_error("identity path insertion collision");
                }
            }
            // Previous values have no path (success returned above). Failed
            // reasons remain retryable, never promoted to successful identities.
            entry->Identity=prepared.Value;return entry->Identity;
        }
        void AllocationFailed(const Ticket* ticket=nullptr) noexcept
        {
            ++AllocationFailures;
            if(ticket&&Current(*ticket))if(auto* entry=Find(ticket->Definition);entry&&!entry->Identity.Path)
                entry->Identity.Reason="identity_metadata_allocation_failure";
        }
        // GT sole maintenance caller. Extract one node under lock; potentially
        // final string ownership and state destruction happen AFTER unlocking.
        size_t Maintain(std::mutex& mutex,size_t limit=16,double budgetMs=2.0)
        {
            const auto start=std::chrono::steady_clock::now();size_t units=0;
            while(units<limit)
            {
                typename Definitions::node_type definition;typename Paths::node_type path;State* empty{};
                {
                    std::scoped_lock lock(mutex);if(!Head)break;
                    if(!Head->Items.empty())definition=Head->Items.extract(Head->Items.begin());
                    else if(!Head->Canonical.empty())path=Head->Canonical.extract(Head->Canonical.begin());
                    if(Head->Items.empty()&&Head->Canonical.empty()){empty=Head;Head=Head->Next;if(!Head)Tail=nullptr;}
                }
                delete empty;++units;
                if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
            }
            return units;
        }
        // Normal lifecycle must detach and service incrementally. This fallback
        // runs only at final process/plugin destruction, never on generation reset.
        ~Store(){Reset();std::mutex mutex;while(Maintain(mutex,16,(std::numeric_limits<double>::max)())){}}
    };
}
