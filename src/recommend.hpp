#pragma once
#include "core.hpp"

namespace mora {
    // One real candidate plus the reason it was proposed. Reasons are data for the host model, not
    // instructions, and they always point back to a real search hit or a real playlist.
    struct Candidate {
        Track track;
        std::wstring reason;
    };

    // Deterministic, bounded pick over a real candidate pool: the same seed yields the same order,
    // `avoid` (the track that is already playing) is never returned, duplicates collapse and the
    // result never exceeds `limit`.
    std::vector<Candidate> selectCandidates(std::vector<Candidate> pool, size_t limit, uint64_t seed,
                                            int64_t avoid = 0);
} // namespace mora
