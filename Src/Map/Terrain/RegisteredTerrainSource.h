// Game-thread selection, embedded FE floor resources, and native brush re-resolution.
#pragma once
#include "RegisteredTerrainImage.h"
#include "DiagnosticExports.h"
#if defined(MODLOADER_CLIENT_BUILD)
namespace SDK { struct FVector; }
namespace MiniMapTerrainSource
{
    struct SourceTile;
    /** Owns one optional registration and reusable immutable embedded FE image owners.
     * Native texture pointers exist only in synchronous resolution results.
     */
    class RegisteredTerrainSource
    {
    public:
        /** Returns an owned registration snapshot, or no override. Game-thread only. */
        static std::optional<RegisteredTerrainImage> Snapshot();
        /** Returns terrain-local selection revision; unrelated to content/world generations. */
        static uint64_t Revision();
        /** Clears a selected override and immediately withdraws its presentation without releasing receipts. */
        static void Clear();
        /** Drops registrations belonging to a replaced world; returns whether selection changed. */
        static bool SynchronizeWorld();
        /** Resolves the selected image at a covered position, otherwise the unchanged game path. */
        static bool Resolve(const SDK::FVector& position, SourceTile& tile);
        /** Re-resolves a registered game brush and checks its recorded grid/radiation/variant.
         * The returned texture is borrowed only for synchronous acquisition.
         */
        static bool ResolveTexture(const RegisteredTerrainImage& image, SourceTile& tile);
        /** Selects a validated registration for the current gameplay world and immediately withdraws old presentation.
         * Re-selecting the same descriptor leaves presentation and revision unchanged.
         */
        static bool Select(const RegisteredTerrainImage& image);
    private:
        /** Returns a lazily decoded shared image and permanently remembers invalid resources. */
        static bool LoadForgottenEngineImage(int floorIndex, RegisteredTerrainImage& image);
        /** Reads one compiled RCDATA PNG and constructs its validated registered image. */
        static bool DecodeForgottenEngineResource(int floorIndex, RegisteredTerrainImage& image);
        /** Decodes one embedded PNG and attaches its immutable owner and exact world registration. */
        static bool DecodeForgottenEnginePng(int floorIndex, const uint8_t* pngBytes,
            uint32_t pngByteCount, RegisteredTerrainImage& image);
        /** Latches and logs one failed embedded resource candidate. */
        static void FailForgottenEngineResource(int floorIndex, const char* reason);
    };
}
#endif
