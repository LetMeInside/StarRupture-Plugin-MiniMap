#include "Fingerprints.h"

#include "../plugin_helpers.h"
#include "NativeApi.h"
#include <array>
#include <cstring>

// ---------------------------------------------------------------------------
// Native pattern resolution.
//
// This is the only place where MiniMap defines and resolves native fingerprints.
// Resolved native addresses remain valid after this callback; the scanner
// itself must not be retained.
//
// Each fingerprint is intentionally tied to native behavior MiniMap depends on.
// If StarRupture changes an incompatible implementation or layout, resolution
// should fail rather than allowing MiniMap to continue with stale assumptions.
// ---------------------------------------------------------------------------

namespace MiniMapFingerprints
{
    bool Resolve(
        MiniMap::Services::IRequiredScanner& scanner,
        ResolvedAddresses& addresses)
    {
        addresses = {};

        using MiniMap::Services::AddressKind;
        using MiniMap::Services::RequiredScan;
        bool scanFailed = false;
        const auto resolveRequired = [&](const RequiredScan& request) -> uintptr_t
        {
            const auto result = scanner.Resolve(request);
            if (result.status.error != MiniMap::Services::Error::None ||
                !result.value || !*result.value)
            {
                scanFailed = true;
                return 0;
            }
            return *result.value;
        };

        // UCommonActivatableWidget native lifecycle callbacks. These are the
        // shared post-transition virtual implementations used by the three
        // blocking game menus; reflected ActivateWidget/DeactivateWidget
        // dispatch is not sufficient because CommonUI calls them natively.
        RequiredScan commonUIActivatedRequest{};
        commonUIActivatedRequest.name =
            "MiniMap::UCommonActivatableWidget::NativeOnActivated";
        commonUIActivatedRequest.idaPattern =
            "40 53 48 83 EC 50 80 B9 50 04 00 00 00 48 8B D9 75 17 "
            "48 89 4C 24 60 48 8D 4C 24 60 E8 ?? ?? ?? ?? 84 C0 "
            "0F 84 ?? ?? ?? ?? 80 BB 30 05 00 00 00 0F 84 ?? ?? ?? ?? "
            "48 8B 03 48 8B CB 0F B6 93 31 05 00 00 FF 90 C0 02 00 00";
        commonUIActivatedRequest.kind = AddressKind::FunctionEntry;
        addresses.commonUIWidgetNativeOnActivated = resolveRequired(commonUIActivatedRequest);
        if (addresses.commonUIWidgetNativeOnActivated == 0)
        {
            return false;
        }

        RequiredScan commonUIDeactivatedRequest{};
        commonUIDeactivatedRequest.name =
            "MiniMap::UCommonActivatableWidget::NativeOnDeactivated";
        commonUIDeactivatedRequest.idaPattern =
            "40 53 48 83 EC 50 80 B9 50 04 00 00 00 48 8B D9 74 37 "
            "48 8D 05 ?? ?? ?? ?? 41 B9 2C 01 00 00 4C 8D 05 ?? ?? ?? ?? "
            "48 89 44 24 20 33 D2 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
            "84 C0 0F 84 ?? ?? ?? ??";
        commonUIDeactivatedRequest.kind = AddressKind::FunctionEntry;
        addresses.commonUIWidgetNativeOnDeactivated = resolveRequired(commonUIDeactivatedRequest);
        if (addresses.commonUIWidgetNativeOnDeactivated == 0)
        {
            return false;
        }



        // -------------------------------------------------------------------
        // FSoftObjectPtr::LoadSynchronous
        //
        // Fingerprint the StarRupture terrain-loading call site in:
        //
        //   UCrUW_MapMenuTerrain::NativeConstruct
        //
        // NativeConstruct obtains the UCrMapMenuDevSettings class default
        // object and passes CDO + 0x3C8 to FSoftObjectPtr::LoadSynchronous.
        //
        // AlienX follows the relative E8 call and returns the target function
        // address.
        // -------------------------------------------------------------------

        RequiredScan loadSynchronousRequest{};

        loadSynchronousRequest.name =
            "MiniMap::FSoftObjectPtr::LoadSynchronous";

        loadSynchronousRequest.idaPattern =
            "48 8B 8B 10 01 00 00 "
            "48 85 C9 "
            "0F 84 ?? ?? ?? ?? "
            "48 81 C1 C8 03 00 00 "
            "48 89 7C 24 48 "
            "E8 ?? ?? ?? ?? "
            "48 8B F8 "
            "48 85 C0";

        loadSynchronousRequest.followRel32At = 0x1C;


        loadSynchronousRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t loadSynchronousAddress =
            resolveRequired(loadSynchronousRequest);

        if (loadSynchronousAddress == 0)
        {
            return false;
        }

        addresses.softObjectLoadSynchronous =
            loadSynchronousAddress;

        // -------------------------------------------------------------------
        // UWidgetBlueprintLibrary::GetBrushResourceAsTexture2D
        //
        // Verified current-build RVA:
        //
        //   0x04332AE0
        // -------------------------------------------------------------------

        RequiredScan brushTextureRequest{};

        brushTextureRequest.name =
            "MiniMap::UWidgetBlueprintLibrary::"
            "GetBrushResourceAsTexture2D";

        brushTextureRequest.idaPattern =
            "40 53 "
            "48 83 EC ?? "
            "48 8B 59 38 "
            "48 85 DB "
            "74 ?? "
            "E8 ?? ?? ?? ?? "
            "48 8B 53 10 "
            "4C 8D 40 30 "
            "48 63 40 38";


        brushTextureRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t brushTextureAddress =
            resolveRequired(brushTextureRequest);

        if (brushTextureAddress == 0)
        {
            return false;
        }

        addresses.getBrushResourceAsTexture2D =
            brushTextureAddress;

        // -------------------------------------------------------------------
        // UStreamableRenderAsset::SetForceMipLevelsToBeResident
        //
        // Verified current-build RVA:
        //
        //   0x0528FA10
        // -------------------------------------------------------------------

        RequiredScan forceMipsRequest{};

        forceMipsRequest.name =
            "MiniMap::UStreamableRenderAsset::"
            "SetForceMipLevelsToBeResident";

        forceMipsRequest.idaPattern =
            "48 89 5C 24 ?? "
            "57 "
            "48 83 EC ?? "
            "48 8B 01 "
            "41 8B F8 "
            "0F 29 74 24 ?? "
            "48 8B D9 "
            "0F 28 F1 "
            "FF 90 B0 02 00 00 "
            "85 FF "
            "74 ?? "
            "85 C0 "
            "78 ?? "
            "48 98 "
            "48 83 F8 20 "
            "73 ??";


        forceMipsRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t forceMipsAddress =
            resolveRequired(forceMipsRequest);

        if (forceMipsAddress == 0)
        {
            return false;
        }

        addresses.setForceMipLevelsToBeResident =
            forceMipsAddress;

        // -------------------------------------------------------------------
        // UStreamableRenderAsset::WaitForStreaming
        //
        // Verified current-build RVA:
        //
        //   0x0529DE10
        // -------------------------------------------------------------------

        RequiredScan waitForStreamingRequest{};

        waitForStreamingRequest.name =
            "MiniMap::UStreamableRenderAsset::WaitForStreaming";

        waitForStreamingRequest.idaPattern =
            "48 89 5C 24 ?? "
            "48 89 74 24 ?? "
            "57 "
            "48 83 EC ?? "
            "41 0F B6 F8 "
            "0F B6 F2 "
            "48 8B D9 "
            "E8 ?? ?? ?? ?? "
            "83 BB B8 00 00 00 FF "
            "74 ??";


        waitForStreamingRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t waitForStreamingAddress =
            resolveRequired(waitForStreamingRequest);

        if (waitForStreamingAddress == 0)
        {
            return false;
        }

        addresses.waitForStreaming =
            waitForStreamingAddress;

        // -------------------------------------------------------------------
        // UTexture2D::GetNumResidentMips
        //
        // Verified current-build RVA:
        //
        //   0x052DF460
        // -------------------------------------------------------------------

        RequiredScan residentMipsRequest{};

        residentMipsRequest.name =
            "MiniMap::UTexture2D::GetNumResidentMips";

        residentMipsRequest.idaPattern =
            "40 53 "
            "48 83 EC ?? "
            "48 8B D9 "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 "
            "74 ?? "
            "48 8B 03 "
            "48 8B CB "
            "FF 90 48 03 00 00 "
            "84 C0 "
            "74 ??";


        residentMipsRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t residentMipsAddress =
            resolveRequired(residentMipsRequest);

        if (residentMipsAddress == 0)
        {
            return false;
        }

        addresses.getNumResidentMips =
            residentMipsAddress;

        // -------------------------------------------------------------------
        // UTexture2D::GetNumMipsAllowed
        //
        // Verified current-build RVA:
        //
        //   0x052DF280
        // -------------------------------------------------------------------

        RequiredScan allowedMipsRequest{};

        allowedMipsRequest.name =
            "MiniMap::UTexture2D::GetNumMipsAllowed";

        allowedMipsRequest.idaPattern =
            "48 89 5C 24 ?? "
            "57 "
            "48 83 EC ?? "
            "48 8B D9 "
            "E8 ?? ?? ?? ?? "
            "33 C9 "
            "8B F8 "
            "E8 ?? ?? ?? ?? "
            "48 8B C8 "
            "E8 ?? ?? ?? ?? "
            "48 8B C8 "
            "E8 ?? ?? ?? ?? "
            "45 33 C0 "
            "48 8B D3 "
            "48 8B C8 "
            "E8 ?? ?? ?? ??";


        allowedMipsRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t allowedMipsAddress =
            resolveRequired(allowedMipsRequest);

        if (allowedMipsAddress == 0)
        {
            return false;
        }

        addresses.getNumMipsAllowed =
            allowedMipsAddress;

        // -------------------------------------------------------------------
        // UTexture2D::GetNumMips
        //
        // Verified current-build RVA:
        //
        //   0x052DF1E0
        // -------------------------------------------------------------------

        RequiredScan numMipsRequest{};

        numMipsRequest.name =
            "MiniMap::UTexture2D::GetNumMips";

        numMipsRequest.idaPattern =
            "40 53 "
            "48 83 EC ?? "
            "48 83 B9 50 01 00 00 00 "
            "48 8B D9 "
            "74 ?? "
            "48 8B 01 "
            "FF 90 48 03 00 00 "
            "48 8B 9B 50 01 00 00";


        numMipsRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t numMipsAddress =
            resolveRequired(numMipsRequest);

        if (numMipsAddress == 0)
        {
            return false;
        }

        addresses.getNumMips =
            numMipsAddress;

        // -------------------------------------------------------------------
        // UTexture2D::StreamIn
        //
        // Verified current-build RVA:
        //
        //   0x052E7420
        //
        // Signature:
        //
        //   bool UTexture2D::StreamIn(
        //       int32 NewMipCount,
        //       bool bHighPrio)
        //
        // The fingerprint includes the native game-thread check.
        // -------------------------------------------------------------------

        RequiredScan streamInRequest{};

        streamInRequest.name =
            "MiniMap::UTexture2D::StreamIn";

        streamInRequest.idaPattern =
            "40 53 "
            "55 "
            "56 "
            "57 "
            "41 54 "
            "48 83 EC ?? "
            "45 0F B6 E0 "
            "8B EA "
            "48 8B F9 "
            "E8 ?? ?? ?? ?? "
            "84 C0 "
            "75 ??";


        streamInRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t streamInAddress =
            resolveRequired(streamInRequest);

        if (streamInAddress == 0)
        {
            return false;
        }

        addresses.streamIn =
            streamInAddress;

        // -------------------------------------------------------------------
        // UStreamableRenderAsset::WaitForPendingInitOrStreaming
        //
        // Verified current-build RVA:
        //
        //   0x0529DD40
        //
        // Signature:
        //
        //   void WaitForPendingInitOrStreaming(
        //       bool bWaitForLODTransition,
        //       bool bSendCompletionEvents)
        // -------------------------------------------------------------------

        RequiredScan waitPendingRequest{};

        waitPendingRequest.name =
            "MiniMap::UStreamableRenderAsset::"
            "WaitForPendingInitOrStreaming";

        waitPendingRequest.idaPattern =
            "48 89 5C 24 ?? "
            "48 89 74 24 ?? "
            "57 "
            "48 83 EC ?? "
            "41 0F B6 F0 "
            "0F B6 FA "
            "48 8B D9 "
            "E8 ?? ?? ?? ?? "
            "84 C0 "
            "0F 84 ?? ?? ?? ??";


        waitPendingRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t waitPendingAddress =
            resolveRequired(waitPendingRequest);

        if (waitPendingAddress == 0)
        {
            return false;
        }

        addresses.waitForPendingInitOrStreaming =
            waitPendingAddress;

        // -------------------------------------------------------------------
        // UWorld::GetFirstPlayerController
        //
        // Verified current-build RVA:
        //
        //   0x05386A20
        //
        // Signature:
        //
        //   APlayerController*
        //   UWorld::GetFirstPlayerController() const
        // -------------------------------------------------------------------

        RequiredScan firstControllerRequest{};

        firstControllerRequest.name =
            "MiniMap::UWorld::GetFirstPlayerController";

        firstControllerRequest.idaPattern =
            "40 53 "
            "48 83 EC ?? "
            "83 B9 78 02 00 00 00 "
            "48 8B D9 "
            "7E ?? "
            "48 63 81 78 02 00 00 "
            "85 C0 "
            "7F ??";


        firstControllerRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t firstControllerAddress =
            resolveRequired(firstControllerRequest);

        if (firstControllerAddress == 0)
        {
            return false;
        }

        addresses.getFirstPlayerController =
            firstControllerAddress;

        // -------------------------------------------------------------------
        // USceneComponent::K2_GetComponentLocation
        //
        // Verified current-build RVA:
        //
        //   0x049001F0
        //
        // This is an ordinary native leaf function despite its K2 name.
        //
        // It has no RUNTIME_FUNCTION entry, so AddressKind::FunctionEntry
        // would reject it. The unique signature is therefore validated as
        // executable code with AddressKind::Code.
        // -------------------------------------------------------------------

        RequiredScan componentLocationRequest{};

        componentLocationRequest.name =
            "MiniMap::USceneComponent::K2_GetComponentLocation";

        componentLocationRequest.idaPattern =
            "0F 10 81 10 02 00 00 "
            "48 8B C2 "
            "0F 10 89 20 02 00 00 "
            "0F 11 02 "
            "F2 0F 11 4A 10 "
            "C3";


        componentLocationRequest.kind =
            AddressKind::Code;

        const uintptr_t componentLocationAddress =
            resolveRequired(componentLocationRequest);

        if (componentLocationAddress == 0)
        {
            return false;
        }

        addresses.getComponentLocation =
            componentLocationAddress;

        // -------------------------------------------------------------------
// UTexture2D::GetPlatformData
//
// Verified against:
//
//   ++Earth20+Neon-HF2.5-CL-126119
//
// Target RVA:
//
//   0x03824E90
//
// The target itself is only:
//
//   mov rax,[rcx+150h]
//   ret
//
// That byte sequence is not unique, so fingerprint a verified call
// site in IsTextureDataValid and follow its E8.
//
// ABI:
//
//   RCX = UTexture2D*
//   RAX = FTexturePlatformData*
//
// The target is a leaf without unwind metadata, so validate it as
// executable code rather than as a function start.
// -------------------------------------------------------------------

        RequiredScan getPlatformDataRequest{};

        getPlatformDataRequest.name =
            "MiniMap::UTexture2D::GetPlatformData";

        getPlatformDataRequest.idaPattern =
            "8B FE "
            "48 85 C9 "
            "74 ?? "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 "
            "74 ?? "
            "48 8D 54 24 ?? "
            "48 8B CB";

        getPlatformDataRequest.followRel32At = 7;


        getPlatformDataRequest.kind =
            AddressKind::Code;

        const uintptr_t getPlatformDataAddress =
            resolveRequired(getPlatformDataRequest);

        if (getPlatformDataAddress == 0)
        {
            return false;
        }

        addresses.getPlatformData =
            getPlatformDataAddress;


        // -------------------------------------------------------------------
        // FBulkData::GetBulkDataSize
        //
        // Verified target RVA:
        //
        //   0x015C12B0
        //
        // ABI:
        //
        //   RCX = const FBulkData*
        //   RAX = int64 byte size
        //
        // Leaf function without unwind metadata.
        // -------------------------------------------------------------------

        RequiredScan getBulkDataSizeRequest{};

        getBulkDataSizeRequest.name =
            "MiniMap::FBulkData::GetBulkDataSize";

        getBulkDataSizeRequest.idaPattern =
            "44 0F B6 49 0C "
            "4C 8B C1 "
            "0F B6 41 0B "
            "0F B6 51 0A "
            "0F B6 49 09 "
            "49 C1 E1 08 "
            "49 0B C1 "
            "48 C1 E0 08 "
            "48 0B C2";


        getBulkDataSizeRequest.kind =
            AddressKind::Code;

        const uintptr_t getBulkDataSizeAddress =
            resolveRequired(getBulkDataSizeRequest);

        if (getBulkDataSizeAddress == 0)
        {
            return false;
        }

        addresses.getBulkDataSize =
            getBulkDataSizeAddress;


        // -------------------------------------------------------------------
        // FBulkData::CanLoadFromDisk
        //
        // Verified target RVA:
        //
        //   0x015BE710
        //
        // ABI:
        //
        //   RCX = const FBulkData*
        //   AL  = bool
        //
        // Leaf function without unwind metadata.
        // -------------------------------------------------------------------

        RequiredScan canLoadFromDiskRequest{};

        canLoadFromDiskRequest.name =
            "MiniMap::FBulkData::CanLoadFromDisk";

        canLoadFromDiskRequest.idaPattern =
            "48 8B 51 18 "
            "48 2B 15 ?? ?? ?? ?? "
            "75 ?? "
            "8B 51 20 "
            "8B 05 ?? ?? ?? ?? "
            "48 2B D0 "
            "48 85 D2 "
            "0F 95 C0 "
            "C3";


        canLoadFromDiskRequest.kind =
            AddressKind::Code;

        const uintptr_t canLoadFromDiskAddress =
            resolveRequired(canLoadFromDiskRequest);

        if (canLoadFromDiskAddress == 0)
        {
            return false;
        }

        addresses.canLoadFromDisk =
            canLoadFromDiskAddress;


        // -------------------------------------------------------------------
        // FBulkData::GetCopy
        //
        // Verified target RVA:
        //
        //   0x015C12F0
        //
        // Signature:
        //
        //   void FBulkData::GetCopy(
        //       void** Dest,
        //       bool bDiscardInternalCopy)
        //
        // ABI:
        //
        //   RCX  = FBulkData*
        //   RDX  = void** Dest
        //   R8B  = bDiscardInternalCopy
        //
        // This has valid unwind metadata and is fingerprinted at the native
        // function entry.
        // -------------------------------------------------------------------

        RequiredScan getBulkDataCopyRequest{};

        getBulkDataCopyRequest.name =
            "MiniMap::FBulkData::GetCopy";

        getBulkDataCopyRequest.idaPattern =
            "48 89 5C 24 ?? "
            "55 "
            "56 "
            "57 "
            "48 81 EC ?? ?? ?? ?? "
            "48 8B 05 ?? ?? ?? ?? "
            "48 33 C4 "
            "48 89 84 24 ?? ?? ?? ?? "
            "80 79 13 00 "
            "41 0F B6 E8 "
            "48 8B F2 "
            "48 8B D9";


        getBulkDataCopyRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t getBulkDataCopyAddress =
            resolveRequired(getBulkDataCopyRequest);

        if (getBulkDataCopyAddress == 0)
        {
            return false;
        }

        addresses.getBulkDataCopy =
            getBulkDataCopyAddress;


        // -------------------------------------------------------------------
        // FMemory::Free
        //
        // Verified target RVA:
        //
        //   0x01334660
        //
        // ABI:
        //
        //   RCX = allocation
        //
        // Allocation returned by FBulkData::GetCopy must be released through
        // this function, not CRT free/delete[].
        //
        // This is a native function start with unwind metadata.
        // -------------------------------------------------------------------

        RequiredScan memoryFreeRequest{};

        memoryFreeRequest.name =
            "MiniMap::FMemory::Free";

        memoryFreeRequest.idaPattern =
            "48 85 C9 "
            "74 ?? "
            "53 "
            "48 83 EC ?? "
            "48 8B D9 "
            "48 8B 0D ?? ?? ?? ?? "
            "48 85 C9 "
            "75 ?? "
            "E8 ?? ?? ?? ?? "
            "48 8B 0D ?? ?? ?? ?? "
            "48 8B 01 "
            "48 8B D3 "
            "FF 50 48";


        memoryFreeRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t memoryFreeAddress =
            resolveRequired(memoryFreeRequest);

        if (memoryFreeAddress == 0)
        {
            return false;
        }

        addresses.memoryFree =
            memoryFreeAddress;


        // -------------------------------------------------------------------
        // ACrCharacterPlayerBase::IsPlayerInForgottenEngine
        //
        // Verified against HF2.5-CL-126119.
        //
        // Native target:
        //
        //   bool ACrCharacterPlayerBase::IsPlayerInForgottenEngine() const
        //   RVA 0x075712D0
        //
        // This fingerprint is anchored on:
        //
        //   UCrUW_MapMenuMapArea::CenterOnPlayerLocation
        //
        // The call-site path selects UCrMapMenuDevSettings::
        // ForgottenEngineMarkerLocation when the predicate returns true and
        // otherwise continues to the pawn RootComponent position path.
        //
        // The complete pattern is unique in the verified shipping EXE.
        // AlienX follows the E8 at offset 3 and validates the destination as a
        // native function start.
        // -------------------------------------------------------------------

        RequiredScan forgottenEngineRequest{};

        forgottenEngineRequest.name =
            "MiniMap::ACrCharacterPlayerBase::"
            "IsPlayerInForgottenEngine";

        forgottenEngineRequest.idaPattern =
            "48 8B CB "
            "E8 ?? ?? ?? ?? "
            "84 C0 "
            "74 ?? "
            "F2 0F 10 47 5C "
            "8B 47 64 "
            "F2 0F 11 44 24 ?? "
            "89 44 24 ?? "
            "EB ?? "
            "48 8B 83 B8 01 00 00";

        forgottenEngineRequest.followRel32At =
            3;


        forgottenEngineRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t forgottenEngineAddress =
            resolveRequired(forgottenEngineRequest);

        if (forgottenEngineAddress == 0)
        {
            return false;
        }

        addresses.isPlayerInForgottenEngine =
            forgottenEngineAddress;


        // -------------------------------------------------------------------
        // AController::GetControlRotation
        //
        // Verified against HF2.5-CL-126119.
        //
        // Native target:
        //
        //   FRotator AController::GetControlRotation() const
        //   RVA 0x04821C60
        //
        // The complete 27-byte leaf body is unique in the verified shipping
        // EXE. It has no RUNTIME_FUNCTION entry, so validate it as executable
        // code rather than as an unwind-backed function start.
        // -------------------------------------------------------------------

        RequiredScan controlRotationRequest{};

        controlRotationRequest.name =
            "MiniMap::AController::GetControlRotation";

        controlRotationRequest.idaPattern =
            "0F 10 81 38 03 00 00 "
            "48 8B C2 "
            "F2 0F 10 89 48 03 00 00 "
            "0F 11 02 "
            "F2 0F 11 4A 10 "
            "C3";

        controlRotationRequest.kind =
            AddressKind::Code;

        const uintptr_t controlRotationAddress =
            resolveRequired(controlRotationRequest);

        if (controlRotationAddress == 0)
        {
            return false;
        }

        addresses.getControlRotation =
            controlRotationAddress;

        RequiredScan getAllActorsRequest{};

        getAllActorsRequest.name =
            "MiniMap::UGameplayStatics::GetAllActorsOfClass";

        getAllActorsRequest.idaPattern =
            "4C 8D 44 24 30 4C 89 A4 24 28 01 00 00 "
            "48 8D 54 24 58 4C 89 AC 24 20 01 00 00 "
            "49 8B CE 4C 89 BC 24 18 01 00 00 "
            "E8 ?? ?? ?? ?? "
            "48 63 44 24 38 85 C0 0F 84 ?? ?? ?? ??";

        getAllActorsRequest.followRel32At = 0x25;

        getAllActorsRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t getAllActorsAddress =
            resolveRequired(getAllActorsRequest);

        if (getAllActorsAddress == 0)
        {
            return false;
        }

        addresses.getAllActorsOfClass =
            getAllActorsAddress;


        RequiredScan poiStaticClassRequest{};

        poiStaticClassRequest.name =
            "MiniMap::ACrPointOfInterestMarkerActor::StaticClass";

        poiStaticClassRequest.idaPattern =
            "48 C7 44 24 30 00 00 00 00 "
            "48 C7 44 24 38 00 00 00 00 "
            "E8 ?? ?? ?? ?? "
            "80 3D ?? ?? ?? ?? 00 "
            "48 89 44 24 58 74 ?? 48 85 C0 74 ?? "
            "48 8B C8 E8 ?? ?? ?? ?? "
            "48 89 B4 24 38 01 00 00 "
            "4C 8D 44 24 30";

        poiStaticClassRequest.followRel32At = 0x12;

        poiStaticClassRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t poiStaticClassAddress =
            resolveRequired(poiStaticClassRequest);

        if (poiStaticClassAddress == 0)
        {
            return false;
        }

        addresses.pointOfInterestStaticClass =
            poiStaticClassAddress;


        RequiredScan poiCategoryRequest{};

        poiCategoryRequest.name =
            "MiniMap::UCrMapMenuDevSettings::FindPOIMarkerCategoryData";

        poiCategoryRequest.idaPattern =
            "48 8B 8B 10 01 00 00 48 85 C9 74 ?? "
            "0F B6 96 E9 02 00 00 "
            "E8 ?? ?? ?? ?? "
            "4C 8B E8 EB ?? 45 33 ED "
            "49 83 BE C0 03 00 00 00";

        poiCategoryRequest.followRel32At = 0x13;

        poiCategoryRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t poiCategoryAddress =
            resolveRequired(poiCategoryRequest);

        if (poiCategoryAddress == 0)
        {
            return false;
        }

        addresses.findPOIMarkerCategoryData =
            poiCategoryAddress;


        RequiredScan markerFilterRequest{};

        markerFilterRequest.name =
            "MiniMap::UCrPlayerMapMenuDataComponent::"
            "GetMapMenuMarkerFiltersOnOffStatus";

        markerFilterRequest.idaPattern =
            "48 89 5C 24 08 57 48 83 EC ?? "
            "0F B6 FA 48 8B D9 "
            "3B B9 18 01 00 00 7D ?? "
            "48 63 89 18 01 00 00 3B F9 7C ??";

        markerFilterRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t markerFilterAddress =
            resolveRequired(markerFilterRequest);

        if (markerFilterAddress == 0)
        {
            return false;
        }

        addresses.getMapMenuMarkerFilterStatus =
            markerFilterAddress;


        // -------------------------------------------------------------------
        // FCrAbandonBaseData::IsCompleted
        //
        // Anchored in UCrUW_MapMenuMapArea::UpdatePOIMarkers.
        //
        // The same native branch first looks up abandoned-base data and then
        // tests completion. We intentionally resolve only IsCompleted here:
        // MiniMap pre-scans the replicated array by GUID, avoiding the native
        // FindAbandonBaseData ensure path for absent records.
        //
        // Verified current-build RVA: 0x073C1360
        //
        // This leaf function has no RUNTIME_FUNCTION entry, so CODE
        // validation is required.
        // -------------------------------------------------------------------

        RequiredScan abandonBaseCompletedRequest{};

        abandonBaseCompletedRequest.name =
            "MiniMap::FCrAbandonBaseData::IsCompleted";

        abandonBaseCompletedRequest.idaPattern =
            "48 8B 44 24 70 48 8D 55 B0 "
            "48 8B 88 A8 03 00 00 "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 0F 84 ?? ?? ?? ?? "
            "48 8B C8 E8 ?? ?? ?? ?? "
            "84 C0 74 ?? 4C 63 43 08";

        abandonBaseCompletedRequest.followRel32At =
            0x21;


        abandonBaseCompletedRequest.kind =
            AddressKind::Code;

        const uintptr_t abandonBaseCompletedAddress =
            resolveRequired(abandonBaseCompletedRequest);

        if (abandonBaseCompletedAddress == 0)
        {
            return false;
        }

        addresses.isAbandonBaseCompleted =
            abandonBaseCompletedAddress;
        // -------------------------------------------------------------------
        // Foundable category and callback-free Mass query support.
        // -------------------------------------------------------------------

        RequiredScan foundableCategoryRequest{};

        foundableCategoryRequest.name =
            "MiniMap::UCrMapMenuDevSettings::"
            "FindFoundableMarkerCateroryData";

        foundableCategoryRequest.idaPattern =
            "48 8B 8C 24 D0 00 00 00 41 0F B6 14 24 48 8B 49 08 "
            "E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
            "40 38 78 30 0F 85 ?? ?? ?? ?? 49 8B EE 45 85 F6";

        foundableCategoryRequest.followRel32At = 0x11;

        foundableCategoryRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t foundableCategoryAddress =
            resolveRequired(foundableCategoryRequest);

        if (foundableCategoryAddress == 0)
        {
            return false;
        }

        addresses.findFoundableMarkerCategoryData =
            foundableCategoryAddress;


#if defined(MODLOADER_CLIENT_BUILD)
        // Shared engine ownership/name APIs are required by minimal acquisition,
        // independently of private-world capture diagnostics. Existing patterns.
        {
            RequiredScan request{};
            request.name = "MiniMap::Retain";
            request.idaPattern = "40 53 48 83 EC 30 8B 59 0C 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 0D 04 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 3B D8 0F 8D ?? ?? ?? ?? 48 89 74 24 40 48 89 7C 24 48 8B FB C1 EF 10 0F B7 F3 3B D8 7D ?? 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 89 5C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 72 ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 7C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B D8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 00 89 5C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 1C F8 48 8B 7C 24 48 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8D 0C 76 48 8B 74 24 40 48 8D 1C CB 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 4B 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 33 DB F0 FF 43 14 8B 43 08 0F BA E0 1D 72 ?? BA 00 00 00 20 48 8B CB 48 83 C4 30 5B E9 ?? ?? ?? ?? 48 83 C4 30 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 10 57 48 83 EC 20 48 8B 02 48 8B FA 48 8B D9 48 8D 51 78 45 33 C9 45 33 C0 48 8B CF FF 90 88 00 00 00 F6 43 10 01 74 ?? 48 8B 43 10 48 83 E0 FE EB ?? 33 C0 80 3D ?? ?? ?? ?? 00 48 89 44 24 30 74 ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 44 24 30 48 85 C0 74 ?? 48 8B 07";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.retainObject = resolveRequired(request);
            if (!addresses.retainObject) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Release";
            request.idaPattern = "80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? C3 CC CC 40 53 55 56 57 41 54 41 56 41 57 48 83 EC 50 48 8B EA 4C 8B F9 48 8B D1 49 8B D8 48 8D 4C 24 30 E8 ?? ?? ?? ?? 4D 8B 67 08 49 8D 77 08 4D 63 77 10 49 8B FC 49 C1 E6 04 4D 03 F4 4D 3B E6 74 ?? 33 C0 88 84 24 90 00 00 00 48 39 5F 08 75 ?? 88 84 24 98 00 00 00 48 85 ED 74 ?? 48 8B CF 88 84 24 A0 00 00 00 E8 ?? ?? ?? ?? 48 3B E8 74 ?? 48 83 C7 10 49 3B FE 75 ?? 49 8D 77 08 41 B0 01 48 8D 94 24 A8 00 00 00 48 8B CE E8 ?? ?? ?? ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 83 C4 50 41 5F 41 5E 41 5C 5F 5E 5D 5B C3 49 2B FC 49 8D 77 08 48 C1 FF 04 83 FF FF 74 ?? 48 63 56 08 48 8D 5E 08 33 C0 8B CF F7 D1 48 8B EE C1 E9 1F 3B FA 0F 4C C1 85 C0 75 ?? 48 89 54 24 28 4C 8D 0D ?? ?? ?? ?? 48 63 CF 48 8D 15 ?? ?? ?? ?? 48 89 4C 24 20 41 B8 2B 04 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 49 8D 6F 08 8B 0B BA 01 00 00 00 4C 8B 0E 8B C1 2B C7 FF C8 3B C2 0F 4F C2 85 C0 74 ?? 2B C8 4C 63 C0 48 63 D1 48 C1 E2 04 48 63 CF 49 03 D1 48 C1 E1 04 49 03 C9 49 C1 E0 04 E8 ?? ?? ?? ?? 48 8B F5 FF 0B E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 18 55 48 83 EC 20 8B 41 08 48 8B EA 48 8B D9 3B 41 34 0F 84 ?? ?? ?? ?? 4C 8B C2 48 89 74 24 30 49 C1 E8 04 41 8B C0 48 89 7C 24 38 C1 E8 10 33 FF 41 33 C0 4C 8B 43 40 69 D0 6B CA EB 85 8B C2 C1 E8 0D 33 C2 69 C8 35 AE B2 C2 48 63 C1 8B D1 48 C1 EA 10 48 33 D0 8B 43 48 FF C8 48 63 C8 48 8D 43 38 48 23 D1 4D 85 C0 4C 0F 44 C0 41 8B 04 90 49 8D 34 90 83 F8 FF 74 ?? 66 90 48 63 C8 48 8D 14 89 48 8B 0B 48 39 2C D1 4C 8D 04 D1";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.releaseObject = resolveRequired(request);
            if (!addresses.releaseObject) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::FName::EqualsWide";
            request.idaPattern = "45 33 C0 48 8B C2 48 85 D2 74 13 66 44 39 02 74 0D 48 8D 40 02 41 FF C0 66 83 38 00 75 F3 E9 0D 65 FF FF CC CC CC CC CC CC CC CC CC CC CC CC CC 44 8B 42 08 48 8B 12 E9 F4 64 FF FF CC CC CC CC";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.nameEqualsWide = resolveRequired(request);
            if (!addresses.nameEqualsWide) return false;
        }
        // Shared thread predicate is also required by the standalone payload adapter.
        {
            RequiredScan request{};
            request.name = "MiniMap::GameThread";
            request.idaPattern = "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 ?? 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 02 74 ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 81 3C 02 02 00 00 40 0F 94 C0 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 8B 04 02 C1 E8 05 24 01 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 83 3D ?? ?? ?? ?? 00 BA D0 09 00 00 75 ?? 65 48 8B 04 25 58 00 00 00 8B 0D ?? ?? ?? ?? 48 8B 0C C8 8B 04 0A 83 F8 10 74 ?? 83 F8 02 74 ?? 3D 10 00 00 40 74 ?? 32 C0 C3 B0 01 C3 8B 05 ?? ?? ?? ?? 65 48 8B 0C 25 58 00 00 00 48 8B 0C C1 8B 04 11 83 E8 10 A9 EF FF FF BF 0F 94 C0 C3 CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 20 0F 94 C0 C3 48 83 EC 28 83 3D ?? ?? ?? ?? 00 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 0C C8 75 ?? 8B 04 0A 83 F8 02 74 ?? 83 F8 10 74 ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 83 3C 0A 10 0F 94 C0 48 83 C4 28 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 04 0F 94 C0 C3 48 8B 49 08 48 8B 01 48 FF A0 B8 00 00 00 CC CC 40 53 48 83 EC 20 48 8B 19 48 85 DB 74 ?? 48 83 BB 90 00 00 00 00 74 ?? FF 15 ?? ?? ?? ?? 48 8B 8B 90 00 00 00 3B 41 34 74 ?? B0 01 48 83 C4 20 5B C3 32 C0 48 83 C4 20 5B C3 CC CC CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.isGameThread = resolveRequired(request);
            if (!addresses.isGameThread) return false;
        }

        // Stage 42G, CL-127004. Mandatory typed native functions, not hooks.
        // The getter's generic forwarding body is not unique. Follow the call
        // at +0x0C in SetActorPlacementPayloadData's unique numerical-copy site.
        RequiredScan splinePayloadRequest{};
        splinePayloadRequest.name = "MiniMap::SplinePayload::GetSplineActorData";
        splinePayloadRequest.idaPattern =
            "48 8D 97 30 03 00 00 48 8D 4C 24 58 E8 ?? ?? ?? ?? "
            "83 BC 24 D0 00 00 00 00 48 8B CF 7E ?? 48 8B 07";
        splinePayloadRequest.kind = AddressKind::FunctionEntry;

        splinePayloadRequest.followRel32At = 0x0C;
        addresses.getSplineActorData = resolveRequired(splinePayloadRequest);
        if (!addresses.getSplineActorData) return false;

        RequiredScan splinePayloadDestroyRequest{};
        splinePayloadDestroyRequest.name = "MiniMap::SplinePayload::Destroy";
        splinePayloadDestroyRequest.idaPattern =
            "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 "
            "48 8D 05 ?? ?? ?? ?? 48 89 41 68 8B 51 78 48 8B 49 70 "
            "E8 ?? ?? ?? ?? 48 8B 4B 70 48 85 C9 74 05 E8 ?? ?? ?? ?? "
            "48 8D 05 ?? ?? ?? ?? 48 89 03 48 8B 4B 50";
        splinePayloadDestroyRequest.kind = AddressKind::FunctionEntry;

        addresses.destroySplineActorPayloadData = resolveRequired(splinePayloadDestroyRequest);
        if (!addresses.destroySplineActorPayloadData) return false;

        // Warm the exact type cache used by GetValue during NativeApi initialization.
        // The fixed name-string LEA displacement distinguishes this generated
        // StaticStruct body in CL-127004; no absolute runtime address is embedded.
        RequiredScan splinePayloadTypeRequest{};
        splinePayloadTypeRequest.name = "MiniMap::SplinePayload::StaticStruct";
        splinePayloadTypeRequest.idaPattern =
            "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 ?? E8 ?? ?? ?? ?? "
            "48 8B D0 4C 8D 05 41 FF B5 05 48 8D 0D ?? ?? ?? ?? "
            "E8 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 83 C4 28 C3";
        splinePayloadTypeRequest.kind = AddressKind::FunctionEntry;

        addresses.splineActorPayloadStaticStruct = resolveRequired(splinePayloadTypeRequest);
        if (!addresses.splineActorPayloadStaticStruct) return false;
#endif

        RequiredScan massSubsystemRequest{};

        massSubsystemRequest.name =
            "MiniMap::UWorld::GetSubsystem<UMassEntitySubsystem>";

        massSubsystemRequest.idaPattern =
            "49 8B CD E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C8 "
            "E8 ?? ?? ?? ?? 48 8B D8 EB ?? 33 DB 48 83 7B 38 00 75 ?? "
            "4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ??";

        massSubsystemRequest.followRel32At = 0x10;

        massSubsystemRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t massSubsystemAddress =
            resolveRequired(massSubsystemRequest);

        if (massSubsystemAddress == 0)
        {
            return false;
        }

        addresses.getMassEntitySubsystem =
            massSubsystemAddress;


        RequiredScan massQueryCtorRequest{};

        massQueryCtorRequest.name =
            "MiniMap::FMassEntityQuery::FMassEntityQuery";

        massQueryCtorRequest.idaPattern =
            "48 8D 54 24 60 48 8D 8D 80 03 00 00 E8 ?? ?? ?? ?? "
            "48 8B 5C 24 68 48 85 DB 74 ?? 41 8B C4 F0 0F C1 43 08";

        massQueryCtorRequest.followRel32At = 0x0C;

        massQueryCtorRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t massQueryCtorAddress =
            resolveRequired(massQueryCtorRequest);

        if (massQueryCtorAddress == 0)
        {
            return false;
        }

        addresses.massQueryConstruct =
            massQueryCtorAddress;


        RequiredScan massRequirementsRequest{};

        massRequirementsRequest.idaPattern =
            "41 B1 01 48 8D 8D D0 06 00 00 0F 28 D6 48 8B D6 "
            "E8 ?? ?? ?? ?? 45 33 C0 48 8D 8D 80 03 00 00 B2 01 "
            "E8 ?? ?? ?? ?? 45 33 C0 48 8D 8D 80 03 00 00 B2 01 "
            "E8 ?? ?? ?? ?? 33 D2 48 8D 8D 80 03 00 00 E8 ?? ?? ?? ??";

        massRequirementsRequest.kind =
            AddressKind::FunctionEntry;

        massRequirementsRequest.name =
            "MiniMap::AddRequirement<FCrInventoryFragment>";
        massRequirementsRequest.followRel32At = 0x21;

        const uintptr_t inventoryRequirementAddress =
            resolveRequired(massRequirementsRequest);

        if (inventoryRequirementAddress == 0)
        {
            return false;
        }

        addresses.addInventoryRequirement =
            inventoryRequirementAddress;

        massRequirementsRequest.name =
            "MiniMap::AddRequirement<FTransformFragment>";
        massRequirementsRequest.followRel32At = 0x32;

        const uintptr_t transformRequirementAddress =
            resolveRequired(massRequirementsRequest);

        if (transformRequirementAddress == 0)
        {
            return false;
        }

        addresses.addTransformRequirement =
            transformRequirementAddress;

        massRequirementsRequest.name =
            "MiniMap::AddConstSharedRequirement<"
            "FCrMassFoundableParameters>";
        massRequirementsRequest.followRel32At = 0x40;

        const uintptr_t foundableParametersRequirementAddress =
            resolveRequired(massRequirementsRequest);

        if (foundableParametersRequirementAddress == 0)
        {
            return false;
        }

        addresses.addFoundableParametersRequirement =
            foundableParametersRequirementAddress;


        RequiredScan foundableTagRequest{};

        foundableTagRequest.name =
            "MiniMap::AddTagRequirement<FCrMassFoundableTag>";

        foundableTagRequest.idaPattern =
            "40 53 48 83 EC 20 48 8B DA 33 D2 48 8B CB E8 ?? ?? ?? ?? "
            "48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
            "45 33 C0 B2 02 48 8B CB E8 ?? ?? ?? ?? "
            "41 B0 03 B2 02 48 8B CB E8 ?? ?? ?? ?? "
            "33 D2 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ??";

        foundableTagRequest.followRel32At = 0x0E;

        foundableTagRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t foundableTagAddress =
            resolveRequired(foundableTagRequest);

        if (foundableTagAddress == 0)
        {
            return false;
        }

        addresses.addFoundableTagRequirement =
            foundableTagAddress;


        RequiredScan massQueryDtorRequest{};

        massQueryDtorRequest.name =
            "MiniMap::FMassEntityQuery::~FMassEntityQuery";

        massQueryDtorRequest.idaPattern =
            "48 8D 8D D0 06 00 00 E8 ?? ?? ?? ?? "
            "48 8D 8D 80 03 00 00 E8 ?? ?? ?? ?? "
            "48 8D 4D 30 E8 ?? ?? ?? ?? "
            "48 8D 8D B0 0B 00 00 E8 ?? ?? ?? ??";

        massQueryDtorRequest.followRel32At = 0x13;

        massQueryDtorRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t massQueryDtorAddress =
            resolveRequired(massQueryDtorRequest);

        if (massQueryDtorAddress == 0)
        {
            return false;
        }

        addresses.massQueryDestruct =
            massQueryDtorAddress;


        RequiredScan matchingHandlesRequest{};

        matchingHandlesRequest.name =
            "MiniMap::FMassEntityQuery::GetMatchingEntityHandles";

        matchingHandlesRequest.idaPattern =
            "40 53 41 54 41 56 41 57 48 83 EC 48 "
            "48 8B D9 4C 8D 62 08 33 C9 4C 8B F2 "
            "48 89 0A 41 89 0C 24 89 4A 0C 48 8B CB "
            "E8 ?? ?? ?? ?? "
            "4C 8B BB F8 02 00 00 48 63 83 00 03 00 00";

        matchingHandlesRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t matchingHandlesAddress =
            resolveRequired(matchingHandlesRequest);

        if (matchingHandlesAddress == 0)
        {
            return false;
        }

        addresses.getMatchingEntityHandles =
            matchingHandlesAddress;


        RequiredScan fragmentDataRequest{};

        fragmentDataRequest.name =
            "MiniMap::FMassEntityManager::"
            "InternalGetFragmentDataPtr";

        fragmentDataRequest.idaPattern =
            "E8 ?? ?? ?? ?? "
            "4C 8B C0 48 8B D3 48 8B CE "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 0F 84 ?? ?? ?? ?? "
            "0F 10 48 20 66 41 0F 2F 4D 30 "
            "0F 10 40 30 0F 11 4D EF F2 0F 11 45 FF";

        fragmentDataRequest.followRel32At = 0x0E;

        fragmentDataRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t fragmentDataAddress =
            resolveRequired(fragmentDataRequest);

        if (fragmentDataAddress == 0)
        {
            return false;
        }

        addresses.getMassFragmentDataPtr =
            fragmentDataAddress;

        fragmentDataRequest.name =
            "MiniMap::FTransformFragment::StaticStruct";
        fragmentDataRequest.followRel32At = 0x00;

        const uintptr_t transformStaticStructAddress =
            resolveRequired(fragmentDataRequest);

        if (transformStaticStructAddress == 0)
        {
            return false;
        }

        addresses.transformFragmentStaticStruct =
            transformStaticStructAddress;


        RequiredScan constSharedDataRequest{};

        constSharedDataRequest.name =
            "MiniMap::FMassEntityManager::"
            "InternalGetConstSharedFragmentPtr";

        constSharedDataRequest.idaPattern =
            "E8 ?? ?? ?? ?? "
            "4C 8B C0 48 8B D3 48 8B CE "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 74 ?? "
            "48 8B 10 48 85 D2 74 ?? "
            "48 8B 02 48 0F BF 48 5C "
            "48 8D 41 07 48 F7 D9 48 03 C2 48 23 C1 "
            "74 ?? "
            "F3 0F 10 00 48 8B 4D 6F F3 0F 58 01 F3 0F 11 01";

        constSharedDataRequest.followRel32At = 0x0E;

        constSharedDataRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t constSharedDataAddress =
            resolveRequired(constSharedDataRequest);

        if (constSharedDataAddress == 0)
        {
            return false;
        }

        addresses.getMassConstSharedFragmentPtr =
            constSharedDataAddress;


        RequiredScan inventoryStaticStructRequest{};

        inventoryStaticStructRequest.name =
            "MiniMap::FCrInventoryFragment::StaticStruct";

        inventoryStaticStructRequest.idaPattern =
            "E8 ?? ?? ?? ?? "
            "4C 8B C0 48 8B D3 48 8B CF "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 74 ?? "
            "48 8B 7C 24 48 "
            "48 8D 88 20 01 00 00 "
            "48 8B 57 30 "
            "E8 ?? ?? ?? ?? "
            "85 C0 7E ?? 8B 5F 38";

        inventoryStaticStructRequest.followRel32At = 0x00;

        inventoryStaticStructRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t inventoryStaticStructAddress =
            resolveRequired(inventoryStaticStructRequest);

        if (inventoryStaticStructAddress == 0)
        {
            return false;
        }

        addresses.inventoryFragmentStaticStruct =
            inventoryStaticStructAddress;


        RequiredScan foundableParametersStructRequest{};

        foundableParametersStructRequest.name =
            "MiniMap::FCrMassFoundableParameters::StaticStruct";

        foundableParametersStructRequest.idaPattern =
            "4D 63 B5 30 03 00 00 48 0F 44 D8 48 C1 E6 05 48 03 F3 "
            "48 3B DE 74 ?? E8 ?? ?? ?? ?? 48 39 03 74 ?? "
            "48 83 C3 20 48 3B DE 75 ??";

        foundableParametersStructRequest.followRel32At = 0x17;

        foundableParametersStructRequest.kind =
            AddressKind::FunctionEntry;

        const uintptr_t foundableParametersStructAddress =
            resolveRequired(foundableParametersStructRequest);

        if (foundableParametersStructAddress == 0)
        {
            return false;
        }

        addresses.foundableParametersStaticStruct =
            foundableParametersStructAddress;

        // Alien Actors: each anchor verified unique in client CL-127004.
        {
            RequiredScan request{};
            request.name = "MiniMap::AddRequirement<FMassEnemyStateFragment>";
            request.idaPattern =
                "45 33 C0 B2 02 48 8B CB E8 ?? ?? ?? ?? 41 B0 03 "
                "B2 02 48 8B CB E8 ?? ?? ?? ?? 33 D2 48 8B CB E8 "
                "?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D0 48 8D 8F 58 03 "
                "00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D0 48 8D "
                "8F 58 03 00 00";
            request.followRel32At = 8;

            request.kind = AddressKind::FunctionEntry;
            request.resultOffset = 0;

            addresses.addEnemyStateRequirement = resolveRequired(request);
            if (addresses.addEnemyStateRequirement == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: AddRequirement<FMassEnemyStateFragment> = 0x%llX",
                static_cast<unsigned long long>(addresses.addEnemyStateRequirement));
        }

        {
            RequiredScan request{};
            request.name = "MiniMap::FMassEnemyStateFragment::StaticStruct";
            request.idaPattern =
                "0F 10 46 48 0F 11 00 F2 0F 10 4E 58 F2 0F 11 48 "
                "10 E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8 ?? ?? ?? "
                "?? 48 8B F8 E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8 "
                "?? ?? ?? ?? 48 8B D8 E8 ?? ?? ?? ?? 48 8B D0 49 "
                "8B CE";
            request.followRel32At = 17;

            request.kind = AddressKind::FunctionEntry;
            request.resultOffset = 0;

            addresses.enemyStateFragmentStaticStruct = resolveRequired(request);
            if (addresses.enemyStateFragmentStaticStruct == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassEnemyStateFragment::StaticStruct = 0x%llX",
                static_cast<unsigned long long>(addresses.enemyStateFragmentStaticStruct));
        }

        {
            RequiredScan request{};
            request.name = "MiniMap::FMassEnemyTag::StaticStruct";
            request.idaPattern =
                "48 8B 9A C8 00 00 00 49 8B F8 4C 8B F2 48 8B F1 "
                "E8 ?? ?? ?? ?? 48 8D 4B 20 8B D0 E8 ?? ?? ?? ?? "
                "E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8 ?? ?? ?? ?? "
                "E8 ?? ?? ?? ?? 48 8B D0 49 8B CE";
            request.followRel32At = 32;

            request.kind = AddressKind::FunctionEntry;
            request.resultOffset = 0;

            addresses.enemyTagStaticStruct = resolveRequired(request);
            if (addresses.enemyTagStaticStruct == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassEnemyTag::StaticStruct = 0x%llX",
                static_cast<unsigned long long>(addresses.enemyTagStaticStruct));
        }

        {
            RequiredScan request{};
            request.name = "MiniMap::FMassNeutralAITag::StaticStruct";
            request.idaPattern =
                "E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 20 E8 ?? ?? "
                "?? ?? 48 83 C4 50 5F 5E 5B C3 32 C0 48 83 C4 50";
            request.followRel32At = 0;

            request.kind = AddressKind::FunctionEntry;
            request.resultOffset = 0;

            addresses.neutralTagStaticStruct = resolveRequired(request);
            if (addresses.neutralTagStaticStruct == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassNeutralAITag::StaticStruct = 0x%llX",
                static_cast<unsigned long long>(addresses.neutralTagStaticStruct));
        }

        {
            RequiredScan request{};
            request.name = "MiniMap::FMassFragmentRequirements::AddTagRequirement";
            request.idaPattern =
                "48 8B 03 48 8B CB FF 90 10 03 00 00 45 33 C0 49 "
                "8B CF 48 8B D0 E8 ?? ?? ?? ?? 48 8B 03 48 8D 54 "
                "24 20 48 8B CB FF 90 18 03 00 00";
            request.followRel32At = 21;

            request.kind = AddressKind::FunctionEntry;
            request.resultOffset = 0;

            addresses.addTagRequirement = resolveRequired(request);
            if (addresses.addTagRequirement == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassFragmentRequirements::AddTagRequirement = 0x%llX",
                static_cast<unsigned long long>(addresses.addTagRequirement));
        }

        // Mandatory grid/collector functions. Each pattern is unique in CL-127004
        // executable and its target ABI was checked against the matching PDB.
        // These resolve functions, not hooks. Scanner failures refuse plugin admission.
        const auto resolveGridDiagnostic = [&](const char* name, const char* pattern,
            int32_t followAt, uintptr_t& target)
        {
            RequiredScan request{};
            request.name = name;
            request.idaPattern = pattern;
            request.kind = AddressKind::FunctionEntry;
            if (followAt >= 0) request.followRel32At = static_cast<uint32_t>(followAt);
            request.resultOffset = 0;

            target = resolveRequired(request);
            LOG_DEBUG("MiniMap: building native: resolve %s = 0x%llX", name,
                static_cast<unsigned long long>(target));
        };
        constexpr const char* entityBuiltPattern =
            "48 89 5C 24 18 48 89 54 24 10 57 48 83 EC 40 48 8B DA 48 8B F9 "
            "E8 ?? ?? ?? ?? 84 C0 75 4B 48 8D 0D ?? ?? ?? ?? 85 DB 48 8D 05 ?? ?? ?? ?? "
            "41 B8 45 07 00 00 48 0F 44 C1 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 30 48 8D 15 ?? ?? ?? ??";
        resolveGridDiagnostic("MiniMap::FMassEntityManager::IsEntityBuilt",
            entityBuiltPattern, -1, addresses.isMassEntityBuilt);
        resolveGridDiagnostic("MiniMap::FMassEntityManager::IsEntityValid",
            entityBuiltPattern, 21, addresses.isMassEntityValid);
        resolveGridDiagnostic("MiniMap::UWorld::GetSubsystem<UCrEntityGridSubsystem>",
            "48 89 74 24 20 57 48 81 EC 90 00 00 00 48 8B F9 E8 ?? ?? ?? ?? "
            "48 85 C0 0F 84 F8 00 00 00 48 8B C8 E8 ?? ?? ?? ?? 48 8B F0 "
            "48 85 C0 0F 84 E4 00 00 00", 0x21, addresses.getEntityGridSubsystem);
        constexpr const char* gridBoxPattern =
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 "
            "48 81 EC B0 00 00 00 41 83 78 0C 00 49 8D 78 0C 0F 29 70 E8 "
            "48 8B F2 41 8B 50 08 48 8B E9 49 8B 08 4D 8B F1 0F 29 78 D8 "
            "49 8B D8 44 0F 29 40 C8 7C 0C E8 ?? ?? ?? ?? 33 C0";
        // CL-127004: unique existing box anchor, call at +14D -> 07714720.
        // PDB: (const FVector&, float, const TFunctionRef<bool(const
        // FCrGridEntityHandle&)>&, bool). Final bool gates IsInRadius only;
        // cell selection is XY at both hierarchy levels regardless of Z.
        resolveGridDiagnostic("MiniMap::UCrEntityGridSubsystem::ForEachCellInRadius",
            gridBoxPattern, 0x14D, addresses.gridForEachCellInRadius);
#if defined(MODLOADER_CLIENT_BUILD)
        {
            RequiredScan request{};
            request.name = "MiniMap::Spawn";
            request.idaPattern = "48 89 5C 24 18 55 56 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 00 00 00 48 8B 19 48 8B EA 48 8B F1 48 85 DB 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8D 50 30 48 63 40 38 3B 43 38 0F 8F ?? ?? ?? ?? 48 8B C8 48 8B 43 30 48 39 14 C8 0F 85 ?? ?? ?? ?? 48 8B 1E 48 85 DB 0F 84 ?? ?? ?? ?? 48 85 ED 75 ?? 80 3D ?? ?? ?? ?? 03 0F 82 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 FF 48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 75 ?? 48 8B 06 EB ?? 48 8B C7 48 39 B8 E0 00 00 00 0F 84 ?? ?? ?? ?? 48 8B 1E 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 75 ?? 48 8B 1E EB ?? 48 8B DF 40 38 3D ?? ?? ?? ?? 48 8B 9B E0 00 00 00 74 ?? 48 85 DB 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 45 10 48 83 C3 30 48 63 4B 08 3B 48 38 7F ?? 48 8B 40 30 48 39 1C C8 0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 03 0F 82 ?? ?? ?? ?? 48 8B 1E 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 75 ?? 48 8B 3E 48 8B 8F E0 00 00 00 48 85 C9 75 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? EB ?? 45 33 C0 48 8D 54 24 20 E8 ?? ?? ?? ?? 83 7C 24 28 00 48 8D 3D ?? ?? ?? ?? 48 8B DF 48 8D 54 24 30 48 0F 45 5C 24 20 48 8B CD 45 33 C0 E8 ?? ?? ?? ?? 83 7C 24 38 00 48 8D 15 ?? ?? ?? ?? 4C 8B CB 48 8D 0D ?? ?? ?? ?? 48 0F 45 7C 24 30 4C 8B C7 E8 ?? ?? ?? ?? 48 8B 4C 24 30 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.spawnObject = resolveRequired(request);
            if (!addresses.spawnObject) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Flags";
            request.idaPattern = "44 8B 41 08 41 8B C0 0B C2 41 3B C0 74 ?? F0 09 51 08 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 8B 12 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 40 53 48 83 EC 30 48 8B 19 48 8B C2 48 8B C8 48 8D 54 24 20 E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 48 8B 4B 08 48 8D 54 24 40 48 89 44 24 40 48 8B 03 FF D0 48 8B 4C 24 20 0F B6 D8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 0F B6 C3 48 83 C4 30 5B C3 CC CC CC CC CC CC CC 48 89 5C 24 08 57 48 83 EC 30 0F 10 0A 48 8B 51 08 BF 3A 00 00 00 0F 29 4C 24 20 44 8B 02 41 83 F8 01 41 8D 40 01 89 02 B8 2E 00 00 00 48 8B 19 66 0F 45 F8 48 8B 43 08 48 83 C0 02 48 3B 43 10 72 ?? BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ?? 0F 28 4C 24 20 48 8B 43 08 66 0F 6F C1 66 0F 73 D8 08 66 48 0F 7E CA 66 41 0F 7E C0 48 8B CB 66 89 38 48 83 43 08 02 48 8B 5C 24 40 48 83 C4 30 5F E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 4D 8B 00 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC 48 8B 12 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 48 8B 12 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC 40 55 53 57 48 8D AC 24 50 F8 FF FF 48 81 EC B0 08 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 A0 07 00 00 48 8B F9 48 8B DA 8B 4A 18 8B C1 C1 E8 1F 84 C0 0F 85 ?? ?? ?? ?? 81 E1 FF FF FF 7F 77 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 31 06 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 83 F9 01 0F 87 ?? ?? ?? ??";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.setObjectFlags = resolveRequired(request);
            if (!addresses.setObjectFlags) return false;
        }


        {
            RequiredScan request{};
            request.name = "MiniMap::Register";
            request.idaPattern = "48 89 5C 24 18 48 89 74 24 20 57 41 54 41 57 48 81 EC A0 00 00 00 8B 79 0C 48 8D 1D ?? ?? ?? ?? 4D 8B E0 4C 8B FA 48 8B F1 85 FF 79 ?? 4C 8B CB 48 8D 15 ?? ?? ?? ?? 41 B8 0D 04 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 48 89 AC 24 C0 00 00 00 4C 89 B4 24 C8 00 00 00 3B F8 0F 8D ?? ?? ?? ?? 8B EF 44 0F B7 F7 C1 ED 10 3B F8 7D ?? 85 FF 79 ?? 4C 8D 0D ?? ?? ?? ?? 89 7C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B E8 72 ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 6C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 00 89 7C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 3C E8 48 85 FF 75 ?? 4C 8B CB 48 8D 15 ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 4B 8D 0C 76 48 8D 3C CF 48 85 FF 75 ?? 4C 8B CB 48 8D 15 ?? ?? ?? ?? 41 B8 4B 03 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 33 FF 8B 47 08 C1 E8 1C A8 01 74 ?? 45 33 C9 48 8D 54 24 50 45 33 C0 48 8B CE E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8B C3 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 3E 07 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 50 0F B6 F8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 40 84 FF 74 ?? 90 CC 8B 46 08 C1 E8 1E F6 D0 A8 01 75 ?? 80 3D ?? ?? ?? ?? 05 0F 82 ?? ?? ?? ?? 45 33 C0";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.registerComponent = resolveRequired(request);
            if (!addresses.registerComponent) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Unregister";
            request.idaPattern = "40 53 48 83 EC 50 F6 81 94 00 00 00 01 48 8B D9 75 ?? 80 3D ?? ?? ?? ?? 05 0F 82 ?? ?? ?? ?? 45 33 C0 48 8D 54 24 30 E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8B C0 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 30 48 85 C9 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C4 50 5B C3 48 83 B9 A8 00 00 00 00 75 ?? 45 33 C9 48 89 7C 24 60 45 33 C0 48 8D 54 24 40 E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 E5 07 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 40 0F B6 F8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 40 84 FF 48 8B 7C 24 60 74 ?? 90 CC 33 D2 48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 C7 83 A8 00 00 00 00 00 00 00 48 83 C4 50 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC 48 83 EC 48 33 C0 48 89 02 48 89 42 08 48 89 42 10 48 89 42 18 48 89 42 20 48 89 42 28 88 42 30 48 8B C2 4C 8B 81 90 00 00 00 4D 85 C0 74 ?? 41 0F 10 90 D0 00 00 00 C6 44 24 30 01 F2 41 0F 10 88 C8 00 00 00 41 0F 10 80 B8 00 00 00 0F 11 54 24 18 F2 41 0F 10 90 E0 00 00 00 0F 11 02 0F 10 44 24 10 F2 0F 10 C1 0F 10 4C 24 20 0F 11 42 10 F2 0F 10 44 24 30 66 0F 14 CA 0F 11 4A 20 F2 0F 11 42 30 48 83 C4 48 C3 CC CC CC CC CC CC CC CC 48 83 EC 48 33 C0 48 89 02 48 89 42 08 48 89 42 10 48 89 42 18 48 89 42 20 48 89 42 28 88 42 30 48 8B C2 4C 8B 41 30 4D 85 C0 74 ?? 41 0F 10 90 E0 00 00 00 C6 44 24 30 01 F2 41 0F 10 88 D8 00 00 00 41 0F 10 80 C8 00 00 00 0F 11 54 24 18 F2 41 0F 10 90 F0 00 00 00 0F 11 02 0F 10 44 24 10";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.unregisterComponent = resolveRequired(request);
            if (!addresses.unregisterComponent) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::DestroyComponent";
            request.idaPattern = "4C 8B DC 41 56 48 81 EC A0 00 00 00 0F B6 81 97 00 00 00 4C 8B F1 A8 40 0F 85 ?? ?? ?? ?? 0C 40 49 89 5B 10 49 89 6B F0 88 81 97 00 00 00 A8 20 74 ?? 48 8B 01 33 D2 FF 90 F0 03 00 00 41 F6 86 97 00 00 00 08 74 ?? 49 8B 06 49 8B CE FF 90 F8 03 00 00 41 80 A6 97 00 00 00 EF 33 ED 41 F6 86 94 00 00 00 01 0F 84 ?? ?? ?? ?? 49 39 AE A8 00 00 00 75 ?? 45 33 C9 48 8D 54 24 60 45 33 C0 49 8B CE E8 ?? ?? ?? ?? 39 68 08 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 E5 07 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 60 0F B6 D8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 84 DB 74 ?? 90 CC 33 D2 49 8B CE E8 ?? ?? ?? ?? 49 8B CE E8 ?? ?? ?? ?? 49 89 AE A8 00 00 00 49 8B 9E A0 00 00 00 48 89 B4 24 90 00 00 00 48 89 BC 24 88 00 00 00 48 89 5C 24 58 48 85 DB 0F 84 ?? ?? ?? ?? 41 0F B6 86 B1 00 00 00 FE C8 3C 01 76 ?? 49 8B D6 48 8B CB E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 40 38 2D ?? ?? ?? ?? 49 8B CE 48 89 4C 24 50 74 ?? E8 ?? ?? ?? ?? 48 8B 4C 24 50 48 8B 93 90 02 00 00 48 8D 44 24 50 48 3B C2 72 ?? 4C 63 83 9C 02 00 00 4C 8D 4C 24 50 4A 8D 04 C2 4C 3B C8 73 ?? 48 63 83 98 02 00 00 4C 8D 0D ?? ?? ?? ?? 48 C7 44 24 40 08 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 44 24 38 48 8D 44 24 50 4C 89 44 24 30 41 B8 AC 07 00 00 48 89 54 24 28 48 8D 15 ?? ?? ?? ?? 48 89 44 24 20 E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 4C 24 50 48 63 BB 98 02 00 00 85 FF 0F 84 ?? ?? ?? ?? 4C 89 A4 24 80 00 00 00 8B F5 40 38 35 ?? ?? ?? ?? 4C 8B A3 90 02 00 00 48 8B D9 4C 89 6C 24 78 44 8B ED 4C 89 7C 24 70 89 AC 24 C0 00 00 00";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.destroyComponent = resolveRequired(request);
            if (!addresses.destroyComponent) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::InitTarget";
            request.idaPattern = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 41 0F B6 E9 41 8B F8 8B F2 48 8B D9 85 D2 74 ?? 45 85 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 89 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 40 0F B6 CD E8 ?? ?? ?? ?? 84 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 0F B6 83 62 01 00 00 24 FE 89 B3 48 01 00 00 0A 44 24 60 89 BB 4C 01 00 00 40 88 AB 68 01 00 00 88 83 62 01 00 00 81 FE 00 00 01 00 76 ?? 48 8D 4C 24 40 48 89 5C 24 40 E8 ?? ?? ?? ?? 84 C0 75 ?? C7 83 48 01 00 00 01 00 00 00 81 BB 4C 01 00 00 00 00 01 00 76 ?? 48 8D 4C 24 20 48 89 5C 24 20 E8 ?? ?? ?? ?? 84 C0 75 ?? C7 83 4C 01 00 00 01 00 00 00 48 8B 03 48 8B CB 48 8B 5C 24 48 48 8B 6C 24 50 48 8B 74 24 58 48 83 C4 30 5F 48 FF A0 28 03 00 00 CC CC CC CC CC CC CC CC CC 4C 8B DC 55 53 56 49 8D AB 28 FF FF FF 48 81 EC C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 49 89 7B 18 48 8B F1 48 8B 89 90 00 00 00 4D 89 6B D8 45 33 ED 4D 89 73 D0 4D 89 7B C8 4C 8B FA 44 39 A9 48 01 00 00 0F 8E ?? ?? ?? ?? 44 39 A9 4C 01 00 00 0F 8E ?? ?? ?? ?? 44 39 A9 50 01 00 00 0F 8E ?? ?? ?? ?? 48 8B 01 4D 89 63 E0 FF 90 D8 03 00 00 4C 8B 8E 90 00 00 00 41 BC 01 00 00 00 F6 D8 4C 89 AD 80 00 00 00 41 8B FC 4C 89 AD 88 00 00 00 48 1B C9 41 8B 81 48 01 00 00 83 E1 10 45 8B 81 44 01 00 00 8B D1 41 0F B7 99 50 01 00 00 48 0F BA EA 11 41 F6 C0 02 89 44 24 30 41 8B 81 4C 01 00 00 48 0F 44 D1 89 44 24 34 8B 0D ?? ?? ?? ?? 4C 8B F2 49 8B 01 49 0F BA EE 18";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.initRenderTarget = resolveRequired(request);
            if (!addresses.initRenderTarget) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::TargetResource";
            request.idaPattern = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 38 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 BD 00 00 00 00 00 01 00 00 48 FF C3 41 8B F0 48 89 1D ?? ?? ?? ?? 48 8B FA 48 3B DD 75 ?? 48 8D 4C 24 40 C6 44 24 40 00 48 8B DD E8 ?? ?? ?? ?? 84 C0 B9 01 00 00 00 48 0F 44 D9 48 C7 07 00 00 00 00 81 FE FF FF FF 00 76 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 3D 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 3B DD 72 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 3E 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 6C 24 38 48 8B C7 48 C1 E3 18 48 0B DE 48 8B 74 24 48 48 89 1F 48 8B 5C 24 30 48 83 C4 20 5F C3 CC CC CC CC CC CC CC 48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA B0 17 00 00 48 8B 04 C8 8B 04 02 39 05 ?? ?? ?? ?? 7F ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 45 33 C9 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 50 70 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 33 C0 48 39 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.getRenderTargetResource = resolveRequired(request);
            if (!addresses.getRenderTargetResource) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Transition";
            request.idaPattern = "40 53 56 57 41 54 41 56 41 57 48 83 EC 78 0F 10 02 48 8B DA 48 8B F9 48 8D 54 24 40 0F 29 44 24 40 E8 ?? ?? ?? ?? 48 8B 43 08 0F 10 03 44 0F B6 67 6A 4C 63 F8 49 8B DF 0F 29 44 24 50 48 C1 E3 04 80 7F 25 00 74 ?? 83 7F 20 00 7F ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 37 48 83 C6 07 48 83 E6 F8 48 8D 04 1E 48 3B 47 08 76 ?? 8D 53 08 48 8B CF E8 ?? ?? ?? ?? 48 8B 37 48 83 C6 07 48 83 E6 F8 48 8D 04 1E 48 8B 5C 24 50 4F 8D 3C 7F 49 C1 E7 04 45 33 F6 4C 03 FB 48 89 07 49 3B DF 0F 84 ?? ?? ?? ?? 48 89 AC 24 C0 00 00 00 48 8B EE 4C 89 6C 24 70 41 BD FF FF 00 00 66 44 39 2B 75 ?? 66 44 39 6B 02 75 ?? 66 44 39 6B 04 74 ?? 48 8D 8C 24 B0 00 00 00 C6 84 24 B0 00 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 4B 18 48 8B D5 41 FF C6 48 83 C5 10 89 0A 44 88 62 04 48 89 42 08 48 83 C3 30 49 3B DF 75 ?? 4C 8B 6C 24 70 45 85 F6 0F 8E ?? ?? ?? ?? 80 7F 25 00 48 89 74 24 20 44 89 74 24 28 0F 29 74 24 60 0F 28 74 24 20 74 ?? 83 7F 20 00 7F ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 1F 48 83 C3 07 48 83 E3 F8 48 8D 43 20 48 3B 47 08 76 ?? BA 28 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B 1F 48 83 C3 07 48 83 E3 F8 48 8D 43 20 48 89 07 66 0F 6F C6 48 8B 47 30 FF 47 60 66 0F 73 D8 08 48 89 18 48 8D 43 08 48 89 47 30 66 48 0F 7E C0 85 C0 75 ?? 33 ED 33 F6 E9 ?? ?? ?? ?? 4C 63 F0 49 C1 E6 04 80 7F 25 00 74 ?? 83 7F 20 00 7F ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.transitionResources = resolveRequired(request);
            if (!addresses.transitionResources) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::RetirementConstruct";
            request.idaPattern = "48 C7 01 00 00 00 00 48 8B C1 C3 CC CC CC CC CC 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 82 42 7D 09 48 8B D9 48 89 01 48 8B 02 48 89 41 08 48 8B 42 08 48 89 41 10 48 85 C0 74 04";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.renderFenceConstruct = resolveRequired(request);
            if (!addresses.renderFenceConstruct) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::RetirementDestroy";
            request.idaPattern = "48 8B 09 48 85 C9 74 1A B8 FF FF FF FF F0 0F C1 41 08 83 F8 01 75 0B 48 8B 01 BA 01 00 00 00 48 FF 20 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 8D 05 91 07 F3 0B 48 89 01 C3 CC CC CC CC CC";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.renderFenceDestroy = resolveRequired(request);
            if (!addresses.renderFenceDestroy) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::RetirementBegin";
            request.idaPattern = "40 55 53 56 48 8D 6C 24 B9 48 81 EC 00 01 00 00 48 8B 05 39 49 6C 0A 48 33 C4 48 89 45 27 80 3D D3 6D 98 0A 00 8B DA 89 55 9F 48 8B F1 48 89 4D 87 0F 84 3F 08 00 00 E8 14 70 84 FD 84 C0 75 26";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.renderFenceBegin = resolveRequired(request);
            if (!addresses.renderFenceBegin) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::RetirementPoll";
            request.idaPattern = "40 53 48 83 EC 30 80 3D 3B 1B 97 0A 00 48 8B D9 0F 84 8A 00 00 00 E8 85 1D 83 FD 84 C0 75 30 FF 15 EB 7B 4D 0A 84 C0 75 26 4C 8D 0D B4 10 3F 07 41 B8 5C 04 00 00 48 8D 15 E3 88 AC 07 48 8D 0D";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.renderFencePoll = resolveRequired(request);
            if (!addresses.renderFencePoll) return false;
        }
#endif

#if defined(MODLOADER_CLIENT_BUILD)
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::FromValidEName";
            request.idaPattern = "48 89 5C 24 08 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B D9 48 8D 0D ?? ?? ?? ?? 8B FA 75 ?? E8 ?? ?? ?? ?? 48 8B C8 C6 05 ?? ?? ?? ?? 01 8B 8C B9 40 40 01 00 48 8B C3 89 0B 48 8B 5C 24 30 48 83 C4 20 5F C3";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackFromValidEName = resolveRequired(request);
            if (!addresses.readbackFromValidEName) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Submit";
            request.idaPattern = "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 70 4C 8B B4 24 D0 00 00 00 48 8B D9 48 89 4C 24 38";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackSubmit = resolveRequired(request);
            if (!addresses.readbackSubmit) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Immediate";
            request.idaPattern = "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 89 5C 24 08 57 48 83 EC 30 33 C0 48 63 DA 44 8B C3 48 8B F9 48 63 49 28 41 F7 D0 41 C1 E8 1F 3B D9 41 0F 4C C0 85 C0";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.readbackImmediate = resolveRequired(request);
            if (!addresses.readbackImmediate) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Create";
            request.idaPattern = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40 49 83 79 48 00 49 8B F9 48 8B 01 49 8B F0 4C 8B F2 48 C7 44 24 20 00 00 00 00 41 0F 95 C1 48 8D 54 24 30";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackCreate = resolveRequired(request);
            if (!addresses.readbackCreate) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Construct";
            request.idaPattern = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 33 C0 48 89 43 18 48 89 43 20 48 89 43 28 48 89 43 30 48 89 43 38 48 89 43 40 48 89 43 48 48 89 43 50 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 7C 24 08";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackConstruct = resolveRequired(request);
            if (!addresses.readbackConstruct) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Destroy";
            request.idaPattern = "48 89 5C 24 08 57 48 83 EC 20 48 8B 79 50 48 8B D9 48 85 FF 74 ?? 48 8D 4F 08 BA 03 00 00 00 E8 ?? ?? ?? ?? 85 C0 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 53 00 00 00 48 8D 15 ?? ?? ?? ??";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackDestroy = resolveRequired(request);
            if (!addresses.readbackDestroy) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Copy";
            request.idaPattern = "40 55 57 41 54 41 56 48 8D AC 24 D8 FD FF FF 48 81 EC 28 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F0 01 00 00 4C 8B B5 78 02 00 00 48 8B F9 48 89 4C 24 60 4C 8B E2";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackCopy = resolveRequired(request);
            if (!addresses.readbackCopy) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Lock";
            request.idaPattern = "4C 8B DC 49 89 5B 10 57 48 83 EC 50 0F BC 41 10 41 B9 20 00 00 00 49 8B D8 48 8B FA 48 8B D1 44 0F 45 C8 33 C0 4A 39 44 C9 18 4E 8D 04 C9 74 ?? 49 89 43 E8 89 44 24 78";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackLock = resolveRequired(request);
            if (!addresses.readbackLock) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Unlock";
            request.idaPattern = "40 53 48 83 EC 30 8B 41 14 48 8B D9 48 83 7C C1 18 00 75 ?? 48 8D 05 ?? ?? ?? ?? 41 B9 0C 01 00 00 4C 8D 05 ?? ?? ?? ?? 48 89 44 24 20 33 D2 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackUnlock = resolveRequired(request);
            if (!addresses.readbackUnlock) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Poll";
            request.idaPattern = "48 8B C1 BA 01 00 00 00 8B 0D ?? ?? ?? ?? D3 E2 48 8B C8 FF CA E9 ?? ?? ?? ?? CC CC CC CC CC CC 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FB FF FF 48 81 EC 78 05 00 00";
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            addresses.readbackPoll = resolveRequired(request);
            if (!addresses.readbackPoll) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::MarkForDelete";
            request.idaPattern = "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F 0D 49 08 8B 41 08 0F 1F 40 00 0F 1F 84 00 00 00 00 00 8B D0 0F BA EA 1E F0 0F B1 51 08 75 ?? 8B F8 85 C0 79 ?? 4C 8D 0D ?? ?? ?? ??";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackMarkForDelete = resolveRequired(request);
            if (!addresses.readbackMarkForDelete) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Malloc";
            request.idaPattern = "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 44 8B C3 48 8B D7 48 8B 5C 24 30 48 83 C4 20 5F 48 FF 60 28";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackMalloc = resolveRequired(request);
            if (!addresses.readbackMalloc) return false;
        }
        {
            RequiredScan request{};
            request.name = "MiniMap::Readback::Free";
            request.idaPattern = "48 85 C9 74 ?? 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 48 48 83 C4 20 5B C3 CC CC CC CC";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            addresses.readbackFree = resolveRequired(request);
            if (!addresses.readbackFree) return false;
        }
        // The readback pattern extends the existing FMemory::Free fingerprint.
        // Keep both mandatory contracts, but publish one NativeApi callable.
        if (addresses.readbackFree != addresses.memoryFree)
        {
            scanner.Fail("MiniMap::Readback::Free", "FMemory::Free fingerprints resolved to different entries");
            return false;
        }

        const uintptr_t readbackImage = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER readbackDos{};
        IMAGE_NT_HEADERS64 readbackNt{};
        auto imageFailure = [&]()
        {
            scanner.Fail("MiniMap::Readback::Globals", "invalid or unreadable executable headers/section table");
            return false;
        };
        if (!readbackImage || !MiniMapNative::ReadMemory(readbackImage, &readbackDos, sizeof(readbackDos)) ||
            readbackDos.e_magic != IMAGE_DOS_SIGNATURE || readbackDos.e_lfanew <= 0 ||
            uintptr_t(readbackDos.e_lfanew) > UINTPTR_MAX - readbackImage) return imageFailure();
        const uintptr_t readbackNtAddress = readbackImage + uintptr_t(readbackDos.e_lfanew);
        if (!MiniMapNative::ReadMemory(readbackNtAddress, &readbackNt, sizeof(readbackNt)) ||
            readbackNt.Signature != IMAGE_NT_SIGNATURE ||
            readbackNt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !readbackNt.OptionalHeader.SizeOfImage ||
            readbackNt.OptionalHeader.SizeOfImage > UINTPTR_MAX - readbackImage) return imageFailure();
        const uintptr_t readbackImageEnd = readbackImage + readbackNt.OptionalHeader.SizeOfImage;
        const size_t readbackSectionOffset = size_t(readbackDos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + readbackNt.FileHeader.SizeOfOptionalHeader;
        const size_t readbackSectionBytes = size_t(readbackNt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (readbackSectionOffset > readbackNt.OptionalHeader.SizeOfImage ||
            readbackSectionBytes > readbackNt.OptionalHeader.SizeOfImage - readbackSectionOffset ||
            !MiniMapNative::IsMemoryRangeReadable(readbackImage + readbackSectionOffset, readbackSectionBytes)) return imageFailure();

        // Resolve the CODE anchor centrally. FOLLOW_REL32 only understands E8/E9,
        // so preserve the actual MOV/LEA/CMP RIP operand decode here.
        auto decodeGlobal = [&](const char* name, uintptr_t anchor, size_t displacementOffset,
            size_t instructionSize, std::array<uint8_t,3> opcode, size_t storageSize,
            size_t alignment, uintptr_t& output)
        {
            auto invalid = [&]()
            {
                scanner.Fail(name, "invalid RIP instruction or unreadable/non-data global target");
                return false;
            };
            std::array<uint8_t,7> instruction{};
            if (anchor < readbackImage || anchor > readbackImageEnd || instructionSize > readbackImageEnd-anchor ||
                !MiniMapNative::ReadMemory(anchor, instruction.data(), instructionSize) ||
                std::memcmp(instruction.data(), opcode.data(), displacementOffset) != 0 ||
                (instructionSize == 7 && displacementOffset == 2 && instruction[6] != 0)) return invalid();
            int32_t displacement{};
            std::memcpy(&displacement, instruction.data()+displacementOffset, sizeof(displacement));
            const uintptr_t next = anchor + instructionSize;
            const uint64_t magnitude = displacement < 0 ? uint64_t(-int64_t(displacement)) : uint64_t(displacement);
            if ((displacement < 0 && next < magnitude) ||
                (displacement >= 0 && magnitude > UINTPTR_MAX-next)) return invalid();
            const uintptr_t target = displacement < 0 ? next-uintptr_t(magnitude) : next+uintptr_t(magnitude);
            if (target < readbackImage || target > readbackImageEnd || storageSize > readbackImageEnd-target ||
                !MiniMapNative::IsMemoryRangeReadable(target,storageSize,alignment)) return invalid();
            bool valid = false;
            for (unsigned i=0; i<readbackNt.FileHeader.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER section{};
                if (!MiniMapNative::ReadMemory(readbackImage+readbackSectionOffset+size_t(i)*sizeof(section), &section, sizeof(section))) return invalid();
                if (section.VirtualAddress > readbackNt.OptionalHeader.SizeOfImage ||
                    section.Misc.VirtualSize > readbackNt.OptionalHeader.SizeOfImage-section.VirtualAddress) continue;
                const uintptr_t begin = readbackImage+section.VirtualAddress;
                const uintptr_t end = begin+section.Misc.VirtualSize;
                if (target >= begin && target <= end && storageSize <= end-target &&
                    (section.Characteristics & IMAGE_SCN_MEM_READ) && !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) valid = true;
            }
            if (!valid) return invalid();
            output = target; // Storage address, not its current pointee/value.
            return true;
        };
        auto resolveReadbackGlobal = [&](const char* name, const char* pattern, size_t displacementOffset,
            size_t instructionSize, std::array<uint8_t,3> opcode, uintptr_t& output)
        {
            RequiredScan request{};
            request.name = name;
            request.idaPattern = pattern;
            request.kind = AddressKind::Code;

            request.resultOffset = 0;
            const uintptr_t anchor = resolveRequired(request);
            return anchor && decodeGlobal(name, anchor, displacementOffset, instructionSize, opcode, 8, 1, output);
        };
        if (!resolveReadbackGlobal("MiniMap::Readback::DynamicRHI",
            "48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 20 C7 44 24 20 01 03 01 07 48 8D 54 24 40 C7 44 24 24 00 00 02 00 C7 44 24 28 07 00 00 00 66 C7 44 24 2C FF F4 48 8B 01 FF 50 58 48 8B 1D ?? ?? ?? ??", 3, 7, {0x48,0x8b,0x0d}, addresses.readbackDynamicRHI)) return false;
        if (!resolveReadbackGlobal("MiniMap::Readback::Pipe",
            "48 8D 0D ?? ?? ?? ?? 48 89 45 E7 48 8D 45 E7 48 89 44 24 20 E8 ?? ?? ?? ?? 48 39 7D E7 0F 84 ?? ?? ?? ?? 48 8B 45 0F 48 8D 4D F7 48 85 C0 48 0F 45 C8 48 8B 01 FF 50 10", 3, 7, {0x48,0x8d,0x0d}, addresses.readbackPipe)) return false;
        if (!resolveReadbackGlobal("MiniMap::Readback::Threaded",
            "80 3D ?? ?? ?? ?? 00 74 ?? 8B 05 ?? ?? ?? ?? 83 F8 01 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 E5 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 8B B0 00 00 00 E8 ?? ?? ?? ?? 48 83 BB C0 00 00 00 00", 2, 7, {0x80,0x3d,0}, addresses.readbackThreaded)) return false;
        if (!resolveReadbackGlobal("MiniMap::Readback::Multithreaded",
            "80 3D ?? ?? ?? ?? 00 41 8B F9 49 8B E8 48 8B F2 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 4C 8B 88 38 02 00 00 83 FF FF 75 ?? 0F BC 82 BC 01 00 00 BF 20 00 00 00 0F 45 F8 44 8B C7", 2, 7, {0x80,0x3d,0}, addresses.readbackMultithreaded)) return false;
        if (!resolveReadbackGlobal("MiniMap::Readback::GpuCount",
            "8B 0D ?? ?? ?? ?? D3 E2 48 8B C8 FF CA E9 ?? ?? ?? ?? CC CC CC CC CC CC 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FB FF FF 48 81 EC 78 05 00 00 48 8B 05 ?? ?? ?? ??", 2, 6, {0x8b,0x0d,0}, addresses.readbackGpuCount)) return false;
#endif

#if defined(MODLOADER_CLIENT_BUILD)
        {
            RequiredScan request{};
            request.name = "MiniMap::GUObjectArray";
            request.idaPattern = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 1B 48 8B D3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 08 B0 01 48 83 C4 20 5B C3 32 C0";
            request.kind = AddressKind::FunctionEntry;

            request.resultOffset = 0;
            const uintptr_t anchor = resolveRequired(request);
            if (!anchor) return false;
            auto invalid = [&]()
            {
                scanner.Fail(request.name, "invalid UObject-array LEA or unreadable array storage");
                return false;
            };
            std::array<uint8_t,7> instruction{};
            if (anchor > UINTPTR_MAX-0x1c ||
                !MiniMapNative::ReadMemory(anchor+0x15, instruction.data(), instruction.size()) ||
                instruction[0]!=0x48 || instruction[1]!=0x8d || instruction[2]!=0x0d) return invalid();
            int32_t displacement{};
            std::memcpy(&displacement, instruction.data()+3, sizeof(displacement));
            const uintptr_t next = anchor+0x1c;
            const uint64_t magnitude = displacement<0 ? uint64_t(-int64_t(displacement)) : uint64_t(displacement);
            if ((displacement<0 && next<magnitude) || (displacement>=0 && magnitude>UINTPTR_MAX-next)) return invalid();
            const uintptr_t target = displacement<0 ? next-uintptr_t(magnitude) : next+uintptr_t(magnitude);
            if (!MiniMapNative::IsMemoryRangeReadable(target,0x30,8)) return invalid();
            addresses.objectArray = target; // Enclosing UObject array; no extra indirection.
        }
#if defined(MODLOADER_CLIENT_BUILD)
        // Private-world native entry points. Leaf functions use CODE; unwind-covered
        // callable entries use FUNCTION_START. All requests are mandatory.
        auto resolvePrivateWorld = [&](const char* name, const char* pattern,
            AddressKind kind, uintptr_t& output)
        {
            RequiredScan request{};
            request.name = name;
            request.idaPattern = pattern;
            request.kind = kind;

            request.resultOffset = 0;
            output = resolveRequired(request);
            return output != 0;
        };
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::createWorld",
            "48 89 5C 24 10 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00 44 8B A4 24 08 01 00 00 33 FF 41 83 FC 05 49 8B E9 44 0F B6 EA 44 8B F9",
            AddressKind::FunctionEntry, addresses.createWorld)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::initWorld",
            "40 55 57 41 54 48 8D AC 24 40 FE FF FF 48 81 EC C0 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 80 01 00 00 0F B6 81 8B 01 00 00 4C 8B E2 48 8B F9 A8 40",
            AddressKind::FunctionEntry, addresses.initWorld)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::updateWorldComponents",
            "4C 8B DC 55 56 57 41 55 41 57 48 81 EC 80 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 60 01 00 00 4D 89 63 D0 45 0F B6 E0 4D 89 73 C8 4D 8B F9 44 0F B6 EA 48 8B E9",
            AddressKind::FunctionEntry, addresses.updateWorldComponents)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::cleanupWorld",
            "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 FF 05 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? F6 81 8B 01 00 00 40 49 8B F1 41 0F B6 E8 44 0F B6 F2 48 8B D9",
            AddressKind::FunctionEntry, addresses.cleanupWorld)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::getWorldContextFromWorld",
            "48 8B 81 C8 11 00 00 48 63 89 D0 11 00 00 48 8D 0C C8 48 3B C1 74 ?? 66 0F 1F 84 00 00 00 00 00 4C 8B 00 49 39 90 C0 02 00 00 74 ?? 48 83 C0 08 48 3B C1 75 ?? 33 C0 C3 49 8B C0 C3",
            AddressKind::Code, addresses.getWorldContextFromWorld)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::getSubsystemInternal",
            "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B FA 48 8B F1 48 85 D2 75 ?? 48 8D 05 ?? ?? ?? ?? 44 8D 4A 35 48 89 44 24 20 4C 8D 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 33 C0",
            AddressKind::FunctionEntry, addresses.getSubsystemInternal)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::isGarbageCollectingAndLocking",
            "0F B6 05 ?? ?? ?? ?? 90 C3 CC CC CC CC CC CC CC 40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 B2 00 00 00",
            AddressKind::Code, addresses.isGarbageCollectingAndLocking)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::isIncrementalPurgePending",
            "0F B6 05 ?? ?? ?? ?? 90 84 C0 75 ?? 0F B6 05 ?? ?? ?? ?? 90 84 C0 75 ?? C3 B0 01 C3 CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC",
            AddressKind::Code, addresses.isIncrementalPurgePending)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::isIncrementalReachabilityPending",
            "0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 8B 05 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 7C ?? 80 3D ?? ?? ?? ?? 00 75 ?? 32 C0 C3 B0 01 C3",
            AddressKind::Code, addresses.isIncrementalReachabilityPending)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::getTickableTickType",
            "40 53 48 83 EC 20 48 8B D9 BA 30 00 00 00 48 83 C1 D0 E8 ?? ?? ?? ?? 84 C0 75 ?? 38 43 08 74 ?? 48 83 C4 20 5B C3 B0 02 48 83 C4 20 5B C3",
            AddressKind::FunctionEntry, addresses.getTickableTickType)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::isVisibilityRequestPending",
            "40 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 89 5C 24 30 48 8D 9F 18 01 00 00 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 44 8D 43 1C",
            AddressKind::FunctionEntry, addresses.isVisibilityRequestPending)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::destroyActor",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 F6 41 5D 02 41 0F B6 F0 0F B6 EA 48 8B F9 75 ?? 8B 41 08 C1 E8 1E F6 D0 A8 01 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 44 0F B6 CE 44 0F B6 C5 48 8B D7 48 8B C8 E8 ?? ?? ?? ??",
            AddressKind::FunctionEntry, addresses.destroyActor)) return false;

        uintptr_t activeGCAnchor = 0, engineAnchor = 0;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::ActiveGarbageCollection",
            "0F B6 35 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 FF 15 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 0F 57 C0 39 05 ?? ?? ?? ?? F2 48 0F 2A 44 24 50",
            AddressKind::Code, activeGCAnchor) ||
            !decodeGlobal("MiniMap::PrivateWorld::ActiveGarbageCollection", activeGCAnchor,
                3, 7, {0x0f,0xb6,0x35}, 1, 1, addresses.activeGarbageCollection)) return false;
        if (!resolvePrivateWorld("MiniMap::PrivateWorld::EngineStorage",
            "48 8B 3D ?? ?? ?? ?? 49 8B F1 49 8B E8 4C 8B F2 48 8B D9 48 85 FF 74 ?? 48 83 BF ?? ?? ?? ?? 00 75 ??",
            AddressKind::Code, engineAnchor) ||
            !decodeGlobal("MiniMap::PrivateWorld::EngineStorage", engineAnchor,
                3, 7, {0x48,0x8b,0x3d}, 8, 8, addresses.engineStorage)) return false;
        // Reuse the already resolved guarded Release wrapper, without another scan.
        if (!decodeGlobal("MiniMap::Release", addresses.releaseObject,
                2, 7, {0x80,0x3d,0}, 1, 1, addresses.exitPurge)) return false;

        // Preserve the legacy direct ReleaseRef verification, deriving its address
        // from the wrapper's JE rather than a historical RVA. It is not a call binding.
        auto invalidRelease = [&]()
        {
            scanner.Fail("MiniMap::Release", "invalid guarded Release branch or direct ReleaseRef entry");
            return false;
        };
        std::array<uint8_t,13> releaseInstruction{};
        if (addresses.releaseObject < readbackImage || addresses.releaseObject > readbackImageEnd ||
            releaseInstruction.size() > readbackImageEnd-addresses.releaseObject ||
            !MiniMapNative::ReadMemory(addresses.releaseObject, releaseInstruction.data(), releaseInstruction.size()) ||
            releaseInstruction[7]!=0x0f || releaseInstruction[8]!=0x84) return invalidRelease();
        int32_t releaseDisplacement{};
        std::memcpy(&releaseDisplacement, releaseInstruction.data()+9, sizeof(releaseDisplacement));
        const uintptr_t releaseNext = addresses.releaseObject+releaseInstruction.size();
        const uint64_t releaseMagnitude = releaseDisplacement<0 ? uint64_t(-int64_t(releaseDisplacement)) : uint64_t(releaseDisplacement);
        if ((releaseDisplacement<0 && releaseNext<releaseMagnitude) ||
            (releaseDisplacement>=0 && releaseMagnitude>UINTPTR_MAX-releaseNext)) return invalidRelease();
        const uintptr_t releaseRef = releaseDisplacement<0 ? releaseNext-uintptr_t(releaseMagnitude) : releaseNext+uintptr_t(releaseMagnitude);
        constexpr std::array<uint8_t,16> releasePrefix = {0x40,0x53,0x48,0x83,0xec,0x30,0x8b,0x59,0x0c,0x85,0xdb,0x79,0x26,0x4c,0x8d,0x0d};
        std::array<uint8_t,16> releaseBytes{};
        MEMORY_BASIC_INFORMATION releasePage{};
        if (releaseRef<readbackImage || releaseRef>readbackImageEnd || releaseBytes.size()>readbackImageEnd-releaseRef ||
            !MiniMapNative::ReadMemory(releaseRef, releaseBytes.data(), releaseBytes.size()) || releaseBytes!=releasePrefix ||
            !VirtualQuery(reinterpret_cast<void*>(releaseRef), &releasePage, sizeof(releasePage)) || releasePage.State!=MEM_COMMIT ||
            (releasePage.Protect & (PAGE_GUARD|PAGE_NOACCESS)) ||
            !(releasePage.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return invalidRelease();
#endif
#endif

        return !scanFailed;
    }
}
