#include "Fingerprints.h"

#include "../plugin_helpers.h"

// ---------------------------------------------------------------------------
// Native pattern resolution.
//
// This is the only place where MiniMap uses the AlienX pattern scanner.
// Resolved native addresses remain valid after this callback; the scanner
// itself must not be retained.
//
// Fourteen native functions are currently resolved:
//
//   1.  FSoftObjectPtr::LoadSynchronous
//   2.  UWidgetBlueprintLibrary::GetBrushResourceAsTexture2D
//   3.  UStreamableRenderAsset::SetForceMipLevelsToBeResident
//   4.  UStreamableRenderAsset::WaitForStreaming
//   5.  UTexture2D::GetNumResidentMips
//   6.  UTexture2D::GetNumMipsAllowed
//   7.  UTexture2D::GetNumMips
//   8.  UTexture2D::StreamIn
//   9.  UStreamableRenderAsset::WaitForPendingInitOrStreaming
//   10. UWorld::GetFirstPlayerController
//   11. AController::GetPawn<ACrCharacterPlayerBase>
//   12. USceneComponent::K2_GetComponentLocation
//   13. ACrCharacterPlayerBase::IsPlayerInForgottenEngine
//   14. AController::GetControlRotation
//
// Each fingerprint is intentionally tied to native behavior MiniMap depends on.
// If StarRupture changes an incompatible implementation or layout, resolution
// should fail rather than allowing MiniMap to continue with stale assumptions.
// ---------------------------------------------------------------------------

namespace MiniMapFingerprints
{
    bool Resolve(
        IPluginSelf* self,
        IPluginHookScanner* scanner,
        ResolvedAddresses& addresses)
    {
        addresses = {};

        if (self == nullptr || scanner == nullptr)
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

        PluginScanRequest loadSynchronousRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        loadSynchronousRequest.hookName =
            "MiniMap::FSoftObjectPtr::LoadSynchronous";

        loadSynchronousRequest.pattern =
            "48 8B 8B 10 01 00 00 "
            "48 85 C9 "
            "0F 84 ?? ?? ?? ?? "
            "48 81 C1 C8 03 00 00 "
            "48 89 7C 24 48 "
            "E8 ?? ?? ?? ?? "
            "48 8B F8 "
            "48 85 C0";

        loadSynchronousRequest.followRel32At = 0x1C;

        loadSynchronousRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;

        loadSynchronousRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t loadSynchronousAddress =
            scanner->Resolve(
                self,
                &loadSynchronousRequest);

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

        PluginScanRequest brushTextureRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        brushTextureRequest.hookName =
            "MiniMap::UWidgetBlueprintLibrary::"
            "GetBrushResourceAsTexture2D";

        brushTextureRequest.pattern =
            "40 53 "
            "48 83 EC ?? "
            "48 8B 59 38 "
            "48 85 DB "
            "74 ?? "
            "E8 ?? ?? ?? ?? "
            "48 8B 53 10 "
            "4C 8D 40 30 "
            "48 63 40 38";

        brushTextureRequest.flags = 0;

        brushTextureRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t brushTextureAddress =
            scanner->Resolve(
                self,
                &brushTextureRequest);

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

        PluginScanRequest forceMipsRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        forceMipsRequest.hookName =
            "MiniMap::UStreamableRenderAsset::"
            "SetForceMipLevelsToBeResident";

        forceMipsRequest.pattern =
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

        forceMipsRequest.flags = 0;

        forceMipsRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t forceMipsAddress =
            scanner->Resolve(
                self,
                &forceMipsRequest);

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

        PluginScanRequest waitForStreamingRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        waitForStreamingRequest.hookName =
            "MiniMap::UStreamableRenderAsset::WaitForStreaming";

        waitForStreamingRequest.pattern =
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

        waitForStreamingRequest.flags = 0;

        waitForStreamingRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t waitForStreamingAddress =
            scanner->Resolve(
                self,
                &waitForStreamingRequest);

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

        PluginScanRequest residentMipsRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        residentMipsRequest.hookName =
            "MiniMap::UTexture2D::GetNumResidentMips";

        residentMipsRequest.pattern =
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

        residentMipsRequest.flags = 0;

        residentMipsRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t residentMipsAddress =
            scanner->Resolve(
                self,
                &residentMipsRequest);

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

        PluginScanRequest allowedMipsRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        allowedMipsRequest.hookName =
            "MiniMap::UTexture2D::GetNumMipsAllowed";

        allowedMipsRequest.pattern =
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

        allowedMipsRequest.flags = 0;

        allowedMipsRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t allowedMipsAddress =
            scanner->Resolve(
                self,
                &allowedMipsRequest);

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

        PluginScanRequest numMipsRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        numMipsRequest.hookName =
            "MiniMap::UTexture2D::GetNumMips";

        numMipsRequest.pattern =
            "40 53 "
            "48 83 EC ?? "
            "48 83 B9 50 01 00 00 00 "
            "48 8B D9 "
            "74 ?? "
            "48 8B 01 "
            "FF 90 48 03 00 00 "
            "48 8B 9B 50 01 00 00";

        numMipsRequest.flags = 0;

        numMipsRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t numMipsAddress =
            scanner->Resolve(
                self,
                &numMipsRequest);

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

        PluginScanRequest streamInRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        streamInRequest.hookName =
            "MiniMap::UTexture2D::StreamIn";

        streamInRequest.pattern =
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

        streamInRequest.flags = 0;

        streamInRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t streamInAddress =
            scanner->Resolve(
                self,
                &streamInRequest);

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

        PluginScanRequest waitPendingRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        waitPendingRequest.hookName =
            "MiniMap::UStreamableRenderAsset::"
            "WaitForPendingInitOrStreaming";

        waitPendingRequest.pattern =
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

        waitPendingRequest.flags = 0;

        waitPendingRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t waitPendingAddress =
            scanner->Resolve(
                self,
                &waitPendingRequest);

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

        PluginScanRequest firstControllerRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        firstControllerRequest.hookName =
            "MiniMap::UWorld::GetFirstPlayerController";

        firstControllerRequest.pattern =
            "40 53 "
            "48 83 EC ?? "
            "83 B9 78 02 00 00 00 "
            "48 8B D9 "
            "7E ?? "
            "48 63 81 78 02 00 00 "
            "85 C0 "
            "7F ??";

        firstControllerRequest.flags = 0;

        firstControllerRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t firstControllerAddress =
            scanner->Resolve(
                self,
                &firstControllerRequest);

        if (firstControllerAddress == 0)
        {
            return false;
        }

        addresses.getFirstPlayerController =
            firstControllerAddress;

        // -------------------------------------------------------------------
        // AController::GetPawn<ACrCharacterPlayerBase>
        //
        // Verified current-build RVA:
        //
        //   0x097D9CA0
        //
        // The native helper validates the object stored in controller pawn
        // storage as an ACrCharacterPlayerBase before returning it.
        // -------------------------------------------------------------------

        PluginScanRequest playerPawnRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        playerPawnRequest.hookName =
            "MiniMap::AController::"
            "GetPawn<ACrCharacterPlayerBase>";

        playerPawnRequest.pattern =
            "40 53 "
            "48 83 EC ?? "
            "48 8D 99 F8 02 00 00 "
            "48 83 3B 00 "
            "74 ?? "
            "E8 ?? ?? ?? ?? "
            "48 8B D0 "
            "48 8B CB "
            "E8 ?? ?? ?? ?? "
            "84 C0 "
            "74 ?? "
            "48 8B 03";

        playerPawnRequest.flags = 0;

        playerPawnRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t playerPawnAddress =
            scanner->Resolve(
                self,
                &playerPawnRequest);

        if (playerPawnAddress == 0)
        {
            return false;
        }

        addresses.getPlayerPawn =
            playerPawnAddress;

        // -------------------------------------------------------------------
        // USceneComponent::K2_GetComponentLocation
        //
        // Verified current-build RVA:
        //
        //   0x049001F0
        //
        // This is an ordinary native leaf function despite its K2 name.
        //
        // It has no RUNTIME_FUNCTION entry, so PLUGIN_SCAN_FUNCTION_START
        // would reject it. The unique signature is therefore validated as
        // executable code with PLUGIN_SCAN_CODE.
        // -------------------------------------------------------------------

        PluginScanRequest componentLocationRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        componentLocationRequest.hookName =
            "MiniMap::USceneComponent::K2_GetComponentLocation";

        componentLocationRequest.pattern =
            "0F 10 81 10 02 00 00 "
            "48 8B C2 "
            "0F 10 89 20 02 00 00 "
            "0F 11 02 "
            "F2 0F 11 4A 10 "
            "C3";

        componentLocationRequest.flags = 0;

        componentLocationRequest.kind =
            PLUGIN_SCAN_CODE;

        const uintptr_t componentLocationAddress =
            scanner->Resolve(
                self,
                &componentLocationRequest);

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

        PluginScanRequest getPlatformDataRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        getPlatformDataRequest.hookName =
            "MiniMap::UTexture2D::GetPlatformData";

        getPlatformDataRequest.pattern =
            "8B FE "
            "48 85 C9 "
            "74 ?? "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 "
            "74 ?? "
            "48 8D 54 24 ?? "
            "48 8B CB";

        getPlatformDataRequest.followRel32At = 7;

        getPlatformDataRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;

        getPlatformDataRequest.kind =
            PLUGIN_SCAN_CODE;

        const uintptr_t getPlatformDataAddress =
            scanner->Resolve(
                self,
                &getPlatformDataRequest);

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

        PluginScanRequest getBulkDataSizeRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        getBulkDataSizeRequest.hookName =
            "MiniMap::FBulkData::GetBulkDataSize";

        getBulkDataSizeRequest.pattern =
            "44 0F B6 49 0C "
            "4C 8B C1 "
            "0F B6 41 0B "
            "0F B6 51 0A "
            "0F B6 49 09 "
            "49 C1 E1 08 "
            "49 0B C1 "
            "48 C1 E0 08 "
            "48 0B C2";

        getBulkDataSizeRequest.flags = 0;

        getBulkDataSizeRequest.kind =
            PLUGIN_SCAN_CODE;

        const uintptr_t getBulkDataSizeAddress =
            scanner->Resolve(
                self,
                &getBulkDataSizeRequest);

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

        PluginScanRequest canLoadFromDiskRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        canLoadFromDiskRequest.hookName =
            "MiniMap::FBulkData::CanLoadFromDisk";

        canLoadFromDiskRequest.pattern =
            "48 8B 51 18 "
            "48 2B 15 ?? ?? ?? ?? "
            "75 ?? "
            "8B 51 20 "
            "8B 05 ?? ?? ?? ?? "
            "48 2B D0 "
            "48 85 D2 "
            "0F 95 C0 "
            "C3";

        canLoadFromDiskRequest.flags = 0;

        canLoadFromDiskRequest.kind =
            PLUGIN_SCAN_CODE;

        const uintptr_t canLoadFromDiskAddress =
            scanner->Resolve(
                self,
                &canLoadFromDiskRequest);

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

        PluginScanRequest getBulkDataCopyRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        getBulkDataCopyRequest.hookName =
            "MiniMap::FBulkData::GetCopy";

        getBulkDataCopyRequest.pattern =
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

        getBulkDataCopyRequest.flags = 0;

        getBulkDataCopyRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t getBulkDataCopyAddress =
            scanner->Resolve(
                self,
                &getBulkDataCopyRequest);

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

        PluginScanRequest memoryFreeRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        memoryFreeRequest.hookName =
            "MiniMap::FMemory::Free";

        memoryFreeRequest.pattern =
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

        memoryFreeRequest.flags = 0;

        memoryFreeRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t memoryFreeAddress =
            scanner->Resolve(
                self,
                &memoryFreeRequest);

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

        PluginScanRequest forgottenEngineRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        forgottenEngineRequest.hookName =
            "MiniMap::ACrCharacterPlayerBase::"
            "IsPlayerInForgottenEngine";

        forgottenEngineRequest.pattern =
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

        forgottenEngineRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;

        forgottenEngineRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t forgottenEngineAddress =
            scanner->Resolve(
                self,
                &forgottenEngineRequest);

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

        PluginScanRequest controlRotationRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        controlRotationRequest.hookName =
            "MiniMap::AController::GetControlRotation";

        controlRotationRequest.pattern =
            "0F 10 81 38 03 00 00 "
            "48 8B C2 "
            "F2 0F 10 89 48 03 00 00 "
            "0F 11 02 "
            "F2 0F 11 4A 10 "
            "C3";

        controlRotationRequest.flags = 0;
        controlRotationRequest.kind =
            PLUGIN_SCAN_CODE;

        const uintptr_t controlRotationAddress =
            scanner->Resolve(
                self,
                &controlRotationRequest);

        if (controlRotationAddress == 0)
        {
            return false;
        }

        addresses.getControlRotation =
            controlRotationAddress;

        PluginScanRequest getAllActorsRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        getAllActorsRequest.hookName =
            "MiniMap::UGameplayStatics::GetAllActorsOfClass";

        getAllActorsRequest.pattern =
            "4C 8D 44 24 30 4C 89 A4 24 28 01 00 00 "
            "48 8D 54 24 58 4C 89 AC 24 20 01 00 00 "
            "49 8B CE 4C 89 BC 24 18 01 00 00 "
            "E8 ?? ?? ?? ?? "
            "48 63 44 24 38 85 C0 0F 84 ?? ?? ?? ??";

        getAllActorsRequest.followRel32At = 0x25;
        getAllActorsRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        getAllActorsRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t getAllActorsAddress =
            scanner->Resolve(self, &getAllActorsRequest);

        if (getAllActorsAddress == 0)
        {
            return false;
        }

        addresses.getAllActorsOfClass =
            getAllActorsAddress;


        PluginScanRequest poiStaticClassRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        poiStaticClassRequest.hookName =
            "MiniMap::ACrPointOfInterestMarkerActor::StaticClass";

        poiStaticClassRequest.pattern =
            "48 C7 44 24 30 00 00 00 00 "
            "48 C7 44 24 38 00 00 00 00 "
            "E8 ?? ?? ?? ?? "
            "80 3D ?? ?? ?? ?? 00 "
            "48 89 44 24 58 74 ?? 48 85 C0 74 ?? "
            "48 8B C8 E8 ?? ?? ?? ?? "
            "48 89 B4 24 38 01 00 00 "
            "4C 8D 44 24 30";

        poiStaticClassRequest.followRel32At = 0x12;
        poiStaticClassRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        poiStaticClassRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t poiStaticClassAddress =
            scanner->Resolve(self, &poiStaticClassRequest);

        if (poiStaticClassAddress == 0)
        {
            return false;
        }

        addresses.pointOfInterestStaticClass =
            poiStaticClassAddress;


        PluginScanRequest poiCategoryRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        poiCategoryRequest.hookName =
            "MiniMap::UCrMapMenuDevSettings::FindPOIMarkerCategoryData";

        poiCategoryRequest.pattern =
            "48 8B 8B 10 01 00 00 48 85 C9 74 ?? "
            "0F B6 96 E9 02 00 00 "
            "E8 ?? ?? ?? ?? "
            "4C 8B E8 EB ?? 45 33 ED "
            "49 83 BE C0 03 00 00 00";

        poiCategoryRequest.followRel32At = 0x13;
        poiCategoryRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        poiCategoryRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t poiCategoryAddress =
            scanner->Resolve(self, &poiCategoryRequest);

        if (poiCategoryAddress == 0)
        {
            return false;
        }

        addresses.findPOIMarkerCategoryData =
            poiCategoryAddress;


        PluginScanRequest markerFilterRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        markerFilterRequest.hookName =
            "MiniMap::UCrPlayerMapMenuDataComponent::"
            "GetMapMenuMarkerFiltersOnOffStatus";

        markerFilterRequest.pattern =
            "48 89 5C 24 08 57 48 83 EC ?? "
            "0F B6 FA 48 8B D9 "
            "3B B9 18 01 00 00 7D ?? "
            "48 63 89 18 01 00 00 3B F9 7C ??";

        markerFilterRequest.flags = 0;
        markerFilterRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t markerFilterAddress =
            scanner->Resolve(self, &markerFilterRequest);

        if (markerFilterAddress == 0)
        {
            return false;
        }

        addresses.getMapMenuMarkerFilterStatus =
            markerFilterAddress;
        return true;
    }
}