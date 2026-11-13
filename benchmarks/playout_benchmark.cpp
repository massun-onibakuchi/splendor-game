// Uniform-random playouts through the engine's three execution surfaces:
//
//   staged    trusted staged primitives (`detail::play_forced`, `detail::play_choice`)
//   recorded  the recorded `TurnEpisode` a match host drives, its trace drained per choice
//   atomic    the atomic adapter (`generate_candidates`, `detail::play_decision`)
//
// Usage: splendor_game_benchmark [games] [seed]. The checksum ties each surface's play to
// its seed, so a change that alters play shows here before it shows in timings.

#include "splendor/candidate.hpp"
#include "splendor/catalog.hpp"
#include "splendor/episode.hpp"
#include "splendor/seed.hpp"
#include "splendor/detail/episode.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace splendor;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kMaxGamePlies = 160;

struct Totals {
    std::uint64_t decisions{};
    std::uint64_t checksum{};
    double seconds{};
};

std::uint64_t pick(const std::uint64_t seed, const std::size_t count) {
    return derive64(seed, "benchmark-pick") % count;
}

std::uint64_t score(const TerminalFacts& terminal) {
    return terminal.final_prestige[0] * 31U + terminal.final_prestige[1];
}

Totals staged(const Catalog& catalog, const std::uint64_t games, const std::uint64_t seed) {
    Totals totals;
    std::vector<TurnChoice> choices;
    const auto start = Clock::now();
    for (std::uint64_t game = 0; game < games; ++game) {
        const std::array<std::uint64_t, 1> coordinate{game};
        const detail::EpisodeSeeds seeds(derive64(seed, "benchmark-episode", coordinate));
        Decision decision{initial_state(catalog, kMaxGamePlies, derive64(seed, "benchmark-setup", coordinate)),
                          DecisionPhase::Main};
        for (;;) {
            std::optional<TerminalFacts> terminal =
                detail::play_forced(decision, choices, seeds, kMaxGamePlies, catalog);
            if (!terminal) {
                const TurnChoice& chosen = choices[pick(seeds.decision(decision), choices.size())];
                terminal = detail::play_choice(decision, chosen, seeds, kMaxGamePlies, catalog);
                ++totals.decisions;
            }
            if (terminal) {
                totals.checksum += score(*terminal);
                break;
            }
        }
    }
    totals.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return totals;
}

Totals recorded(const Catalog& catalog, const std::uint64_t games, const std::uint64_t seed) {
    Totals totals;
    const auto start = Clock::now();
    for (std::uint64_t game = 0; game < games; ++game) {
        const std::array<std::uint64_t, 1> coordinate{game};
        TurnEpisode episode(
            catalog, kMaxGamePlies, derive64(seed, "benchmark-setup", coordinate),
            derive64(seed, "benchmark-episode", coordinate));
        while (!episode.is_terminal()) {
            episode.select(pick(episode.decision_seed(), episode.legal_choices().size()));
            episode.drain_trace();
            ++totals.decisions;
        }
        const TurnTerminal terminal = episode.terminal();
        totals.checksum += static_cast<std::uint64_t>(terminal.final_prestige[0]) * 31U
            + static_cast<std::uint64_t>(terminal.final_prestige[1]);
    }
    totals.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return totals;
}

Totals atomic(const Catalog& catalog, const std::uint64_t games, const std::uint64_t seed) {
    Totals totals;
    const auto start = Clock::now();
    for (std::uint64_t game = 0; game < games; ++game) {
        const std::array<std::uint64_t, 1> coordinate{game};
        const detail::EpisodeSeeds seeds(derive64(seed, "benchmark-episode", coordinate));
        State state = initial_state(catalog, kMaxGamePlies, derive64(seed, "benchmark-setup", coordinate));
        std::uint32_t ordinal = 0;
        for (;;) {
            const CandidateBatch leaves = generate_candidates(state, kMaxGamePlies, catalog);
            const CandidateDescriptor& chosen = leaves.rows[pick(seeds.decision(state), leaves.rows.size())];
            const std::uint64_t draw = seeds.draw(state);
            ++totals.decisions;
            if (const std::optional<TerminalFacts> terminal = detail::play_decision(
                    state, ordinal, chosen, draw, kMaxGamePlies, catalog)) {
                totals.checksum += score(*terminal);
                break;
            }
        }
    }
    totals.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return totals;
}

void report(const char* name, const std::uint64_t games, const Totals& totals) {
    std::printf(
        "%-8s games=%llu decisions=%llu seconds=%.3f decisions/s=%.0f games/s=%.1f checksum=%llu\n",
        name, static_cast<unsigned long long>(games),
        static_cast<unsigned long long>(totals.decisions), totals.seconds,
        static_cast<double>(totals.decisions) / totals.seconds,
        static_cast<double>(games) / totals.seconds,
        static_cast<unsigned long long>(totals.checksum));
}

}  // namespace

int main(const int argc, char** argv) {
    const std::uint64_t games = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000;
    const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    const Catalog catalog = Catalog::load(SPLENDOR_GAME_CATALOG);
    report("staged", games, staged(catalog, games, seed));
    report("recorded", games, recorded(catalog, games, seed));
    report("atomic", games, atomic(catalog, games, seed));
    return 0;
}
