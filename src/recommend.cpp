#include "recommend.hpp"
#include <set>

namespace mora {
    namespace {
        // splitmix64: a small, well-distributed mixer so the selection is reproducible per seed.
        uint64_t mix(uint64_t x) {
            x += 0x9E3779B97F4A7C15ull;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            return x ^ (x >> 31);
        }
    } // namespace
    std::vector<Candidate> selectCandidates(std::vector<Candidate> pool, size_t limit, uint64_t seed,
                                            int64_t avoid) {
        std::vector<Candidate> result;
        if (!limit)
            return result;
        std::set<int64_t> seen;
        std::vector<std::pair<uint64_t, Candidate>> ranked;
        for (auto& candidate : pool) {
            int64_t id = candidate.track.id;
            if (!id || (avoid && id == avoid))
                continue;
            if (!seen.insert(id).second)
                continue;
            ranked.push_back({mix(seed ^ static_cast<uint64_t>(id)), std::move(candidate)});
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& left, const auto& right) { return left.first < right.first; });
        for (auto& entry : ranked) {
            if (result.size() >= limit)
                break;
            result.push_back(std::move(entry.second));
        }
        return result;
    }
} // namespace mora
