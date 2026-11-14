// Uniform-random playouts through the engine's two execution surfaces:
//
//   staged    trusted staged primitives (`detail::play_forced`, `detail::play_choice`)
//   recorded  the recorded `TurnEpisode` a match host drives, its trace drained per choice
//
// Usage: splendor_game_benchmark [games] [seed] [players]. The checksum ties each surface's
// play to its seed, so a change that alters play shows here before it shows in timings.

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
    std::uint64_t value = 0;
    for (std::size_t seat = 0; seat < terminal.player_count; ++seat) {
        value = value * 31U + terminal.final_prestige[seat];
    }
    return value;
}

Totals staged(
    const Catalog& catalog, const std::uint8_t players, const std::uint64_t games,
    const std::uint64_t seed) {
    Totals totals;
    std::vector<TurnChoice> choices;
    const auto start = Clock::now();
    for (std::uint64_t game = 0; game < games; ++game) {
        const std::array<std::uint64_t, 1> coordinate{game};
        const detail::EpisodeSeeds seeds(derive64(seed, "benchmark-episode", coordinate));
        Decision decision{initial_state(catalog, players, kMaxGamePlies, derive64(seed, "benchmark-setup", coordinate)),
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

Totals recorded(
    const Catalog& catalog, const std::uint8_t players, const std::uint64_t games,
    const std::uint64_t seed) {
    Totals totals;
    const auto start = Clock::now();
    for (std::uint64_t game = 0; game < games; ++game) {
        const std::array<std::uint64_t, 1> coordinate{game};
        TurnEpisode episode(
            catalog, players, kMaxGamePlies, derive64(seed, "benchmark-setup", coordinate),
            derive64(seed, "benchmark-episode", coordinate));
        while (!episode.is_terminal()) {
            episode.select(pick(episode.decision_seed(), episode.legal_choices().size()));
            episode.drain_trace();
            ++totals.decisions;
        }
        totals.checksum += score(*episode.terminal());
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
    const auto players =
        static_cast<std::uint8_t>(argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 2);
    const Catalog catalog = Catalog::load(SPLENDOR_GAME_CATALOG);
    report("staged", games, staged(catalog, players, games, seed));
    report("recorded", games, recorded(catalog, players, games, seed));
    return 0;
}
