#pragma once
#include "RepresentationMetadataCore.h"
#include <array>
#include <bit>
#include <memory>
#include <tuple>
#include <chrono>
#include <map>
#include <new>
#include <mutex>
#include <limits>

namespace MiniMapSpriteDiscoveryCore
{
    // Work slices and diagnostic detail bounds do not limit production storage.
    inline constexpr size_t VisitsPerTick=128,NewPerTick=16,DiagnosticCandidateLimit=256,OutputLimit=256*1024;
    enum class Tier : uint8_t { Unknown,High,Low,Mass };
    enum class State : uint8_t { Discovered,NeedsValidation,EligibleCandidate,Unsupported,Deferred,Invalidated };
    inline const char* StateName(State s)
    {
        switch(s){case State::Discovered:return "Discovered";case State::NeedsValidation:return "NeedsValidation";
        case State::EligibleCandidate:return "EligibleCandidate";case State::Unsupported:return "Unsupported";
        case State::Deferred:return "Deferred";default:return "Invalidated";}
    }
    struct Key
    {
        uint64_t Generation{};
        uint32_t Building{},NameNumber{};
        int32_t DefinitionObject{},NameIndex{},VisualClass{},MeshObject{},MeshName{};
        uint32_t MeshNumber{};
        Tier Representation=Tier::Unknown;
        bool Spline=false;
        auto Values()const{return std::tuple(Generation,Building,DefinitionObject,NameIndex,NameNumber,
            VisualClass,MeshObject,MeshName,MeshNumber,Representation,Spline);}
        bool operator==(const Key& b)const{return Values()==b.Values();}
    };
    struct Input
    {
        Key Identity;bool DefinitionValid=false,GeometryMetadata=false,VisualInputsValid=true;
        bool TransformValid=false;std::array<double,3> Scale{};
    };
    struct ScaleAggregate
    {
        bool Known=false;double Maximum=1;
        void Observe(bool transformValid,const std::array<double,3>& scale)noexcept
        {
            if(!transformValid)return;
            bool supported=true;for(double v:scale)supported&=std::isfinite(v)&&v>0&&v<=1000;
            if(!supported)return;
            Known=true;Maximum=(std::max)({Maximum,scale[0],scale[1]});
        }
    };
    struct Candidate
    {
        Key Identity;size_t Instances{};State Status=State::Discovered;
        bool CopiedGeometry=false;ScaleAggregate InstanceScale;
        const char* Reason="not_assessed";
    };
    struct KeyLess
    {bool operator()(const Key& a,const Key& b)const noexcept{return a.Values()<b.Values();}};
    // Ordinal nodes retain first-observation order and stable addresses. Unlike
    // MSVC deque spare blocks, each removed node releases its storage immediately;
    // empty-container destruction does not revisit the historical population.
    class CandidateStorage
    {
        std::map<size_t,Candidate> Nodes;
    public:
        size_t size()const noexcept{return Nodes.size();}
        bool empty()const noexcept{return Nodes.empty();}
        Candidate& operator[](size_t i){return Nodes.at(i);}
        const Candidate& operator[](size_t i)const{return Nodes.at(i);}
        Candidate& back(){return Nodes.rbegin()->second;}
        const Candidate& back()const{return Nodes.rbegin()->second;}
        void push_back(const Candidate& candidate){Nodes.emplace(Nodes.size(),candidate);}
        void pop_back()noexcept{Nodes.erase(std::prev(Nodes.end()));}
        void clear()noexcept{Nodes.clear();}
        void swap(CandidateStorage& other)noexcept{Nodes.swap(other.Nodes);}
    };
    struct Registry
    {
        // Appends preserve addresses; only ordinal indices cross service slices.
        CandidateStorage Entries;
        std::map<Key,size_t,KeyLess> Index;
        size_t Count{},SnapshotRecords{},Cursor{},ValidRecords{},InvalidRecords{},RepresentedInstances{},
            CapacitySkipped{},LastVisits{},LastNew{},Ticks{},AllocationFailures{};
        uint64_t Generation{};
        double PublicationSeconds{};
        bool SourceComplete=false,Finished=false,RecordTruncated=false,CapacityTruncated=false,AllocationBlocked=false;
        void Begin(uint64_t generation,size_t records,bool complete)
        {
            Entries.clear();Index.clear();
            Count=Cursor=ValidRecords=InvalidRecords=RepresentedInstances=CapacitySkipped=LastVisits=LastNew=Ticks=AllocationFailures=0;
            PublicationSeconds=0;Finished=RecordTruncated=CapacityTruncated=AllocationBlocked=false;
            Generation=generation;SnapshotRecords=records;SourceComplete=complete;
        }
        template<class Get> bool Step(Get get,double budgetMs=2.0)
        {
            LastVisits=LastNew=0;if(Finished)return false;++Ticks;AllocationBlocked=false;
            const auto start=std::chrono::steady_clock::now();
            const size_t end=SnapshotRecords;
            while(Cursor<end&&LastVisits<VisitsPerTick)
            {
                if(LastVisits&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
                const auto r=get(Cursor);
                const bool valid=r.DefinitionValid&&r.VisualInputsValid&&r.Identity.Generation==Generation&&
                    r.Identity.DefinitionObject>=0&&r.Identity.NameIndex>=0;
                if(!valid){++Cursor;++LastVisits;++InvalidRecords;continue;}
                const auto existing=Index.find(r.Identity);
                size_t found=existing==Index.end()?Count:existing->second;
                if(existing==Index.end())
                {
                    if(LastNew==NewPerTick)break;
                    Candidate candidate;candidate.Identity=r.Identity;candidate.CopiedGeometry=r.GeometryMetadata;
                    candidate.Status=r.Identity.Spline?State::Unsupported:State::NeedsValidation;
                    candidate.Reason=r.Identity.Spline?"instance_spline_requires_separate_capture_capability":
                        r.Identity.VisualClass>0?"copied_class_index_only_residency_and_static_templates_unverified":
                        "representation_route_not_examined";
                    bool appended=false;
                    try
                    {
                        Entries.push_back(candidate);appended=true;
                        Index.emplace(r.Identity,Count);
                    }
                    catch(const std::bad_alloc&)
                    {
                        if(appended)Entries.pop_back();
                        ++AllocationFailures;AllocationBlocked=true;return false;
                    }
                    // Both owned candidate and index exist before consuming input.
                    ++Count;++LastNew;
                }
                ++Cursor;++LastVisits;++ValidRecords;
                ++Entries[found].Instances;++RepresentedInstances;
                Entries[found].InstanceScale.Observe(r.TransformValid,r.Scale);
            }
            Finished=Cursor==end;return Finished;
        }
        bool Complete()const{return Finished&&SourceComplete&&!AllocationBlocked&&!RecordTruncated&&!CapacityTruncated&&!InvalidRecords;}
        void Swap(Registry& other)noexcept
        {
            Entries.swap(other.Entries);Index.swap(other.Index);
            using std::swap;
            swap(Count,other.Count);
            swap(SnapshotRecords,other.SnapshotRecords);
            swap(Cursor,other.Cursor);
            swap(ValidRecords,other.ValidRecords);
            swap(InvalidRecords,other.InvalidRecords);
            swap(RepresentedInstances,other.RepresentedInstances);
            swap(CapacitySkipped,other.CapacitySkipped);
            swap(LastVisits,other.LastVisits);
            swap(LastNew,other.LastNew);
            swap(Ticks,other.Ticks);
            swap(AllocationFailures,other.AllocationFailures);
            swap(Generation,other.Generation);
            swap(PublicationSeconds,other.PublicationSeconds);
            swap(SourceComplete,other.SourceComplete);
            swap(Finished,other.Finished);
            swap(RecordTruncated,other.RecordTruncated);
            swap(CapacityTruncated,other.CapacityTruncated);
            swap(AllocationBlocked,other.AllocationBlocked);
        }
        void FinishDeferred(){Finished=true;RecordTruncated=true;}
    };
    enum class Publication { None,Ready,Invalidated };
    // The node and shared control block are allocated before scanning. The
    // last shared reader only enqueues; a weak reader cannot resurrect the
    // registry after that control block reaches zero strong references.
    struct RetiredRegistry
    {Registry Data;RetiredRegistry* Next{};};
    class RegistryRetirement : public std::enable_shared_from_this<RegistryRetirement>
    {
        std::mutex Mutex;
        RetiredRegistry *Head{},*Tail{};
        size_t Live{},Queued{},ReclaimedEntries{},ReclaimedRegistries{};
        std::shared_ptr<RegistryRetirement> KeepAlive;
    public:
        struct Counts {size_t LiveRegistries{},QueuedRegistries{},EntriesReclaimed{},RegistriesReclaimed{};};
        Counts Inspect(){std::scoped_lock lock(Mutex);return {Live,Queued,ReclaimedEntries,ReclaimedRegistries};}
        void Register()
        {
            std::scoped_lock lock(Mutex);
            if(!KeepAlive)KeepAlive=shared_from_this();
            ++Live;
        }
        void Enqueue(RetiredRegistry* node)noexcept
        {
            std::scoped_lock lock(Mutex);if(!Live)std::terminate();--Live;++Queued;
            node->Next=nullptr;if(Tail)Tail->Next=node;else Head=node;Tail=node;
        }
        size_t Service(size_t limit=16,double budgetMs=2.0)
        {
            // One maintenance caller; last-reader Enqueue may run concurrently.
            // Keep this object alive even when its self-retention is released.
            auto self=shared_from_this();
            const auto start=std::chrono::steady_clock::now();size_t units=0;
            while(units<limit)
            {
                RetiredRegistry* node=nullptr;
                {std::scoped_lock lock(Mutex);node=Head;if(!node)break;
                 Head=node->Next;if(!Head)Tail=nullptr;node->Next=nullptr;}
                bool removed=false;
                if(!node->Data.Entries.empty())
                {
                    node->Data.Index.erase(node->Data.Entries.back().Identity);
                    node->Data.Entries.pop_back();--node->Data.Count;removed=true;
                }
                const bool empty=node->Data.Entries.empty();
                if(empty){if(!node->Data.Index.empty())std::terminate();delete node;}
                std::shared_ptr<RegistryRetirement> released;
                {
                    std::scoped_lock lock(Mutex);
                    if(removed)++ReclaimedEntries;
                    if(empty){--Queued;++ReclaimedRegistries;}
                    else {node->Next=nullptr;if(Tail)Tail->Next=node;else Head=node;Tail=node;}
                    if(!Live&&!Queued)released=std::move(KeepAlive);
                }
                ++units;
                if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
            }
            return units;
        }
        // Shutdown may drain queued CPU nodes, but never waits for live readers.
        // A false result requires retaining this handle and servicing it again.
        bool DrainReleased()
        {while(Service(16,(std::numeric_limits<double>::max)())){};const auto c=Inspect();return !c.LiveRegistries&&!c.QueuedRegistries;}
        // KeepAlive prevents destruction while any registry is live or queued.
        // Integration MUST keep a service handle until DrainReleased succeeds.
        // If that hook is omitted, resources stay deferred; UI never bulk-frees.
        ~RegistryRetirement(){if(Head||Live||Queued)std::terminate();}
    };
    struct RetireRegistry
    {
        std::shared_ptr<RegistryRetirement> Owner;RetiredRegistry* Node;
        void operator()(const Registry*)const noexcept{Owner->Enqueue(Node);}
    };
    // Builder/session mutation is GT-only. Consumers receive ONLY Published,
    // copied under their publication-access lock; its Registry is immutable.
    template<class Snapshot> struct Session
    {
        std::shared_ptr<const Registry> Published;
        std::shared_ptr<const Snapshot> Held;
        std::shared_ptr<RegistryRetirement> Retirement=std::make_shared<RegistryRetirement>();
        double NextScan{};
        uint64_t BlockedGeneration{};
        size_t AllocationFailures{},IncompleteScans{};
        bool Blocked=false,InvalidateOutput=false,AllocationBlocked=false;
    private:
        std::shared_ptr<const Registry> InProgress;
        Registry* Builder{};
    public:
        // Borrowed inspection on the producer thread only; do not retain it.
        const Registry* WorkingView()const noexcept{return Builder;}
        size_t MaintainRetired(size_t limit=16,double budgetMs=2.0){return Retirement->Service(limit,budgetMs);}
        void Clear(){Builder=nullptr;InProgress.reset();Published.reset();Held.reset();NextScan=0;AllocationBlocked=false;}
        void Invalidate(uint64_t generation)
        {BlockedGeneration=generation;Blocked=true;Clear();InvalidateOutput=true;}
        template<class Get> Publication Service(std::shared_ptr<const Snapshot> latest,bool world,double now,Get get,double budgetMs=2.0)
        {
            const auto start=std::chrono::steady_clock::now();
            MaintainRetired(16,budgetMs);
            Publication result=Publication::None;
            if(InvalidateOutput){result=Publication::Invalidated;InvalidateOutput=false;}
            if(!latest||!world)
            {if(Held||Published){Clear();result=Publication::Invalidated;}return result;}
            if(Blocked){if(latest->Generation==BlockedGeneration)return result;Blocked=false;}
            if((Held&&Held->Generation!=latest->Generation)||(Published&&Published->Generation!=latest->Generation))
            {Clear();result=Publication::Invalidated;}
            if(!Held)
            {if(now<NextScan)return result;Held=std::move(latest);}
            if(!Builder)
            {
                try
                {
                    auto* node=new RetiredRegistry;
                    Retirement->Register();
                    // If control-block allocation fails, shared_ptr invokes this
                    // deleter and enqueues the node without allocating again.
                    std::shared_ptr<const Registry> view(&node->Data,RetireRegistry{Retirement,node});
                    node->Data.Begin(Held->Generation,Held->Records.size(),Held->Complete);
                    node->Data.PublicationSeconds=Held->PublishedAtSeconds;
                    Builder=&node->Data;InProgress=std::move(view);AllocationBlocked=false;
                }
                catch(const std::bad_alloc&){++AllocationFailures;AllocationBlocked=true;return result;}
            }
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(elapsed>=budgetMs)return result;
            const bool done=Builder->Finished||Builder->Step([&](size_t i){return get(Held->Records[i]);},budgetMs-elapsed);
            if(!done)return result;
            if(!Builder->Complete())
            {
                ++IncompleteScans;Builder=nullptr;InProgress.reset();Held.reset();NextScan=now+10;return result;
            }
            // O(1), allocation-free commit. Old publications enqueue on their
            // FINAL reader release; neither copy nor destruction runs here.
            Published.swap(InProgress);Builder=nullptr;InProgress.reset();
            Held.reset();NextScan=now+10;return Publication::Ready;
        }
    };
    // CPU-only source disposal. A last alias release merely links a prepared
    // node; the existing discovery worker performs potentially large destruction.
    // Keep servicing this domain until DrainReleased reports no live aliases.
    // KeepAlive protects outstanding readers and queued nodes, not eventual
    // reclamation: a late release still requires worker servicing (see Run shutdown).
    // This is a conditional P2 lifetime concern, not evidence of a leak or crash.
    template<class Snapshot> class SourceRetirement : public std::enable_shared_from_this<SourceRetirement<Snapshot>>
    {
        struct Node {std::shared_ptr<const Snapshot> Source;Node* Next{};};
        std::mutex Mutex;Node* Head{};Node* Tail{};size_t Live{};
        std::shared_ptr<SourceRetirement> KeepAlive;
        void Release(Node* node) noexcept
        {std::scoped_lock lock(Mutex);if(!Live)std::terminate();--Live;if(Tail)Tail->Next=node;else Head=node;Tail=node;}
    public:
        std::shared_ptr<const Snapshot> Hold(std::shared_ptr<const Snapshot> source)
        {
            if(!source)return {};
            auto* node=new Node{std::move(source)};
            auto self=this->shared_from_this();
            {std::scoped_lock lock(Mutex);if(!KeepAlive)KeepAlive=self;++Live;}
            // Control-block failure invokes the same allocation-free deleter.
            return {node->Source.get(),[self=std::move(self),node](const Snapshot*)noexcept{self->Release(node);}};
        }
        bool ServiceOne()
        {
            auto self=this->shared_from_this();Node* node{};std::shared_ptr<SourceRetirement> released;
            {std::scoped_lock lock(Mutex);node=Head;if(node){Head=node->Next;if(!Head)Tail=nullptr;}}
            delete node; // No publication/queue mutex held. Worker thread only.
            {std::scoped_lock lock(Mutex);if(!Live&&!Head)released=std::move(KeepAlive);}
            return node!=nullptr;
        }
        bool DrainReleased()
        {while(ServiceOne()){};std::scoped_lock lock(Mutex);return !Live&&!Head;}
        ~SourceRetirement(){if(Live||Head)std::terminate();}
    };

    inline std::string KeyText(const Key& k)
    {
        return "g"+std::to_string(k.Generation)+":b"+std::to_string(k.Building)+":d"+std::to_string(k.DefinitionObject)+
            ":n"+std::to_string(k.NameIndex)+"_"+std::to_string(k.NameNumber)+":c"+std::to_string(k.VisualClass)+
            ":m"+std::to_string(k.MeshObject)+"_"+std::to_string(k.MeshName)+"_"+std::to_string(k.MeshNumber)+
            ":t"+std::to_string(int(k.Representation))+":s"+std::to_string(k.Spline);
    }
    inline std::string Json(const Registry& r,size_t limit=OutputLimit,bool invalidated=false)
    {
        namespace J=MiniMapRepresentationCore;
        // Reserve ample space for the fixed footer/header. Never cut JSON bytes.
        limit=(std::min)(limit,OutputLimit);if(limit<4096)return "{\"stage\":\"R2.6a\",\"output_truncated\":true,\"candidates\":[],\"new_sprites_generated\":0}";
        std::string entries;size_t emitted=0;
        size_t definitions=0;
        for(size_t i=0;i<r.Count;++i)
        {
            bool prior=false;
            const auto& k=r.Entries[i].Identity;
            for(size_t j=0;j<i;++j)
            {
                const auto& p=r.Entries[j].Identity;
                prior|=std::tuple(k.DefinitionObject,k.NameIndex,k.NameNumber)==std::tuple(p.DefinitionObject,p.NameIndex,p.NameNumber);
            }
            if(!prior)++definitions;
        }
        for(size_t i=0;i<r.Count;++i)
        {
            const auto& c=r.Entries[i];const auto& k=c.Identity;
            auto item=J::Object({{"session_local_candidate_key",J::Quote(KeyText(k))},
                {"building_id",std::to_string(k.Building)},{"definition_object_index",std::to_string(k.DefinitionObject)},
                {"definition_fname_index",std::to_string(k.NameIndex)},{"definition_fname_number",std::to_string(k.NameNumber)},
                {"copied_visual_class_index",std::to_string(k.VisualClass)},
                {"copied_class_provenance",J::Quote("collector_spline_width_proxy_not_a_selected_representation_tier")},
                {"representation_tier",J::Quote(k.Representation==Tier::Unknown?"not_examined":k.Representation==Tier::High?"high":k.Representation==Tier::Low?"low":"mass")},
                {"visual_identity_confidence",J::Quote("provisional_definition_group_variants_unobserved")},
                {"instance_count",std::to_string(c.Instances)}, {"state",J::Quote(StateName(c.Status))},
                {"reason",J::Quote(c.Reason)}, {"definition_identity",J::Quote("collector_validated_at_publication_not_revalidated")},
                {"copied_geometry_metadata",c.CopiedGeometry?"true":"false"},
                {"resident_metadata_found", "null"},{"resident_metadata_status",J::Quote("not_examined")},
                {"eligible_for_future_validation",c.Status==State::NeedsValidation?"true":"false"},
                {"additional_verification_required","true"},{"verified_safe_to_capture","false"}});
            if(entries.size()+item.size()+4096>limit)break;
            if(emitted)entries+=',';entries+=item;++emitted;
        }
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.6a")},{"game_build",J::Quote("CL-127004")},
            {"world_generation",std::to_string(r.Generation)},{"lifecycle",J::Quote(invalidated?"Invalidated":"observed")},
            {"source_publication_steady_seconds",std::isfinite(r.PublicationSeconds)?std::to_string(r.PublicationSeconds):"null"},
            {"snapshot_complete",r.SourceComplete?"true":"false"},{"registry_complete",!invalidated&&r.Complete()?"true":"false"},
            {"snapshot_records",std::to_string(r.SnapshotRecords)},{"candidate_records_examined",std::to_string(r.Cursor)},
            {"valid_definition_records",std::to_string(r.ValidRecords)},{"invalid_source_records",std::to_string(r.InvalidRecords)},
            {"distinct_candidate_keys",std::to_string(r.Count)},{"total_instances_represented",std::to_string(r.RepresentedInstances)},
            {"distinct_definitions_represented",std::to_string(definitions)},
            {"deferred_unexamined_records",std::to_string(r.SnapshotRecords-r.Cursor)},
            {"candidates_emitted",std::to_string(emitted)},{"scan_finished",r.Finished?"true":"false"},
            {"record_limit_truncated",r.RecordTruncated?"true":"false"},{"registry_capacity_truncated",r.CapacityTruncated?"true":"false"},
            {"capacity_skipped_records",std::to_string(r.CapacitySkipped)},{"output_truncated",emitted<r.Count?"true":"false"},
            {"service_ticks",std::to_string(r.Ticks)},{"record_limit","null"},
            {"allocation_blocked",r.AllocationBlocked?"true":"false"},{"allocation_failures",std::to_string(r.AllocationFailures)},
            {"visits_per_tick_limit",std::to_string(VisitsPerTick)},{"new_entries_per_tick_limit",std::to_string(NewPerTick)},
            {"registry_capacity","null"},{"new_sprites_generated","0"},
            {"limitations",J::Quote("Spatial collector coverage only. Counts describe one held publication, not the live world. Keys are reversible provisional groups, not proven visual equivalence. No native representation probe, residency test, variant hash, sprite or capture validation.")},
            {"candidates","["+entries+"]"}});
    }
}
