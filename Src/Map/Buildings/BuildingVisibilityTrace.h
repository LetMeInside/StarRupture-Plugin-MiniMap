#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace MiniMapBuildingVisibilityTrace
{
    enum class Stage : unsigned { Collector, Prepared, Render };
    enum class State : unsigned
    {
        Unknown, Present, Absent, CanonicalUnavailable, SessionUnavailable,
        AtlasMissing, QuadRejected, Culled, SubmitRejected, Submitted
    };
    inline const char* Name(State state) noexcept
    {
        constexpr const char* names[]{"unknown", "present", "absent", "canonical-unavailable", "session-unavailable",
            "atlas-missing", "quad-rejected", "culled", "submit-rejected", "draw-submitted"};
        return names[unsigned(state)];
    }
    struct Sample
    {
        int32_t Index{}, Serial{}, Definition{};
        uint32_t Building{};
        std::array<State, 3> Last{};
    };
    // Diagnostic identities only; never retains collector records, native objects,
    // paths or draw state. Owner serializes access. Bounded per world generation.
    struct Trace
    {
        static constexpr std::size_t MaximumSamples = 16, MaximumEvents = 128;
        uint64_t Generation{};
        std::size_t Count{}, Events{};
        std::array<Sample, MaximumSamples> Samples{};
        void Reset(uint64_t generation) noexcept { *this = {}; Generation = generation; }
        void Consider(int32_t index, int32_t serial, int32_t definition, uint32_t building) noexcept
        {
            if (Count == MaximumSamples) return;
            for (std::size_t i = 0; i < Count; ++i)
                if (Samples[i].Definition == definition && Samples[i].Building == building) return;
            Samples[Count++] = {index, serial, definition, building, {}};
        }
        bool Observe(std::size_t index, Stage stage, State state) noexcept
        {
            if (index >= Count || Events >= MaximumEvents) return false;
            auto& previous = Samples[index].Last[unsigned(stage)];
            if (previous == state) return false;
            previous = state; ++Events; return true;
        }
    };
}
