// Owns one destination-keyed native preload entry without exposing genuine UE containers to the SDK.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <memory>

namespace Services::Map::Terrain::Capture
{
    /** Reserves an absent native package key as GamePreview until the associated load has retired.
     * Game-thread only. The root operation must release explicitly after all delegate copies retire.
     */
    class PreloadWorldTypeAssociation final
    {
    public:
        /** Creates an inert owner with no native container access. */
        PreloadWorldTypeAssociation();
        /** Refuses destruction while the native association is still owned. */
        ~PreloadWorldTypeAssociation();
        /** Keeps association ownership unique and stationary. */
        PreloadWorldTypeAssociation(const PreloadWorldTypeAssociation&) = delete;
        /** Prevents overwriting an active association. */
        PreloadWorldTypeAssociation& operator=(const PreloadWorldTypeAssociation&) = delete;
        /** Inserts only an absent, non-None destination identity; never overwrites another preload entry. */
        bool Acquire(std::int32_t comparisonIndex, std::uint32_t number);
        /** Removes only this owner's unchanged entry. Caller first establishes native load retirement. */
        bool Release();
    private:
        /** Keeps the genuine key and container access in the UE-only implementation. */
        struct State;
        std::unique_ptr<State> state_;
    };
}
#endif
