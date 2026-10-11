#pragma once
#include "../Map/RepresentationMetadataCore.h"
#include <array>
#include <bit>
#include <memory>
#include <tuple>
#include <chrono>

namespace MiniMapSpriteDiscoveryCore
{
    inline constexpr size_t Capacity=256,VisitsPerTick=128,NewPerTick=16,RecordLimit=65536,OutputLimit=256*1024;
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
        std::array<uint32_t,4> TintBits{0x3f800000,0x3f800000,0x3f800000,0x3f800000};
        Tier Representation=Tier::Unknown;
        bool Spline=false;
        auto Values()const{return std::tuple(Generation,Building,DefinitionObject,NameIndex,NameNumber,
            VisualClass,MeshObject,MeshName,MeshNumber,TintBits,Representation,Spline);}
        bool operator==(const Key& b)const{return Values()==b.Values();}
    };
    struct Input { Key Identity;bool DefinitionValid=false,GeometryMetadata=false,VisualInputsValid=true; };
    inline bool CopyTint(Input& r,const std::array<float,4>& tint)
    {
        for(size_t i=0;i<4;++i)
        {
            if(!std::isfinite(tint[i])){r.VisualInputsValid=false;return false;}
            r.Identity.TintBits[i]=std::bit_cast<uint32_t>(tint[i]==0?0.0f:tint[i]);
        }
        return true;
    }
    struct Candidate
    {
        Key Identity;size_t Instances{};State Status=State::Discovered;
        bool CopiedGeometry=false;
        const char* Reason="not_assessed";
    };
    struct Registry
    {
        std::array<Candidate,Capacity> Entries{};
        size_t Count{},SnapshotRecords{},Cursor{},ValidRecords{},InvalidRecords{},RepresentedInstances{},
            CapacitySkipped{},LastVisits{},LastNew{},Ticks{};
        uint64_t Generation{};
        double PublicationSeconds{};
        bool SourceComplete=false,Finished=false,RecordTruncated=false,CapacityTruncated=false;
        void Begin(uint64_t generation,size_t records,bool complete)
        {
            *this={};Generation=generation;SnapshotRecords=records;SourceComplete=complete;
            RecordTruncated=records>RecordLimit;
        }
        template<class Get> bool Step(Get get,double budgetMs=2.0)
        {
            LastVisits=LastNew=0;if(Finished)return false;++Ticks;
            const auto start=std::chrono::steady_clock::now();
            const size_t end=(std::min)(SnapshotRecords,RecordLimit);
            while(Cursor<end&&LastVisits<VisitsPerTick)
            {
                if(LastVisits&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
                const auto r=get(Cursor);
                const bool valid=r.DefinitionValid&&r.VisualInputsValid&&r.Identity.Generation==Generation&&
                    r.Identity.DefinitionObject>=0&&r.Identity.NameIndex>=0;
                size_t found=0;
                if(valid)for(;found<Count;++found)if(Entries[found].Identity==r.Identity)break;
                if(valid&&found==Count&&Count<Capacity&&LastNew==NewPerTick)break;
                ++Cursor;++LastVisits;
                if(!valid){++InvalidRecords;continue;}
                ++ValidRecords;
                if(found==Count)
                {
                    if(Count==Capacity){++CapacitySkipped;CapacityTruncated=true;continue;}
                    auto& c=Entries[Count++];c.Identity=r.Identity;c.CopiedGeometry=r.GeometryMetadata;
                    c.Status=r.Identity.Spline?State::Unsupported:State::NeedsValidation;
                    c.Reason=r.Identity.Spline?"instance_spline_requires_separate_capture_capability":
                        r.Identity.VisualClass>0?"copied_class_index_only_residency_and_static_templates_unverified":
                        "representation_route_not_examined";
                    ++LastNew;
                }
                ++Entries[found].Instances;++RepresentedInstances;
            }
            Finished=Cursor==end;return Finished;
        }
        bool Complete()const{return Finished&&SourceComplete&&!RecordTruncated&&!CapacityTruncated&&!InvalidRecords;}
        void FinishDeferred(){Finished=true;RecordTruncated=true;}
    };
    enum class Publication { None,Ready,Invalidated };
    // CPU-only snapshot owner. No native pointer, name lookup or asset handle.
    template<class Snapshot> struct Session
    {
        Registry Working,Published;
        std::shared_ptr<const Snapshot> Held;
        double NextScan{};
        uint64_t BlockedGeneration{};
        bool Blocked=false,InvalidateOutput=false;
        void Clear(){Held.reset();Working={};Published={};NextScan=0;}
        void Invalidate(uint64_t generation)
        {BlockedGeneration=generation;Blocked=true;Clear();InvalidateOutput=true;}
        template<class Get> Publication Service(std::shared_ptr<const Snapshot> latest,bool world,double now,Get get)
        {
            Publication result=Publication::None;
            if(InvalidateOutput){result=Publication::Invalidated;InvalidateOutput=false;}
            if(!latest||!world)
            {if(Held||Published.Generation){Clear();result=Publication::Invalidated;}return result;}
            if(Blocked){if(latest->Generation==BlockedGeneration)return result;Blocked=false;}
            if((Held&&Held->Generation!=latest->Generation)||(Published.Generation&&Published.Generation!=latest->Generation))
            {Clear();result=Publication::Invalidated;}
            if(!Held)
            {
                if(now<NextScan)return result;
                Held=std::move(latest);Working.Begin(Held->Generation,Held->Records.size(),Held->Complete);
                Working.PublicationSeconds=Held->PublishedAtSeconds;
            }
            const bool done=Working.Step([&](size_t i){return get(Held->Records[i]);});
            const bool oversized=Held->Records.size()>RecordLimit;
            if(oversized)Working.FinishDeferred();
            if(done||oversized){Published=Working;Held.reset();NextScan=now+10;return Publication::Ready;}
            return result;
        }
    };
    inline std::string KeyText(const Key& k)
    {
        return "g"+std::to_string(k.Generation)+":b"+std::to_string(k.Building)+":d"+std::to_string(k.DefinitionObject)+
            ":n"+std::to_string(k.NameIndex)+"_"+std::to_string(k.NameNumber)+":c"+std::to_string(k.VisualClass)+
            ":m"+std::to_string(k.MeshObject)+"_"+std::to_string(k.MeshName)+"_"+std::to_string(k.MeshNumber)+
            ":t"+std::to_string(int(k.Representation))+":s"+std::to_string(k.Spline)+
            ":rgba"+std::to_string(k.TintBits[0])+"_"+std::to_string(k.TintBits[1])+"_"+std::to_string(k.TintBits[2])+"_"+std::to_string(k.TintBits[3]);
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
                {"copied_customization_rgba_bits","["+std::to_string(k.TintBits[0])+","+std::to_string(k.TintBits[1])+","+std::to_string(k.TintBits[2])+","+std::to_string(k.TintBits[3])+"]"},
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
            {"service_ticks",std::to_string(r.Ticks)},{"record_limit",std::to_string(RecordLimit)},
            {"visits_per_tick_limit",std::to_string(VisitsPerTick)},{"new_entries_per_tick_limit",std::to_string(NewPerTick)},
            {"registry_capacity",std::to_string(Capacity)},{"new_sprites_generated","0"},
            {"limitations",J::Quote("Spatial collector coverage only. Counts describe one held publication, not the live world. Keys are reversible provisional groups, not proven visual equivalence. No native representation probe, residency test, variant hash, sprite or capture validation.")},
            {"candidates","["+entries+"]"}});
    }
}
