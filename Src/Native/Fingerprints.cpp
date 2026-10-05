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

        PluginScanRequest abandonBaseCompletedRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        abandonBaseCompletedRequest.hookName =
            "MiniMap::FCrAbandonBaseData::IsCompleted";

        abandonBaseCompletedRequest.pattern =
            "48 8B 44 24 70 48 8D 55 B0 "
            "48 8B 88 A8 03 00 00 "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 0F 84 ?? ?? ?? ?? "
            "48 8B C8 E8 ?? ?? ?? ?? "
            "84 C0 74 ?? 4C 63 43 08";

        abandonBaseCompletedRequest.followRel32At =
            0x21;

        abandonBaseCompletedRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;

        abandonBaseCompletedRequest.kind =
            PLUGIN_SCAN_CODE;

        const uintptr_t abandonBaseCompletedAddress =
            scanner->Resolve(
                self,
                &abandonBaseCompletedRequest);

        if (abandonBaseCompletedAddress == 0)
        {
            return false;
        }

        addresses.isAbandonBaseCompleted =
            abandonBaseCompletedAddress;
        // -------------------------------------------------------------------
        // Foundable category and callback-free Mass query support.
        // -------------------------------------------------------------------

        PluginScanRequest foundableCategoryRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        foundableCategoryRequest.hookName =
            "MiniMap::UCrMapMenuDevSettings::"
            "FindFoundableMarkerCateroryData";

        foundableCategoryRequest.pattern =
            "48 8B 8C 24 D0 00 00 00 41 0F B6 14 24 48 8B 49 08 "
            "E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
            "40 38 78 30 0F 85 ?? ?? ?? ?? 49 8B EE 45 85 F6";

        foundableCategoryRequest.followRel32At = 0x11;
        foundableCategoryRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        foundableCategoryRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t foundableCategoryAddress =
            scanner->Resolve(self, &foundableCategoryRequest);

        if (foundableCategoryAddress == 0)
        {
            return false;
        }

        addresses.findFoundableMarkerCategoryData =
            foundableCategoryAddress;


        PluginScanRequest massSubsystemRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        massSubsystemRequest.hookName =
            "MiniMap::UWorld::GetSubsystem<UMassEntitySubsystem>";

        massSubsystemRequest.pattern =
            "49 8B CD E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C8 "
            "E8 ?? ?? ?? ?? 48 8B D8 EB ?? 33 DB 48 83 7B 38 00 75 ?? "
            "4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ??";

        massSubsystemRequest.followRel32At = 0x10;
        massSubsystemRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        massSubsystemRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t massSubsystemAddress =
            scanner->Resolve(self, &massSubsystemRequest);

        if (massSubsystemAddress == 0)
        {
            return false;
        }

        addresses.getMassEntitySubsystem =
            massSubsystemAddress;


        PluginScanRequest massQueryCtorRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        massQueryCtorRequest.hookName =
            "MiniMap::FMassEntityQuery::FMassEntityQuery";

        massQueryCtorRequest.pattern =
            "48 8D 54 24 60 48 8D 8D 80 03 00 00 E8 ?? ?? ?? ?? "
            "48 8B 5C 24 68 48 85 DB 74 ?? 41 8B C4 F0 0F C1 43 08";

        massQueryCtorRequest.followRel32At = 0x0C;
        massQueryCtorRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        massQueryCtorRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t massQueryCtorAddress =
            scanner->Resolve(self, &massQueryCtorRequest);

        if (massQueryCtorAddress == 0)
        {
            return false;
        }

        addresses.massQueryConstruct =
            massQueryCtorAddress;


        PluginScanRequest massRequirementsRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        massRequirementsRequest.pattern =
            "41 B1 01 48 8D 8D D0 06 00 00 0F 28 D6 48 8B D6 "
            "E8 ?? ?? ?? ?? 45 33 C0 48 8D 8D 80 03 00 00 B2 01 "
            "E8 ?? ?? ?? ?? 45 33 C0 48 8D 8D 80 03 00 00 B2 01 "
            "E8 ?? ?? ?? ?? 33 D2 48 8D 8D 80 03 00 00 E8 ?? ?? ?? ??";

        massRequirementsRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        massRequirementsRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        massRequirementsRequest.hookName =
            "MiniMap::AddRequirement<FCrInventoryFragment>";
        massRequirementsRequest.followRel32At = 0x21;

        const uintptr_t inventoryRequirementAddress =
            scanner->Resolve(self, &massRequirementsRequest);

        if (inventoryRequirementAddress == 0)
        {
            return false;
        }

        addresses.addInventoryRequirement =
            inventoryRequirementAddress;

        massRequirementsRequest.hookName =
            "MiniMap::AddRequirement<FTransformFragment>";
        massRequirementsRequest.followRel32At = 0x32;

        const uintptr_t transformRequirementAddress =
            scanner->Resolve(self, &massRequirementsRequest);

        if (transformRequirementAddress == 0)
        {
            return false;
        }

        addresses.addTransformRequirement =
            transformRequirementAddress;

        massRequirementsRequest.hookName =
            "MiniMap::AddConstSharedRequirement<"
            "FCrMassFoundableParameters>";
        massRequirementsRequest.followRel32At = 0x40;

        const uintptr_t foundableParametersRequirementAddress =
            scanner->Resolve(self, &massRequirementsRequest);

        if (foundableParametersRequirementAddress == 0)
        {
            return false;
        }

        addresses.addFoundableParametersRequirement =
            foundableParametersRequirementAddress;


        PluginScanRequest foundableTagRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        foundableTagRequest.hookName =
            "MiniMap::AddTagRequirement<FCrMassFoundableTag>";

        foundableTagRequest.pattern =
            "40 53 48 83 EC 20 48 8B DA 33 D2 48 8B CB E8 ?? ?? ?? ?? "
            "48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
            "45 33 C0 B2 02 48 8B CB E8 ?? ?? ?? ?? "
            "41 B0 03 B2 02 48 8B CB E8 ?? ?? ?? ?? "
            "33 D2 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ??";

        foundableTagRequest.followRel32At = 0x0E;
        foundableTagRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        foundableTagRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t foundableTagAddress =
            scanner->Resolve(self, &foundableTagRequest);

        if (foundableTagAddress == 0)
        {
            return false;
        }

        addresses.addFoundableTagRequirement =
            foundableTagAddress;


        PluginScanRequest massQueryDtorRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        massQueryDtorRequest.hookName =
            "MiniMap::FMassEntityQuery::~FMassEntityQuery";

        massQueryDtorRequest.pattern =
            "48 8D 8D D0 06 00 00 E8 ?? ?? ?? ?? "
            "48 8D 8D 80 03 00 00 E8 ?? ?? ?? ?? "
            "48 8D 4D 30 E8 ?? ?? ?? ?? "
            "48 8D 8D B0 0B 00 00 E8 ?? ?? ?? ??";

        massQueryDtorRequest.followRel32At = 0x13;
        massQueryDtorRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        massQueryDtorRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t massQueryDtorAddress =
            scanner->Resolve(self, &massQueryDtorRequest);

        if (massQueryDtorAddress == 0)
        {
            return false;
        }

        addresses.massQueryDestruct =
            massQueryDtorAddress;


        PluginScanRequest matchingHandlesRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        matchingHandlesRequest.hookName =
            "MiniMap::FMassEntityQuery::GetMatchingEntityHandles";

        matchingHandlesRequest.pattern =
            "40 53 41 54 41 56 41 57 48 83 EC 48 "
            "48 8B D9 4C 8D 62 08 33 C9 4C 8B F2 "
            "48 89 0A 41 89 0C 24 89 4A 0C 48 8B CB "
            "E8 ?? ?? ?? ?? "
            "4C 8B BB F8 02 00 00 48 63 83 00 03 00 00";

        matchingHandlesRequest.flags = 0;
        matchingHandlesRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t matchingHandlesAddress =
            scanner->Resolve(self, &matchingHandlesRequest);

        if (matchingHandlesAddress == 0)
        {
            return false;
        }

        addresses.getMatchingEntityHandles =
            matchingHandlesAddress;


        PluginScanRequest fragmentDataRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        fragmentDataRequest.hookName =
            "MiniMap::FMassEntityManager::"
            "InternalGetFragmentDataPtr";

        fragmentDataRequest.pattern =
            "E8 ?? ?? ?? ?? "
            "4C 8B C0 48 8B D3 48 8B CE "
            "E8 ?? ?? ?? ?? "
            "48 85 C0 0F 84 ?? ?? ?? ?? "
            "0F 10 48 20 66 41 0F 2F 4D 30 "
            "0F 10 40 30 0F 11 4D EF F2 0F 11 45 FF";

        fragmentDataRequest.followRel32At = 0x0E;
        fragmentDataRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        fragmentDataRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t fragmentDataAddress =
            scanner->Resolve(self, &fragmentDataRequest);

        if (fragmentDataAddress == 0)
        {
            return false;
        }

        addresses.getMassFragmentDataPtr =
            fragmentDataAddress;

        fragmentDataRequest.hookName =
            "MiniMap::FTransformFragment::StaticStruct";
        fragmentDataRequest.followRel32At = 0x00;

        const uintptr_t transformStaticStructAddress =
            scanner->Resolve(self, &fragmentDataRequest);

        if (transformStaticStructAddress == 0)
        {
            return false;
        }

        addresses.transformFragmentStaticStruct =
            transformStaticStructAddress;


        PluginScanRequest constSharedDataRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        constSharedDataRequest.hookName =
            "MiniMap::FMassEntityManager::"
            "InternalGetConstSharedFragmentPtr";

        constSharedDataRequest.pattern =
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
        constSharedDataRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        constSharedDataRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t constSharedDataAddress =
            scanner->Resolve(self, &constSharedDataRequest);

        if (constSharedDataAddress == 0)
        {
            return false;
        }

        addresses.getMassConstSharedFragmentPtr =
            constSharedDataAddress;


        PluginScanRequest inventoryStaticStructRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        inventoryStaticStructRequest.hookName =
            "MiniMap::FCrInventoryFragment::StaticStruct";

        inventoryStaticStructRequest.pattern =
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
        inventoryStaticStructRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        inventoryStaticStructRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t inventoryStaticStructAddress =
            scanner->Resolve(self, &inventoryStaticStructRequest);

        if (inventoryStaticStructAddress == 0)
        {
            return false;
        }

        addresses.inventoryFragmentStaticStruct =
            inventoryStaticStructAddress;


        PluginScanRequest foundableParametersStructRequest =
            PLUGIN_SCAN_REQUEST_INIT;

        foundableParametersStructRequest.hookName =
            "MiniMap::FCrMassFoundableParameters::StaticStruct";

        foundableParametersStructRequest.pattern =
            "4D 63 B5 30 03 00 00 48 0F 44 D8 48 C1 E6 05 48 03 F3 "
            "48 3B DE 74 ?? E8 ?? ?? ?? ?? 48 39 03 74 ?? "
            "48 83 C3 20 48 3B DE 75 ??";

        foundableParametersStructRequest.followRel32At = 0x17;
        foundableParametersStructRequest.flags =
            PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        foundableParametersStructRequest.kind =
            PLUGIN_SCAN_FUNCTION_START;

        const uintptr_t foundableParametersStructAddress =
            scanner->Resolve(self, &foundableParametersStructRequest);

        if (foundableParametersStructAddress == 0)
        {
            return false;
        }

        addresses.foundableParametersStaticStruct =
            foundableParametersStructAddress;
        return true;
    }
}