#pragma once
#include <cstdint>
#include <cstddef>

namespace MiniMapDefinitionAcquisition
{
    // FUObjectItem internal flags, NOT UObject::EObjectFlags. Verified against
    // UE_5.6 CoreUObject/Public/UObject/ObjectMacros.h EInternalObjectFlags;
    // these are the existing CL-127004 identity mask bits. Garbage must not be
    // referenced; Unreachable is outside the live object graph. Preserve both.
    enum class ObjectItemFlag : uint32_t { Garbage=uint32_t(1)<<21, Unreachable=uint32_t(1)<<28 };
    inline constexpr uint32_t UnusableObjectItemFlags=
        uint32_t(ObjectItemFlag::Garbage)|uint32_t(ObjectItemFlag::Unreachable);
    enum class Status
    {
        Success = 0, InvalidToken = 1, FacilitiesUnavailable = 2,
        InvalidTable = 6, OutOfBounds = 7, UnreadableItem = 8
    };
    inline const char* Label(Status s) noexcept
    {
        switch (s) {
        case Status::Success: return "success";
        case Status::InvalidToken: return "invalid_token";
        case Status::FacilitiesUnavailable: return "facilities_unavailable";
        case Status::InvalidTable: return "invalid_table";
        case Status::OutOfBounds: return "out_of_bounds";
        case Status::UnreadableItem: return "unreadable_item";
        default: return "interrupted";
        }
    }
    struct Slot {uintptr_t Object{},Item{};int32_t Serial{};uint32_t Flags{};};
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
}
