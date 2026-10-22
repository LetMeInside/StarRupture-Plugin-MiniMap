#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>

namespace MiniMapDefinitionAcquisition
{
    // FUObjectItem internal flags, NOT UObject::EObjectFlags. Verified against
    // UE_5.6 CoreUObject/Public/UObject/ObjectMacros.h EInternalObjectFlags;
    // these are the existing CL-127004 identity mask bits. Garbage must not be
    // referenced; Unreachable is outside the live object graph. Preserve both.
    enum class ObjectItemFlag : uint32_t { Garbage=uint32_t(1)<<21, Unreachable=uint32_t(1)<<28 };
    inline constexpr uint32_t UnusableObjectItemFlags=
        uint32_t(ObjectItemFlag::Garbage)|uint32_t(ObjectItemFlag::Unreachable);
    enum class IdentityPredicate : uint8_t
    {
        Unobserved,Stable,Facilities,Pointer,IndexRead,IndexValue,ObjectFlagsRead,ObjectFlags,
        SlotRead,SlotValues,SlotMismatch,ZeroSerial,NegativeSerial,SlotFlags,
        MetaclassRead,NameRead,OuterRead,PackageRead,NameFacility,SuperclassRead,HierarchyCycle,MetaclassMismatch,ActorMismatch,
        RetainFailed,ReleaseFailed,ContextChanged,IdentityChanged,TransientValid
    };
    inline const char* Label(IdentityPredicate p)
    {
        switch(p){
        case IdentityPredicate::Stable:return "stable_identity";case IdentityPredicate::Facilities:return "identity_facilities";
        case IdentityPredicate::Pointer:return "invalid_object_pointer";case IdentityPredicate::IndexRead:return "object_index_read_failed";
        case IdentityPredicate::IndexValue:return "invalid_object_index";case IdentityPredicate::ObjectFlagsRead:return "uobject_flags_read_failed";
        case IdentityPredicate::ObjectFlags:return "rejected_uobject_flags";case IdentityPredicate::SlotRead:return "slot_read_failed";
        case IdentityPredicate::SlotValues:return "invalid_slot_table_values";case IdentityPredicate::SlotMismatch:return "slot_object_mismatch";
        case IdentityPredicate::ZeroSerial:return "zero_unassigned_serial";case IdentityPredicate::NegativeSerial:return "negative_serial";
        case IdentityPredicate::SlotFlags:return "rejected_fuobjectitem_flags";case IdentityPredicate::MetaclassRead:return "metaclass_read_failed";
        case IdentityPredicate::NameRead:return "class_name_read_failed";case IdentityPredicate::OuterRead:return "class_outer_read_failed";
        case IdentityPredicate::PackageRead:return "package_name_read_failed";case IdentityPredicate::NameFacility:return "name_facility_unavailable";
        case IdentityPredicate::SuperclassRead:return "superclass_read_failed";case IdentityPredicate::HierarchyCycle:return "class_hierarchy_cycle";
        case IdentityPredicate::MetaclassMismatch:return "metaclass_mismatch";case IdentityPredicate::ActorMismatch:return "actor_ancestry_mismatch";
        case IdentityPredicate::RetainFailed:return "retention_failed_or_unavailable";case IdentityPredicate::ReleaseFailed:return "release_unverified";
        case IdentityPredicate::ContextChanged:return "acquisition_context_changed";case IdentityPredicate::IdentityChanged:return "identity_changed_during_acquisition";
        case IdentityPredicate::TransientValid:return "transient_valid";
        default:return "unobserved";}
    }
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
    enum class IdentityRole : uint8_t { Unobserved,LookupResult,MetaclassNode,MetaclassOuter,ClassNode,ClassOuter,PublicationOwner };
    inline const char* Label(IdentityRole role)
    {
        switch(role){case IdentityRole::LookupResult:return "lookup_result";case IdentityRole::MetaclassNode:return "metaclass_node";
        case IdentityRole::MetaclassOuter:return "metaclass_outer";case IdentityRole::ClassNode:return "class_node";
        case IdentityRole::ClassOuter:return "class_outer";case IdentityRole::PublicationOwner:return "publication_owner";
        default:return "unobserved";}
    }
    enum class TransientValidity : uint8_t { Unobserved,Unverified,Valid };
    inline const char* Label(TransientValidity value)
    {return value==TransientValidity::Valid?"valid":value==TransientValidity::Unverified?"unverified":"unobserved";}
    struct IdentityObservation
    {
        IdentityPredicate Predicate=IdentityPredicate::Unobserved;
        Status SlotStatus=Status::Success;
        int32_t Index=-1,Serial=0;
        uint32_t ObjectFlags=0,ItemFlags=0;
        bool IndexObserved=false,ObjectFlagsObserved=false,SlotObserved=false;
        IdentityRole Role=IdentityRole::Unobserved;
        uint32_t Depth=0;
        bool IsLookupResult=false;
        TransientValidity Transient=TransientValidity::Unobserved;
        bool operator==(const IdentityObservation&) const=default;
    };
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
    template<class Ops> struct IdentityReader
    {
        Ops& Source;bool Failed=false;
        template<class T> bool Read(uintptr_t address,T& value)
        {const bool ok=Source.Read(address,value);Failed=Failed || !ok;return ok;}
    };
    inline constexpr uint32_t DisallowedUObjectFlags=uint32_t(0x1b400); // Existing load/postload/destruction policy; NOT item flags.
    // Current validity only, within the caller's admitted synchronous game-thread
    // scope. Does not retain, assign serials or return a persistent identity.
    // Reader must safely observe each field. ReadSlot remains the sole decoder.
    template<class Ops> TransientValidity ValidateTransientObject(Ops& ops,uintptr_t array,uintptr_t object,size_t extent,
        IdentityObservation* observation=nullptr)
    {
        IdentityObservation local;auto& out=observation?*observation:local;out={};out.Transient=TransientValidity::Unverified;
        auto fail=[&](IdentityPredicate p){out.Predicate=p;return TransientValidity::Unverified;};
        if(!array)return fail(IdentityPredicate::Facilities);
        if(extent<0x28 || !object || (object&7) || object>UINTPTR_MAX-extent)return fail(IdentityPredicate::Pointer);
        int32_t index=-1;uint32_t flags=0;
        if(!ops.Read(object+0xc,index))return fail(IdentityPredicate::IndexRead);
        out.Index=index;out.IndexObserved=true;
        if(index<0)return fail(IdentityPredicate::IndexValue);
        if(!ops.Read(object+8,flags))return fail(IdentityPredicate::ObjectFlagsRead);
        out.ObjectFlags=flags;out.ObjectFlagsObserved=true;
        if(flags&DisallowedUObjectFlags)return fail(IdentityPredicate::ObjectFlags);
        IdentityReader<Ops> reader{ops};Slot slot;
        out.SlotStatus=ReadSlot(reader,array,index,slot);
        if(out.SlotStatus!=Status::Success)return fail(reader.Failed?IdentityPredicate::SlotRead:IdentityPredicate::SlotValues);
        out.Serial=slot.Serial;out.ItemFlags=slot.Flags;out.SlotObserved=true;
        if(slot.Object!=object)return fail(IdentityPredicate::SlotMismatch);
        if(slot.Flags&UnusableObjectItemFlags)return fail(IdentityPredicate::SlotFlags);
        out.Predicate=IdentityPredicate::TransientValid;out.Transient=TransientValidity::Valid;return TransientValidity::Valid;
    }
    // Stable copied cache identities require a positive serial. AddRef does not
    // allocate one. A live zero-serial slot is observable but remains unverified;
    // never publish it or invent a serial/pointer key. No native reads beyond
    // the original profile identity sequence, and no retained pointer in output.
    template<class Ops> bool ReadStableIdentity(Ops& ops,uintptr_t array,uintptr_t object,size_t extent,
        int32_t& index,int32_t& serial,IdentityObservation* observation=nullptr)
    {
        IdentityObservation local;auto& out=observation?*observation:local;out={};
        auto fail=[&](IdentityPredicate p){out.Predicate=p;return false;};
        if(!array)return fail(IdentityPredicate::Facilities);
        if(!object || (object&7) || object>UINTPTR_MAX-extent)return fail(IdentityPredicate::Pointer);
        int32_t copiedIndex=-1;uint32_t flags=0;
        if(!ops.Read(object+0xc,copiedIndex))return fail(IdentityPredicate::IndexRead);
        out.Index=copiedIndex;out.IndexObserved=true;
        if(copiedIndex<0)return fail(IdentityPredicate::IndexValue);
        if(!ops.Read(object+8,flags))return fail(IdentityPredicate::ObjectFlagsRead);
        out.ObjectFlags=flags;out.ObjectFlagsObserved=true;
        if(flags&DisallowedUObjectFlags)return fail(IdentityPredicate::ObjectFlags);
        IdentityReader<Ops> reader{ops};
        Slot slot;
        out.SlotStatus=ReadSlot(reader,array,copiedIndex,slot);
        if(out.SlotStatus!=Status::Success)return fail(reader.Failed?IdentityPredicate::SlotRead:IdentityPredicate::SlotValues);
        out.Serial=slot.Serial;out.ItemFlags=slot.Flags;out.SlotObserved=true;
        if(slot.Object!=object)return fail(IdentityPredicate::SlotMismatch);
        if(slot.Serial==0)return fail(IdentityPredicate::ZeroSerial);
        if(slot.Serial<0)return fail(IdentityPredicate::NegativeSerial);
        if(slot.Flags&UnusableObjectItemFlags)return fail(IdentityPredicate::SlotFlags);
        index=copiedIndex;serial=slot.Serial;out.Predicate=IdentityPredicate::Stable;return true;
    }
    template<class Ops> Status Check(Ops& ops,uintptr_t array,const Token& token,Slot& slot)
    {
        auto status=ReadSlot(ops,array,token.Index,slot);if(status!=Status::Success)return status;
        if(slot.Serial!=token.Serial)return Status::RecycledSlot;
        if(!slot.Object)return Status::NotResident;
        if(slot.Flags&UnusableObjectItemFlags)return Status::GarbageOrDestroying;
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
