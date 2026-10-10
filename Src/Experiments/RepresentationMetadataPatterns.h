#pragma once
namespace MiniMapBuildingRepresentation::Patterns
{
    // CL-127004 UObjectBase::IsValidLowLevel. Used ONLY as a RIP-relative
    // GUObjectArray anchor; never called on diagnostic pointers.
    inline constexpr char ObjectArrayAnchor[]="40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 1B 48 8B D3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 08 B0 01 48 83 C4 20 5B C3 32 C0";
    // USkeletalMeshComponent::GetSkeletalMeshAsset: fixed reference getter,
    // RCX=this, RAX=borrowed USkeletalMesh*. No returned array or ownership.
    inline constexpr char SkeletalAsset[]="40 53 48 83 EC 20 48 8B 99 88 05 00 00 48 85 DB 75 2A 80 3D ?? ?? ?? ?? 00 48 8B 99 90 05 00 00 74 0D 48 85 DB 74 0D 48 8B CB E8 ?? ?? ?? ?? 48 85 DB";
}
