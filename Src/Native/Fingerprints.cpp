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

        // Alien Actors: each anchor verified unique in client CL-127004.
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::AddRequirement<FMassEnemyStateFragment>";
            request.pattern =
                "45 33 C0 B2 02 48 8B CB E8 ?? ?? ?? ?? 41 B0 03 "
                "B2 02 48 8B CB E8 ?? ?? ?? ?? 33 D2 48 8B CB E8 "
                "?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D0 48 8D 8F 58 03 "
                "00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D0 48 8D "
                "8F 58 03 00 00";
            request.followRel32At = 8;
            request.flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.resultOffset = 0;
            request.module = nullptr;
            addresses.addEnemyStateRequirement = scanner->Resolve(self, &request);
            if (addresses.addEnemyStateRequirement == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: AddRequirement<FMassEnemyStateFragment> = 0x%llX",
                static_cast<unsigned long long>(addresses.addEnemyStateRequirement));
        }

        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::FMassEnemyStateFragment::StaticStruct";
            request.pattern =
                "0F 10 46 48 0F 11 00 F2 0F 10 4E 58 F2 0F 11 48 "
                "10 E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8 ?? ?? ?? "
                "?? 48 8B F8 E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8 "
                "?? ?? ?? ?? 48 8B D8 E8 ?? ?? ?? ?? 48 8B D0 49 "
                "8B CE";
            request.followRel32At = 17;
            request.flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.resultOffset = 0;
            request.module = nullptr;
            addresses.enemyStateFragmentStaticStruct = scanner->Resolve(self, &request);
            if (addresses.enemyStateFragmentStaticStruct == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassEnemyStateFragment::StaticStruct = 0x%llX",
                static_cast<unsigned long long>(addresses.enemyStateFragmentStaticStruct));
        }

        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::FMassEnemyTag::StaticStruct";
            request.pattern =
                "48 8B 9A C8 00 00 00 49 8B F8 4C 8B F2 48 8B F1 "
                "E8 ?? ?? ?? ?? 48 8D 4B 20 8B D0 E8 ?? ?? ?? ?? "
                "E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8 ?? ?? ?? ?? "
                "E8 ?? ?? ?? ?? 48 8B D0 49 8B CE";
            request.followRel32At = 32;
            request.flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.resultOffset = 0;
            request.module = nullptr;
            addresses.enemyTagStaticStruct = scanner->Resolve(self, &request);
            if (addresses.enemyTagStaticStruct == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassEnemyTag::StaticStruct = 0x%llX",
                static_cast<unsigned long long>(addresses.enemyTagStaticStruct));
        }

        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::FMassNeutralAITag::StaticStruct";
            request.pattern =
                "E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 20 E8 ?? ?? "
                "?? ?? 48 83 C4 50 5F 5E 5B C3 32 C0 48 83 C4 50";
            request.followRel32At = 0;
            request.flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.resultOffset = 0;
            request.module = nullptr;
            addresses.neutralTagStaticStruct = scanner->Resolve(self, &request);
            if (addresses.neutralTagStaticStruct == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassNeutralAITag::StaticStruct = 0x%llX",
                static_cast<unsigned long long>(addresses.neutralTagStaticStruct));
        }

        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::FMassFragmentRequirements::AddTagRequirement";
            request.pattern =
                "48 8B 03 48 8B CB FF 90 10 03 00 00 45 33 C0 49 "
                "8B CF 48 8B D0 E8 ?? ?? ?? ?? 48 8B 03 48 8D 54 "
                "24 20 48 8B CB FF 90 18 03 00 00";
            request.followRel32At = 21;
            request.flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.resultOffset = 0;
            request.module = nullptr;
            addresses.addTagRequirement = scanner->Resolve(self, &request);
            if (addresses.addTagRequirement == 0)
            {
                return false;
            }
            LOG_DEBUG("MiniMap: FMassFragmentRequirements::AddTagRequirement = 0x%llX",
                static_cast<unsigned long long>(addresses.addTagRequirement));
        }

        // Optional grid/collector functions. Each pattern is unique in CL-127004
        // executable and its target ABI was checked against the matching PDB.
        // These resolve functions, not hooks. Failure disables dependent stages.
        const auto resolveGridDiagnostic = [&](const char* name, const char* pattern,
            int32_t followAt, uintptr_t& target)
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = name;
            request.pattern = pattern;
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = followAt >= 0 ? PLUGIN_SCAN_FLAG_FOLLOW_REL32 : 0;
            request.followRel32At = followAt >= 0 ? followAt : 0;
            request.resultOffset = 0;
            request.module = nullptr;
            target = scanner->Resolve(self, &request);
            LOG_DEBUG("MiniMap: building native: resolve %s = 0x%llX", name,
                static_cast<unsigned long long>(target));
        };
        // Stage B.1 optional function resolutions, not hooks. PDB/EXE checked:
        // spline anchor unique at 074B9ABE -> 0748E400; built anchor unique at
        // 044657F0, whose first call is IsEntityValid (044658A0).
        resolveGridDiagnostic("MiniMap::AddRequirement<FAuSplineConnectionFragment>",
            "45 33 C0 48 8D 8B E0 00 00 00 B2 01 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
            "48 8D 8B 78 03 00 00 8B D0 E8 ?? ?? ?? ?? 80 8B 98 03 00 00 01",
            12, addresses.addSplineRequirement);
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
        resolveGridDiagnostic("MiniMap::UCrEntityGridSubsystem::FindEntitiesInBox",
            gridBoxPattern, -1, addresses.gridFindEntitiesInBox);
        // CL-127004: unique existing box anchor, call at +14D -> 07714720.
        // PDB: (const FVector&, float, const TFunctionRef<bool(const
        // FCrGridEntityHandle&)>&, bool). Final bool gates IsInRadius only;
        // cell selection is XY at both hierarchy levels regardless of Z.
        resolveGridDiagnostic("MiniMap::UCrEntityGridSubsystem::ForEachCellInRadius",
            gridBoxPattern, 0x14D, addresses.gridForEachCellInRadius);
        // The first call in FindEntitiesInBox destroys its previous results.
        // Linker-folded symbol name may mention FCulture, but verified code
        // destroys 24-byte elements and releases the controller at +0x10.
        resolveGridDiagnostic("MiniMap::FCrGridEntityHandle::DestructItems",
            gridBoxPattern, 0x47, addresses.gridDestructResultItems);
        resolveGridDiagnostic("MiniMap::FMassEntityView::Construct(manager,handle)",
            "4C 8B DC 49 89 5B 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 "
            "45 33 F6 48 C7 41 18 FF FF FF FF 4C 89 71 08 BD FF FF FF FF 89 69 10 "
            "49 8B D8 4C 89 71 20 48 8B F9 48 89 19 45 85 C0 0F 84 C6 00 00 00 "
            "0F B6 42 60 48 8D 72 20 4C 8D 3D ?? ?? ?? ?? 45 88 73 08 48 8B D6",
            -1, addresses.massEntityViewConstruct);
        resolveGridDiagnostic("MiniMap::FMassEntityView::HasTag",
            "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 83 C1 08 "
            "48 8B 53 20 E8 ?? ?? ?? ?? 84 C0 75 26 4C 8D 0D ?? ?? ?? ?? "
            "41 B8 58 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
            "E8 ?? ?? ?? ?? 84 C0 74 02 90 CC 48 8B 5B 20 48 85 FF 75 26 "
            "4C 8D 0D ?? ?? ?? ?? 41 B8 02 01 00 00 48 8D 15 ?? ?? ?? ??",
            -1, addresses.massEntityViewHasTag);

#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Spawn";
            request.pattern = "48 89 5C 24 18 55 56 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 00 00 00 48 8B 19 48 8B EA 48 8B F1 48 85 DB 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8D 50 30 48 63 40 38 3B 43 38 0F 8F ?? ?? ?? ?? 48 8B C8 48 8B 43 30 48 39 14 C8 0F 85 ?? ?? ?? ?? 48 8B 1E 48 85 DB 0F 84 ?? ?? ?? ?? 48 85 ED 75 ?? 80 3D ?? ?? ?? ?? 03 0F 82 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 FF 48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 75 ?? 48 8B 06 EB ?? 48 8B C7 48 39 B8 E0 00 00 00 0F 84 ?? ?? ?? ?? 48 8B 1E 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 75 ?? 48 8B 1E EB ?? 48 8B DF 40 38 3D ?? ?? ?? ?? 48 8B 9B E0 00 00 00 74 ?? 48 85 DB 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 45 10 48 83 C3 30 48 63 4B 08 3B 48 38 7F ?? 48 8B 40 30 48 39 1C C8 0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 03 0F 82 ?? ?? ?? ?? 48 8B 1E 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 50 30 48 63 40 38 3B 43 38 7F ?? 48 8B C8 48 8B 43 30 48 39 14 C8 75 ?? 48 8B 3E 48 8B 8F E0 00 00 00 48 85 C9 75 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? EB ?? 45 33 C0 48 8D 54 24 20 E8 ?? ?? ?? ?? 83 7C 24 28 00 48 8D 3D ?? ?? ?? ?? 48 8B DF 48 8D 54 24 30 48 0F 45 5C 24 20 48 8B CD 45 33 C0 E8 ?? ?? ?? ?? 83 7C 24 38 00 48 8D 15 ?? ?? ?? ?? 4C 8B CB 48 8D 0D ?? ?? ?? ?? 48 0F 45 7C 24 30 4C 8B C7 E8 ?? ?? ?? ?? 48 8B 4C 24 30 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.spawnObject = scanner->Resolve(self, &request);
            if (!addresses.spawnObject) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Transient";
            request.pattern = "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 4C 8B 11 33 C0 48 63 49 08 4D 8D 1C CA 4D 3B D3 0F 84 ?? ?? ?? ?? 66 66 0F 1F 84 00 00 00 00 00 49 8B 12 41 B9 B9 79 37 9E 48 C1 EA 04 49 83 C2 08 8B CA C1 E9 10 33 CA 69 D1 6B CA EB 85 8B CA C1 E9 0D 33 CA 69 D1 35 AE B2 C2 44 8B C2 41 C1 E8 10 44 33 C2 41 2B C0 41 8B C8 C1 E9 0D 33 C1 44 2B C8 8B D0 C1 E0 08 45 2B C8 44 33 C8 41 8B C0 41 2B C1 41 8B C9 2B C2 C1 E9 0D 33 C1 41 2B D1 2B D0 8B C8 C1 E9 0C 33 D1 44 2B CA 8B CA 44 2B C8 C1 E1 10 44 33 C9 41 2B C1 41 8B C9 2B C2 C1 E9 05 33 C1 41 2B D1 2B D0 8B C8 C1 E9 03 33 D1 44 2B CA 8B CA 44 2B C8 C1 E1 0A 44 33 C9 41 2B C1 41 C1 E9 0F 2B C2 41 33 C1 4D 3B D3 0F 85 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC CC CC 48 8B 49 20 48 85 C9 74 ?? 48 8B 01 48 FF A0 80 01 00 00 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA C0 0B 00 00 48 8B 04 C8 C6 04 02 00 33 C0 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 8B C4 55 56 48 8D 68 A1 48 81 EC B8 00 00 00 48 89 58 E8 48 89 78 E0 48 8B F9 4C 89 68 D0 45 33 ED 4C 89 70 C8 44 89 6D 67 44 8B 05 ?? ?? ?? ?? 4C 89 78 C0 4C 8B FA 65 48 8B 04 25 58 00 00 00 41 BE C0 0B 00 00 4A 8B 1C C0 4C 03 F3 41 C6 06 01 48 8B 01 FF 90 80 01 00 00 48 8B F0 45 38 2E 0F 85 ?? ?? ?? ?? 4C 89 A4 24 A0 00 00 00 41 BC B0 17 00 00 41 8B 0C 1C 39 0D ?? ?? ?? ?? 0F 8F ?? ?? ?? ?? 41 8B 04 1C 39 05 ?? ?? ?? ?? 4C 8B A4 24 A0 00 00 00 0F 8F ?? ?? ?? ?? 48 8B 47 10 48 8D 0D ?? ?? ?? ?? 48 89 45 6F FF 15 ?? ?? ?? ?? 48 8B 5D 6F 48 8B CB 48 C1 E9 04 8B C1 C1 E8 10 33 C1 69 C8 6B CA EB 85 8B C1 C1 E8 0D 33 C1";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.getTransientPackage = scanner->Resolve(self, &request);
            if (!addresses.getTransientPackage) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Flags";
            request.pattern = "44 8B 41 08 41 8B C0 0B C2 41 3B C0 74 ?? F0 09 51 08 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 8B 12 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 40 53 48 83 EC 30 48 8B 19 48 8B C2 48 8B C8 48 8D 54 24 20 E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 48 8B 4B 08 48 8D 54 24 40 48 89 44 24 40 48 8B 03 FF D0 48 8B 4C 24 20 0F B6 D8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 0F B6 C3 48 83 C4 30 5B C3 CC CC CC CC CC CC CC 48 89 5C 24 08 57 48 83 EC 30 0F 10 0A 48 8B 51 08 BF 3A 00 00 00 0F 29 4C 24 20 44 8B 02 41 83 F8 01 41 8D 40 01 89 02 B8 2E 00 00 00 48 8B 19 66 0F 45 F8 48 8B 43 08 48 83 C0 02 48 3B 43 10 72 ?? BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ?? 0F 28 4C 24 20 48 8B 43 08 66 0F 6F C1 66 0F 73 D8 08 66 48 0F 7E CA 66 41 0F 7E C0 48 8B CB 66 89 38 48 83 43 08 02 48 8B 5C 24 40 48 83 C4 30 5F E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 4D 8B 00 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC 48 8B 12 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 48 8B 12 E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC 40 55 53 57 48 8D AC 24 50 F8 FF FF 48 81 EC B0 08 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 A0 07 00 00 48 8B F9 48 8B DA 8B 4A 18 8B C1 C1 E8 1F 84 C0 0F 85 ?? ?? ?? ?? 81 E1 FF FF FF 7F 77 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 31 06 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 83 F9 01 0F 87 ?? ?? ?? ??";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.setObjectFlags = scanner->Resolve(self, &request);
            if (!addresses.setObjectFlags) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Retain";
            request.pattern = "40 53 48 83 EC 30 8B 59 0C 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 0D 04 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 3B D8 0F 8D ?? ?? ?? ?? 48 89 74 24 40 48 89 7C 24 48 8B FB C1 EF 10 0F B7 F3 3B D8 7D ?? 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 89 5C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 72 ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 7C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B D8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 00 89 5C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 1C F8 48 8B 7C 24 48 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8D 0C 76 48 8B 74 24 40 48 8D 1C CB 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 4B 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 33 DB F0 FF 43 14 8B 43 08 0F BA E0 1D 72 ?? BA 00 00 00 20 48 8B CB 48 83 C4 30 5B E9 ?? ?? ?? ?? 48 83 C4 30 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 10 57 48 83 EC 20 48 8B 02 48 8B FA 48 8B D9 48 8D 51 78 45 33 C9 45 33 C0 48 8B CF FF 90 88 00 00 00 F6 43 10 01 74 ?? 48 8B 43 10 48 83 E0 FE EB ?? 33 C0 80 3D ?? ?? ?? ?? 00 48 89 44 24 30 74 ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 44 24 30 48 85 C0 74 ?? 48 8B 07";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.retainObject = scanner->Resolve(self, &request);
            if (!addresses.retainObject) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Release";
            request.pattern = "80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? C3 CC CC 40 53 55 56 57 41 54 41 56 41 57 48 83 EC 50 48 8B EA 4C 8B F9 48 8B D1 49 8B D8 48 8D 4C 24 30 E8 ?? ?? ?? ?? 4D 8B 67 08 49 8D 77 08 4D 63 77 10 49 8B FC 49 C1 E6 04 4D 03 F4 4D 3B E6 74 ?? 33 C0 88 84 24 90 00 00 00 48 39 5F 08 75 ?? 88 84 24 98 00 00 00 48 85 ED 74 ?? 48 8B CF 88 84 24 A0 00 00 00 E8 ?? ?? ?? ?? 48 3B E8 74 ?? 48 83 C7 10 49 3B FE 75 ?? 49 8D 77 08 41 B0 01 48 8D 94 24 A8 00 00 00 48 8B CE E8 ?? ?? ?? ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 83 C4 50 41 5F 41 5E 41 5C 5F 5E 5D 5B C3 49 2B FC 49 8D 77 08 48 C1 FF 04 83 FF FF 74 ?? 48 63 56 08 48 8D 5E 08 33 C0 8B CF F7 D1 48 8B EE C1 E9 1F 3B FA 0F 4C C1 85 C0 75 ?? 48 89 54 24 28 4C 8D 0D ?? ?? ?? ?? 48 63 CF 48 8D 15 ?? ?? ?? ?? 48 89 4C 24 20 41 B8 2B 04 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 49 8D 6F 08 8B 0B BA 01 00 00 00 4C 8B 0E 8B C1 2B C7 FF C8 3B C2 0F 4F C2 85 C0 74 ?? 2B C8 4C 63 C0 48 63 D1 48 C1 E2 04 48 63 CF 49 03 D1 48 C1 E1 04 49 03 C9 49 C1 E0 04 E8 ?? ?? ?? ?? 48 8B F5 FF 0B E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 18 55 48 83 EC 20 8B 41 08 48 8B EA 48 8B D9 3B 41 34 0F 84 ?? ?? ?? ?? 4C 8B C2 48 89 74 24 30 49 C1 E8 04 41 8B C0 48 89 7C 24 38 C1 E8 10 33 FF 41 33 C0 4C 8B 43 40 69 D0 6B CA EB 85 8B C2 C1 E8 0D 33 C2 69 C8 35 AE B2 C2 48 63 C1 8B D1 48 C1 EA 10 48 33 D0 8B 43 48 FF C8 48 63 C8 48 8D 43 38 48 23 D1 4D 85 C0 4C 0F 44 C0 41 8B 04 90 49 8D 34 90 83 F8 FF 74 ?? 66 90 48 63 C8 48 8D 14 89 48 8B 0B 48 39 2C D1 4C 8D 04 D1";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.releaseObject = scanner->Resolve(self, &request);
            if (!addresses.releaseObject) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Register";
            request.pattern = "48 89 5C 24 18 48 89 74 24 20 57 41 54 41 57 48 81 EC A0 00 00 00 8B 79 0C 48 8D 1D ?? ?? ?? ?? 4D 8B E0 4C 8B FA 48 8B F1 85 FF 79 ?? 4C 8B CB 48 8D 15 ?? ?? ?? ?? 41 B8 0D 04 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 48 89 AC 24 C0 00 00 00 4C 89 B4 24 C8 00 00 00 3B F8 0F 8D ?? ?? ?? ?? 8B EF 44 0F B7 F7 C1 ED 10 3B F8 7D ?? 85 FF 79 ?? 4C 8D 0D ?? ?? ?? ?? 89 7C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B E8 72 ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 6C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 00 89 7C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 3C E8 48 85 FF 75 ?? 4C 8B CB 48 8D 15 ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 4B 8D 0C 76 48 8D 3C CF 48 85 FF 75 ?? 4C 8B CB 48 8D 15 ?? ?? ?? ?? 41 B8 4B 03 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 33 FF 8B 47 08 C1 E8 1C A8 01 74 ?? 45 33 C9 48 8D 54 24 50 45 33 C0 48 8B CE E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8B C3 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 3E 07 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 50 0F B6 F8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 40 84 FF 74 ?? 90 CC 8B 46 08 C1 E8 1E F6 D0 A8 01 75 ?? 80 3D ?? ?? ?? ?? 05 0F 82 ?? ?? ?? ?? 45 33 C0";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.registerComponent = scanner->Resolve(self, &request);
            if (!addresses.registerComponent) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Unregister";
            request.pattern = "40 53 48 83 EC 50 F6 81 94 00 00 00 01 48 8B D9 75 ?? 80 3D ?? ?? ?? ?? 05 0F 82 ?? ?? ?? ?? 45 33 C0 48 8D 54 24 30 E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8B C0 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 30 48 85 C9 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C4 50 5B C3 48 83 B9 A8 00 00 00 00 75 ?? 45 33 C9 48 89 7C 24 60 45 33 C0 48 8D 54 24 40 E8 ?? ?? ?? ?? 83 78 08 00 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 E5 07 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 40 0F B6 F8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 40 84 FF 48 8B 7C 24 60 74 ?? 90 CC 33 D2 48 8B CB E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 C7 83 A8 00 00 00 00 00 00 00 48 83 C4 50 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC 48 83 EC 48 33 C0 48 89 02 48 89 42 08 48 89 42 10 48 89 42 18 48 89 42 20 48 89 42 28 88 42 30 48 8B C2 4C 8B 81 90 00 00 00 4D 85 C0 74 ?? 41 0F 10 90 D0 00 00 00 C6 44 24 30 01 F2 41 0F 10 88 C8 00 00 00 41 0F 10 80 B8 00 00 00 0F 11 54 24 18 F2 41 0F 10 90 E0 00 00 00 0F 11 02 0F 10 44 24 10 F2 0F 10 C1 0F 10 4C 24 20 0F 11 42 10 F2 0F 10 44 24 30 66 0F 14 CA 0F 11 4A 20 F2 0F 11 42 30 48 83 C4 48 C3 CC CC CC CC CC CC CC CC 48 83 EC 48 33 C0 48 89 02 48 89 42 08 48 89 42 10 48 89 42 18 48 89 42 20 48 89 42 28 88 42 30 48 8B C2 4C 8B 41 30 4D 85 C0 74 ?? 41 0F 10 90 E0 00 00 00 C6 44 24 30 01 F2 41 0F 10 88 D8 00 00 00 41 0F 10 80 C8 00 00 00 0F 11 54 24 18 F2 41 0F 10 90 F0 00 00 00 0F 11 02 0F 10 44 24 10";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.unregisterComponent = scanner->Resolve(self, &request);
            if (!addresses.unregisterComponent) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::DestroyComponent";
            request.pattern = "4C 8B DC 41 56 48 81 EC A0 00 00 00 0F B6 81 97 00 00 00 4C 8B F1 A8 40 0F 85 ?? ?? ?? ?? 0C 40 49 89 5B 10 49 89 6B F0 88 81 97 00 00 00 A8 20 74 ?? 48 8B 01 33 D2 FF 90 F0 03 00 00 41 F6 86 97 00 00 00 08 74 ?? 49 8B 06 49 8B CE FF 90 F8 03 00 00 41 80 A6 97 00 00 00 EF 33 ED 41 F6 86 94 00 00 00 01 0F 84 ?? ?? ?? ?? 49 39 AE A8 00 00 00 75 ?? 45 33 C9 48 8D 54 24 60 45 33 C0 49 8B CE E8 ?? ?? ?? ?? 39 68 08 74 ?? 48 8B 00 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 E5 07 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4C 24 60 0F B6 D8 48 85 C9 74 ?? E8 ?? ?? ?? ?? 84 DB 74 ?? 90 CC 33 D2 49 8B CE E8 ?? ?? ?? ?? 49 8B CE E8 ?? ?? ?? ?? 49 89 AE A8 00 00 00 49 8B 9E A0 00 00 00 48 89 B4 24 90 00 00 00 48 89 BC 24 88 00 00 00 48 89 5C 24 58 48 85 DB 0F 84 ?? ?? ?? ?? 41 0F B6 86 B1 00 00 00 FE C8 3C 01 76 ?? 49 8B D6 48 8B CB E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 40 38 2D ?? ?? ?? ?? 49 8B CE 48 89 4C 24 50 74 ?? E8 ?? ?? ?? ?? 48 8B 4C 24 50 48 8B 93 90 02 00 00 48 8D 44 24 50 48 3B C2 72 ?? 4C 63 83 9C 02 00 00 4C 8D 4C 24 50 4A 8D 04 C2 4C 3B C8 73 ?? 48 63 83 98 02 00 00 4C 8D 0D ?? ?? ?? ?? 48 C7 44 24 40 08 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 44 24 38 48 8D 44 24 50 4C 89 44 24 30 41 B8 AC 07 00 00 48 89 54 24 28 48 8D 15 ?? ?? ?? ?? 48 89 44 24 20 E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 4C 24 50 48 63 BB 98 02 00 00 85 FF 0F 84 ?? ?? ?? ?? 4C 89 A4 24 80 00 00 00 8B F5 40 38 35 ?? ?? ?? ?? 4C 8B A3 90 02 00 00 48 8B D9 4C 89 6C 24 78 44 8B ED 4C 89 7C 24 70 89 AC 24 C0 00 00 00";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.destroyComponent = scanner->Resolve(self, &request);
            if (!addresses.destroyComponent) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::InitTarget";
            request.pattern = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 41 0F B6 E9 41 8B F8 8B F2 48 8B D9 85 D2 74 ?? 45 85 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 89 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 40 0F B6 CD E8 ?? ?? ?? ?? 84 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 0F B6 83 62 01 00 00 24 FE 89 B3 48 01 00 00 0A 44 24 60 89 BB 4C 01 00 00 40 88 AB 68 01 00 00 88 83 62 01 00 00 81 FE 00 00 01 00 76 ?? 48 8D 4C 24 40 48 89 5C 24 40 E8 ?? ?? ?? ?? 84 C0 75 ?? C7 83 48 01 00 00 01 00 00 00 81 BB 4C 01 00 00 00 00 01 00 76 ?? 48 8D 4C 24 20 48 89 5C 24 20 E8 ?? ?? ?? ?? 84 C0 75 ?? C7 83 4C 01 00 00 01 00 00 00 48 8B 03 48 8B CB 48 8B 5C 24 48 48 8B 6C 24 50 48 8B 74 24 58 48 83 C4 30 5F 48 FF A0 28 03 00 00 CC CC CC CC CC CC CC CC CC 4C 8B DC 55 53 56 49 8D AB 28 FF FF FF 48 81 EC C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 49 89 7B 18 48 8B F1 48 8B 89 90 00 00 00 4D 89 6B D8 45 33 ED 4D 89 73 D0 4D 89 7B C8 4C 8B FA 44 39 A9 48 01 00 00 0F 8E ?? ?? ?? ?? 44 39 A9 4C 01 00 00 0F 8E ?? ?? ?? ?? 44 39 A9 50 01 00 00 0F 8E ?? ?? ?? ?? 48 8B 01 4D 89 63 E0 FF 90 D8 03 00 00 4C 8B 8E 90 00 00 00 41 BC 01 00 00 00 F6 D8 4C 89 AD 80 00 00 00 41 8B FC 4C 89 AD 88 00 00 00 48 1B C9 41 8B 81 48 01 00 00 83 E1 10 45 8B 81 44 01 00 00 8B D1 41 0F B7 99 50 01 00 00 48 0F BA EA 11 41 F6 C0 02 89 44 24 30 41 8B 81 4C 01 00 00 48 0F 44 D1 89 44 24 34 8B 0D ?? ?? ?? ?? 4C 8B F2 49 8B 01 49 0F BA EE 18";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.initRenderTarget = scanner->Resolve(self, &request);
            if (!addresses.initRenderTarget) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::TargetResource";
            request.pattern = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 38 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 BD 00 00 00 00 00 01 00 00 48 FF C3 41 8B F0 48 89 1D ?? ?? ?? ?? 48 8B FA 48 3B DD 75 ?? 48 8D 4C 24 40 C6 44 24 40 00 48 8B DD E8 ?? ?? ?? ?? 84 C0 B9 01 00 00 00 48 0F 44 D9 48 C7 07 00 00 00 00 81 FE FF FF FF 00 76 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 3D 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 3B DD 72 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 3E 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 6C 24 38 48 8B C7 48 C1 E3 18 48 0B DE 48 8B 74 24 48 48 89 1F 48 8B 5C 24 30 48 83 C4 20 5F C3 CC CC CC CC CC CC CC 48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA B0 17 00 00 48 8B 04 C8 8B 04 02 39 05 ?? ?? ?? ?? 7F ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 45 33 C9 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 50 70 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 33 C0 48 39 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.getRenderTargetResource = scanner->Resolve(self, &request);
            if (!addresses.getRenderTargetResource) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::SetMesh";
            request.pattern = "40 55 53 56 57 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 48 80 3D ?? ?? ?? ?? 00 48 8B DA 48 8B B9 88 05 00 00 48 8B F1 74 ?? 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? 48 3B FB 0F 94 C0 84 C0 74 ?? 32 C0 E9 ?? ?? ?? ?? 48 85 FF 75 ?? 48 85 DB EB ?? 48 8B 86 A8 00 00 00 48 8B BE A0 00 00 00 4C 89 B4 24 98 01 00 00 48 85 C0 75 ?? 48 8B CE E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? F6 86 94 00 00 00 01 0F 84 ?? ?? ?? ?? 38 86 A3 01 00 00 0F 85 ?? ?? ?? ?? 48 85 FF 0F 84 ?? ?? ?? ?? 41 B8 01 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 68 E8 ?? ?? ?? ?? 48 8D 54 24 68 48 8D 4D F8 E8 ?? ?? ?? ?? 45 33 C0 48 8D 55 B8 48 8B CE 4C 8B F0 E8 ?? ?? ?? ?? 48 8B D0 48 8D 4D D8 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B F0 48 8D 4C 24 60 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 64 E8 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 4C 8D 44 24 60 48 8D 54 24 64 48 8D 4D A8 E8 ?? ?? ?? ?? 48 8B D0 48 8D 4D E8 41 B0 01 E8 ?? ?? ?? ?? 48 8B D6 C7 45 28 04 00 00 00 48 8D 4D 38 48 8B F8 E8 ?? ?? ?? ?? 48 83 7D 38 00 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 69 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 33 DB C7 44 24 58 01 00 00 00 48 8D 44 24 5C 48 89 5C 24 50 48 89 44 24 30 45 33 C9 48 8D 44 24 50 C7 44 24 28 01 00 00 00 8D 53 08 48 89 44 24 20 8D 4B 20 44 8D 43 01 E8 ?? ?? ?? ?? 48 8B 4C 24 50 44 8D 43 01 48 8D 55 28 E8 ?? ?? ?? ?? 48 8B 07 48 89 44 24 78 48 8B 47 08 48 89 45 80 48 85 C0";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.setStaticMesh = scanner->Resolve(self, &request);
            if (!addresses.setStaticMesh) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::CaptureOnly";
            request.pattern = "44 0F B6 81 81 02 00 00 41 0F B6 C0 C0 E8 07 3A C2 74 ?? 41 80 E0 7F C0 E2 07 44 0A C2 44 88 81 81 02 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC 38 91 F4 02 00 00 74 ?? 88 91 F4 02 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC CC CC CC 39 91 FC 02 00 00 74 ?? 89 91 FC 02 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC CC CC CC 0F 2E 89 14 03 00 00 74 ?? F3 0F 11 89 14 03 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC F3 0F 10 02 0F 2E 81 00 03 00 00 75 ?? F3 0F 10 42 04 0F 2E 81 04 03 00 00 75 ?? F3 0F 10 42 08 0F 2E 81 08 03 00 00 75 ?? F3 0F 10 42 0C 0F 2E 81 0C 03 00 00 74 ?? 0F 10 02 0F 11 81 00 03 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC 0F 2E 89 10 03 00 00 74 ?? F3 0F 11 89 10 03 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC 0F 2E 89 1C 03 00 00 74 ?? F3 0F 11 89 1C 03 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC 0F 2E 89 F8 02 00 00 74 ?? F3 0F 11 89 F8 02 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC 0F 2E 89 18 03 00 00 74 ?? F3 0F 11 89 18 03 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC 40 53 48 83 EC 30 0F 29 74 24 20 48 8B D9 0F 28 F1 E8 ?? ?? ?? ?? 84 C0 75 ?? F6 83 94 00 00 00 01 74 ?? 38 83 A3 01 00 00 74 ?? 0F 2E B3 90 02 00 00 74 ?? 48 8B 83 A8 00 00 00 F3 0F 11 B3 90 02 00 00 48 85 C0 75 ?? 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 58 02 00 00 48 85 C9 74 ?? 48 8B 01 48 8B D3 FF 90 60 02 00 00 48 8B D3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 74 24 20 48 83 C4 30 5B C3 CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 0F B6 81 7C 02 00 00";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.setVisibleInSceneCaptureOnly = scanner->Resolve(self, &request);
            if (!addresses.setVisibleInSceneCaptureOnly) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::BuilderConstruct";
            request.pattern = "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8B CA 48 8B 02 FF 90 18 04 00 00 33 FF 48 89 43 08 48 89 7B 10 8D 4F 10 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 89 38 48 89 78 08 48 89 43 18 48 8B C3 48 8B 5C 24 30 48 83 C4 20 5F C3 48 89 7B 18 48 8B C3 48 8B 5C 24 30 48 83 C4 20 5F C3 CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 DF FF FF B8 B0 21 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 50 20 00 00 4C 89 45 90 4C 8B EA 48 89 4D E0 48 8B F1 48 85 D2 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 FD 09 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 49 8B 5D 40 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 FD 09 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 03 48 8B CB FF 90 18 04 00 00 48 8B D8 48 85 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 FD 09 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 89 5E 08 48 8D 7E 10 45 33 E4 48 8D 05 ?? ?? ?? ?? 48 89 06 4C 8D 7F 0C 48 8D 4E 20 4C 89 7C 24 70 45 89 27 4C 89 27 44 89 67 08 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 44 88 66 60 4C 8D B6 B0 00 00 00 48 89 06 49 8B CE 44 89 66 64 49 8B D5 4C 89 66 68 4C 89 66 70 4C 89 66 78 4C 89 A6 90 00 00 00 44 89 A6 98 00 00 00 C7 86 9C 00 00 00 80 00 00 00 C7 86 A0 00 00 00 FF FF FF FF 44 89 A6 A4 00 00 00 4C 89 A6 A8 00 00 00 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 49 8D 8E 98 01 00 00 49 89 06 E8 ?? ?? ?? ?? B9 98 02 00 00 45 88 A6 F8 02 00 00 41 C6 86 B8 00 00 00 01";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderGraphBuilderConstruct = scanner->Resolve(self, &request);
            if (!addresses.renderGraphBuilderConstruct) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::BuilderExecute";
            request.pattern = "40 53 48 83 EC 20 48 8B D9 48 8B 49 10 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 C7 43 10 00 00 00 00 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 98 00 00 00 45 33 FF 48 89 4C 24 58 F6 81 B0 00 00 00 01 48 8B D9 74 ?? 48 8D 41 58 48 39 00 74 ?? 48 8B 48 08 EB ?? 49 8B CF 44 39 79 28 74 ?? 48 8B 41 20 EB ?? 48 8D 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 20 41 B8 14 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8D 4D 70 E8 ?? ?? ?? ?? 48 8D 4C 24 31 E8 ?? ?? ?? ?? 48 63 43 70 4C 8B 63 68 0F B6 74 24 30 0F B6 7C 24 30 44 0F B6 74 24 30 48 C1 E0 04 49 03 C4 44 88 7D A0 48 89 44 24 70 4C 89 64 24 40 4C 3B E0 0F 84 ?? ?? ?? ?? 41 0F 10 04 24 66 0F 7E C1 85 C9 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 F9 01 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 38 05 ?? ?? ?? ?? 75 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? E8 ?? ?? ?? ?? 48 8D 8B 98 00 00 00 4C 89 BB 88 00 00 00 33 D2 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 74 ?? 48 8D 05 ?? ?? ?? ?? 4C 89 7D 38 48 89 45 20 4C 8D 05 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 5D 28 48 89 45 10 48 8D 15 ?? ?? ?? ?? 48 8D 45 10 44 0F B6 CF 48 8D 0D ?? ?? ?? ?? 48 89 44 24 20 E8 ?? ?? ?? ?? 48 83 7D 10 00 0F 84 ?? ?? ?? ?? 48 8B 45 38 48 8D 4D 20 48 85 C0 48 0F 45 C8 48 8B 01 FF 50 10 E9 ?? ?? ?? ?? 44 8B 3D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 44 8B 35 ?? ?? ?? ??";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderGraphBuilderExecute = scanner->Resolve(self, &request);
            if (!addresses.renderGraphBuilderExecute) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::BuilderDestroy";
            request.pattern = "48 89 5C 24 10 57 48 83 EC 30 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8B 41 18 48 85 C0 0F 84 ?? ?? ?? ?? 8B 48 08 48 89 74 24 40 85 C9 74 ?? 89 4C 24 20 4C 8D 0D ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B8 FF 03 00 00 48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 73 18 48 85 F6 74 ?? 48 8B 3E 48 85 FF 74 ?? 8B 47 F8 48 83 C7 F8 48 2B F8 B8 FF FF FF FF F0 0F C1 07 83 F8 01 75 ?? 48 8B 47 40 48 2B C7 48 3D 00 00 01 00 75 ?? E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 E8 ?? ?? ?? ?? EB ?? 48 8B CF E8 ?? ?? ?? ?? BA 10 00 00 00 48 8B CE E8 ?? ?? ?? ?? 48 8B 74 24 40 48 C7 43 18 00 00 00 00 48 83 7B 10 00 74 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 08 04 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 7B 10 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? BA B8 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 C7 43 10 00 00 00 00 48 8B 5C 24 48 48 83 C4 30 5F C3 CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B 89 98 00 00 00 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8D 4B 78 E8 ?? ?? ?? ?? 48 8B 7B 68 BE FF FF FF FF 48 85 FF 74 ?? 8B 47 F8 48 83 C7 F8 48 2B F8 8B C6 F0 0F C1 07 83 F8 01 75 ?? 48 8B 47 40 48 2B C7 48 3D 00 00 01 00 75 ?? E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 E8 ?? ?? ?? ?? EB ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B 7B 38 48 85 FF 74 ?? 8B 47 F8 48 83 C7 F8 48 2B F8 8B C6 F0 0F C1 07 83 F8 01 75 ?? 48 8B 47 40 48 2B C7 48 3D 00 00 01 00 75 ?? E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 E8 ?? ?? ?? ?? EB ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B 7B 28 48 85 FF 74 ?? 8B 47 F8 48 83 C7 F8 48 2B F8";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderGraphBuilderDestroy = scanner->Resolve(self, &request);
            if (!addresses.renderGraphBuilderDestroy) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::UpdateCapture";
            request.pattern = "48 8B 02 4C 8B CA 48 8B D1 49 8B C9 48 FF A0 D8 01 00 00 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 8B 02 4C 8B CA 48 8B D1 49 8B C9 48 FF A0 D0 01 00 00 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F9 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8D B7 D8 02 00 00 48 8B CE 48 8D 93 D8 02 00 00 E8 ?? ?? ?? ?? EB ?? 48 8D B7 D8 02 00 00 48 63 87 D0 02 00 00 48 8B 9F C8 02 00 00 48 8D 0C 40 48 8D 3C CB 48 3B DF 74 ?? 66 66 66 0F 1F 84 00 00 00 00 00 48 8B D3 48 8D 4C 24 20 E8 ?? ?? ?? ?? 83 7C 24 28 00 48 8D 0D ?? ?? ?? ?? 0F B6 43 10 48 0F 45 4C 24 20 33 D2 88 44 24 30 E8 ?? ?? ?? ?? 83 F8 FF 74 ?? 44 0F B6 44 24 30 8B D0 48 8B CE E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 83 C3 18 48 3B DF 75 ?? 48 8B 5C 24 50 48 8B 74 24 58 48 83 C4 40 5F C3 CC CC CC CC CC 40 53 48 83 EC 20 80 89 D2 07 00 00 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B D8 07 00 00 48 85 C9 74 ?? 48 8B D3 48 83 C4 20 5B E9 ?? ?? ?? ?? 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC F6 81 18 01 00 00 02 74 ?? F6 81 29 01 00 00 20 75 ?? 48 8B 01 B2 01 48 FF A0 10 04 00 00 E9 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC CC CC CC 48 83 EC 68 48 89 5C 24 70 33 DB 48 89 6C 24 78 48 8B E9 48 89 7C 24 60 48 8B FA 4C 89 7C 24 50 44 8B FB E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B CD 4C 89 74 24 58 E8 ?? ?? ?? ?? 48 8B C8 4C 8B 00 41 FF 90 98 03 00 00 4C 8B F0 48 85 C0 74 ?? 48 89 B4 24 80 00 00 00 48 63 B5 84 07 00 00 85 F6 78 ?? 3B 70 08 7D ?? 48 63 50 08 8B CE";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.updateSceneCapture = scanner->Resolve(self, &request);
            if (!addresses.updateSceneCapture) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::Transition";
            request.pattern = "40 53 56 57 41 54 41 56 41 57 48 83 EC 78 0F 10 02 48 8B DA 48 8B F9 48 8D 54 24 40 0F 29 44 24 40 E8 ?? ?? ?? ?? 48 8B 43 08 0F 10 03 44 0F B6 67 6A 4C 63 F8 49 8B DF 0F 29 44 24 50 48 C1 E3 04 80 7F 25 00 74 ?? 83 7F 20 00 7F ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 37 48 83 C6 07 48 83 E6 F8 48 8D 04 1E 48 3B 47 08 76 ?? 8D 53 08 48 8B CF E8 ?? ?? ?? ?? 48 8B 37 48 83 C6 07 48 83 E6 F8 48 8D 04 1E 48 8B 5C 24 50 4F 8D 3C 7F 49 C1 E7 04 45 33 F6 4C 03 FB 48 89 07 49 3B DF 0F 84 ?? ?? ?? ?? 48 89 AC 24 C0 00 00 00 48 8B EE 4C 89 6C 24 70 41 BD FF FF 00 00 66 44 39 2B 75 ?? 66 44 39 6B 02 75 ?? 66 44 39 6B 04 74 ?? 48 8D 8C 24 B0 00 00 00 C6 84 24 B0 00 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 4B 18 48 8B D5 41 FF C6 48 83 C5 10 89 0A 44 88 62 04 48 89 42 08 48 83 C3 30 49 3B DF 75 ?? 4C 8B 6C 24 70 45 85 F6 0F 8E ?? ?? ?? ?? 80 7F 25 00 48 89 74 24 20 44 89 74 24 28 0F 29 74 24 60 0F 28 74 24 20 74 ?? 83 7F 20 00 7F ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 1F 48 83 C3 07 48 83 E3 F8 48 8D 43 20 48 3B 47 08 76 ?? BA 28 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B 1F 48 83 C3 07 48 83 E3 F8 48 8D 43 20 48 89 07 66 0F 6F C6 48 8B 47 30 FF 47 60 66 0F 73 D8 08 48 89 18 48 8D 43 08 48 89 47 30 66 48 0F 7E C0 85 C0 75 ?? 33 ED 33 F6 E9 ?? ?? ?? ?? 4C 63 F0 49 C1 E6 04 80 7F 25 00 74 ?? 83 7F 20 00 7F ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 8A 00 00 00";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.transitionResources = scanner->Resolve(self, &request);
            if (!addresses.transitionResources) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::GameThread";
            request.pattern = "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 ?? 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 02 74 ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 81 3C 02 02 00 00 40 0F 94 C0 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 8B 04 02 C1 E8 05 24 01 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 83 3D ?? ?? ?? ?? 00 BA D0 09 00 00 75 ?? 65 48 8B 04 25 58 00 00 00 8B 0D ?? ?? ?? ?? 48 8B 0C C8 8B 04 0A 83 F8 10 74 ?? 83 F8 02 74 ?? 3D 10 00 00 40 74 ?? 32 C0 C3 B0 01 C3 8B 05 ?? ?? ?? ?? 65 48 8B 0C 25 58 00 00 00 48 8B 0C C1 8B 04 11 83 E8 10 A9 EF FF FF BF 0F 94 C0 C3 CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 20 0F 94 C0 C3 48 83 EC 28 83 3D ?? ?? ?? ?? 00 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 0C C8 75 ?? 8B 04 0A 83 F8 02 74 ?? 83 F8 10 74 ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 83 3C 0A 10 0F 94 C0 48 83 C4 28 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 04 0F 94 C0 C3 48 8B 49 08 48 8B 01 48 FF A0 B8 00 00 00 CC CC 40 53 48 83 EC 20 48 8B 19 48 85 DB 74 ?? 48 83 BB 90 00 00 00 00 74 ?? FF 15 ?? ?? ?? ?? 48 8B 8B 90 00 00 00 3B 41 34 74 ?? B0 01 48 83 C4 20 5B C3 32 C0 48 83 C4 20 5B C3 CC CC CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.isGameThread = scanner->Resolve(self, &request);
            if (!addresses.isGameThread) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::RetirementConstruct";
            request.pattern = "48 C7 01 00 00 00 00 48 8B C1 C3 CC CC CC CC CC 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 82 42 7D 09 48 8B D9 48 89 01 48 8B 02 48 89 41 08 48 8B 42 08 48 89 41 10 48 85 C0 74 04";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderFenceConstruct = scanner->Resolve(self, &request);
            if (!addresses.renderFenceConstruct) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::RetirementDestroy";
            request.pattern = "48 8B 09 48 85 C9 74 1A B8 FF FF FF FF F0 0F C1 41 08 83 F8 01 75 0B 48 8B 01 BA 01 00 00 00 48 FF 20 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 8D 05 91 07 F3 0B 48 89 01 C3 CC CC CC CC CC";
            request.kind = PLUGIN_SCAN_CODE;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderFenceDestroy = scanner->Resolve(self, &request);
            if (!addresses.renderFenceDestroy) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::RetirementBegin";
            request.pattern = "40 55 53 56 48 8D 6C 24 B9 48 81 EC 00 01 00 00 48 8B 05 39 49 6C 0A 48 33 C4 48 89 45 27 80 3D D3 6D 98 0A 00 8B DA 89 55 9F 48 8B F1 48 89 4D 87 0F 84 3F 08 00 00 E8 14 70 84 FD 84 C0 75 26";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderFenceBegin = scanner->Resolve(self, &request);
            if (!addresses.renderFenceBegin) return false;
        }
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::RetirementPoll";
            request.pattern = "40 53 48 83 EC 30 80 3D 3B 1B 97 0A 00 48 8B D9 0F 84 8A 00 00 00 E8 85 1D 83 FD 84 C0 75 30 FF 15 EB 7B 4D 0A 84 C0 75 26 4C 8D 0D B4 10 3F 07 41 B8 5C 04 00 00 48 8D 15 E3 88 AC 07 48 8D 0D";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.renderFencePoll = scanner->Resolve(self, &request);
            if (!addresses.renderFencePoll) return false;
        }
#if defined(MINIMAP_STAGE_R25B_STATIC_ASSEMBLY)
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::AssemblySetMaterial";
            request.pattern = "85 D2 0F 88 EF 02 00 00 55 56 57 48 83 EC 30 49 8B F0 48 63 EA 4C 8D 81 38 05 00 00 4C 89 74 24 58 48 8B F9 41 3B 28 7D 23 48 8B 81 30 05 00 00 48 8B 0C E8 48 85 C9 75 05 48 85 F6 EB 03 48 3B";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.setMaterial = scanner->Resolve(self, &request);
            if (!addresses.setMaterial) return false;
        }
#endif
#if defined(MINIMAP_STAGE_R25B_STATIC_ASSEMBLY)
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::AssemblySetWorldTransform";
            request.pattern = "40 55 53 56 57 41 56 48 8D 6C 24 C0 48 81 EC 40 01 00 00 48 8B 05 E6 E1 8A 09 48 33 C4 48 89 45 30 48 8B F9 4D 8B F1 48 8B 89 C8 00 00 00 41 0F B6 F0 48 8B DA 48 85 C9 0F 84 DE 00 00 00 48 8B";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.setWorldTransform = scanner->Resolve(self, &request);
            if (!addresses.setWorldTransform) return false;
        }
#endif
#if defined(MINIMAP_STAGE_R25B_STATIC_ASSEMBLY)
        {
            PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
            request.hookName = "MiniMap::AssemblyShowOnlyExec";
            request.pattern = "48 89 5C 24 08 57 48 83 EC 30 48 8B DA 48 C7 44 24 48 00 00 00 00 48 8B F9 E8 92 C9 E1 FC 48 83 7B 20 00 48 8B CB 74 10 48 8B 53 18 4C 8D 44 24 48 E8 EA 23 E6 FC EB 1C 4C 8B 83 88 00 00 00 48";
            request.kind = PLUGIN_SCAN_FUNCTION_START;
            request.flags = PLUGIN_SCAN_FLAG_NONE;
            request.module = nullptr;
            request.resultOffset = 0;
            addresses.showOnlyComponentExec = scanner->Resolve(self, &request);
            if (!addresses.showOnlyComponentExec) return false;
        }
#endif
#endif

        return true;
    }
}
