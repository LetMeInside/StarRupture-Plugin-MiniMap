#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>

namespace MiniMapDefinitionAcquisition
{
    // Logical key and UObject lifetime are separate. No native pointers survive.
    struct Token
    {
        uint64_t Generation{};
        double Publication{};
        int32_t Index=-1,Serial{},NameIndex=-1;
        uint32_t NameNumber{},Building{};
    };
    enum class Status
    {
        Success,InvalidToken,FacilitiesUnavailable,WrongThread,WorldChanged,PublicationChanged,
        InvalidTable,OutOfBounds,UnreadableItem,NotResident,ObjectNotReady,UnreadableObject,RecycledSlot,
        IndexMismatch,GarbageOrDestroying,WrongClass,DefinitionMismatch,RetentionFailed,
        ChangedDuringAcquisition,Interrupted
    };
    inline const char* Label(Status s)noexcept
    {
        switch(s){
        case Status::Success:return "success";case Status::InvalidToken:return "invalid_token";
        case Status::FacilitiesUnavailable:return "facilities_unavailable";case Status::WrongThread:return "wrong_thread";
        case Status::WorldChanged:return "world_changed";case Status::PublicationChanged:return "publication_changed";
        case Status::InvalidTable:return "invalid_table";case Status::OutOfBounds:return "out_of_bounds";
        case Status::UnreadableItem:return "unreadable_item";case Status::NotResident:return "not_resident";
        case Status::ObjectNotReady:return "object_not_ready";
        case Status::UnreadableObject:return "unreadable_object";case Status::RecycledSlot:return "recycled_slot";
        case Status::IndexMismatch:return "index_mismatch";case Status::GarbageOrDestroying:return "garbage_or_destroying";
        case Status::WrongClass:return "wrong_class";case Status::DefinitionMismatch:return "definition_mismatch";
        case Status::RetentionFailed:return "retention_failed";case Status::ChangedDuringAcquisition:return "changed_during_acquisition";
        default:return "interrupted";}
    }
    struct Slot {uintptr_t Object{},Item{};int32_t Serial{};uint32_t Flags{};};
    // Diagnostic observations only, never acquisition or scheduling authority.
    struct ScopeObservation
    {
        unsigned RetainReached{},RetainReturned{},Retained{},ReleaseReached{},ReleaseReturned{};
        bool PostRetainValidated=false,VisitorEntered=false,VisitorReturned=false,Interrupted=false;
    };
    // CL-127004 PDB: FUObjectArray.ObjObjects +10; item stride 18,
    // Object +0, Flags +8, SerialNumber +10. Reader must range-check every read.
    template<class Ops> Status ReadSlot(Ops& ops,uintptr_t array,int32_t index,Slot& slot)
    {
        if(!array)return Status::FacilitiesUnavailable;
        if((array&7)||array>UINTPTR_MAX-0x30)return Status::InvalidTable;
        if(index<0)return Status::InvalidToken;
        int32_t maximum{},count{},maxChunks{},chunks{};uintptr_t table{},chunk{};
        if(!ops.Read(array+0x20,maximum)||!ops.Read(array+0x24,count)||
            !ops.Read(array+0x28,maxChunks)||!ops.Read(array+0x2c,chunks)||!ops.Read(array+0x10,table))return Status::InvalidTable;
        if(count<0||maximum<count||maximum>64*1024*1024||chunks<0||maxChunks<chunks||maxChunks>4096||
            int64_t(count)>int64_t(chunks)*65536||int64_t(maximum)>int64_t(maxChunks)*65536)return Status::InvalidTable;
        if(index>=count||uint32_t(index)/65536>=uint32_t(chunks))return Status::OutOfBounds;
        const auto offset=uintptr_t(uint32_t(index)/65536)*8;
        if(!table||(table&7)||table>UINTPTR_MAX-offset||!ops.Read(table+offset,chunk))return Status::UnreadableItem;
        const auto itemOffset=uintptr_t(uint32_t(index)%65536)*0x18;
        if(!chunk||(chunk&7)||chunk>UINTPTR_MAX-itemOffset-0x18)return Status::UnreadableItem;
        slot.Item=chunk+itemOffset;
        if(!ops.Read(slot.Item,slot.Object)||!ops.Read(slot.Item+8,slot.Flags)||!ops.Read(slot.Item+0x10,slot.Serial))return Status::UnreadableItem;
        return Status::Success;
    }
    template<class Ops> Status Check(Ops& ops,uintptr_t array,const Token& token,Slot& slot)
    {
        auto status=ReadSlot(ops,array,token.Index,slot);if(status!=Status::Success)return status;
        if(slot.Serial!=token.Serial)return Status::RecycledSlot;
        if(!slot.Object)return Status::NotResident;
        if(slot.Flags&0x10200000)return Status::GarbageOrDestroying;
        if((slot.Object&7)||slot.Object>UINTPTR_MAX-0x174)return Status::UnreadableObject;
        int32_t index{},name{};uint32_t flags{},number{},building{};
        if(!ops.Read(slot.Object+0xc,index)||!ops.Read(slot.Object+8,flags))return Status::UnreadableObject;
        if(index!=token.Index)return Status::IndexMismatch;
        if(flags&0x18000)return Status::GarbageOrDestroying;
        if(flags&0x3400)return Status::ObjectNotReady;
        const auto compatible=ops.Compatible(slot.Object);if(compatible!=Status::Success)return compatible;
        if(!ops.Read(slot.Object+0x18,name)||!ops.Read(slot.Object+0x1c,number)||
            !ops.Read(slot.Object+0x170,building))return Status::UnreadableObject;
        if(name!=token.NameIndex||number!=token.NameNumber||building!=token.Building)return Status::DefinitionMismatch;
        return Status::Success;
    }
    // Only on the permitted synchronous game-thread scope: no yield, GC entry,
    // asset load or native callback between lookup and Retain. Retain returning
    // false means it acquired nothing; true owns exactly one reference. The
    // real void AddRef binding has no recoverable failure return.
    template<class Ops,class Visitor> Status Visit(Ops& ops,uintptr_t array,const Token& token,Visitor visitor,ScopeObservation* observation=nullptr)
    {
        if(observation)*observation={};
        if(!ops.GameThread())return Status::WrongThread;
        if(!ops.Available()||!array)return Status::FacilitiesUnavailable;
        if(!token.Generation||!std::isfinite(token.Publication)||token.Index<0||token.Serial<=0||token.NameIndex<0)return Status::InvalidToken;
        auto current=ops.Current(token);if(current!=Status::Success)return current;
        Slot before;auto status=Check(ops,array,token,before);if(status!=Status::Success)return status;
        if(observation)++observation->RetainReached;
        const bool retained=ops.Retain(before.Object);
        if(observation)++observation->RetainReturned;
        if(!retained)return Status::RetentionFailed;
        if(observation)++observation->Retained;
        struct Release {Ops& Owner;uintptr_t Object;ScopeObservation* Observation;
            ~Release(){if(Observation)++Observation->ReleaseReached;Owner.Release(Object);if(Observation)++Observation->ReleaseReturned;}} release{ops,before.Object,observation};
        try
        {
            Slot after;status=Check(ops,array,token,after);
            if(status!=Status::Success||after.Object!=before.Object)return Status::ChangedDuringAcquisition;
            if(observation)observation->PostRetainValidated=true;
            current=ops.Current(token);if(current!=Status::Success)return current;
            if(observation)observation->VisitorEntered=true;
            visitor(before.Object);
            if(observation)observation->VisitorReturned=true;
            current=ops.Current(token);if(current!=Status::Success)return current;
            return Status::Success;
        }
        catch(...){if(observation)observation->Interrupted=true;return Status::Interrupted;}
    }
}
