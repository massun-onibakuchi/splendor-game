---
title: Game Engine
description: Independent native Splendor execution, legal-choice surfaces, clocks and match adjudication.
date: 2026-11-14
status: active
tags: [specs, game, architecture]
---

# Game engine

## Dependency boundary

`splendor::game` is a C++20 library. It builds, tests and installs without Python and
without any model, search, feature or training code, and nothing in this repository
depends on one. The CMake package is `SplendorGame` (targets `splendor::game` and
`splendor::play`); it installs the standard catalog and names it `SplendorGame_CATALOG`. A
consumer that adds this directory instead reads `SPLENDOR_GAME_CATALOG`.

`splendor::game` holds no JSON and no host. `splendor::play` builds hosts and boards on it:
the [native match host](play.md) and the [presentation renderings](../../include/splendor/play/render.hpp)
of boards, choices and results that Python and WebAssembly hosts share, so neither carries
a second host or renderer.

The `splendor-game` Python distribution has no runtime dependencies. `splendor_game`
binds the engine and its clock; [`splendor_play`](play.md) drives the native match host. A
downstream native extension `cimport`s [`splendor_game.engine`](../../python/splendor_game/engine.pxd),
reads an `Episode`'s or `Clock`'s native object directly and links the one shared library
under `NATIVE_PREFIX`; it MUST NOT embed a second engine. A bridge MUST NOT serialize a
game value or a selected choice into Python objects or JSON just to pass it back into
native code. JSON board, choice and trace rendering is presentation.

Contract identifiers name what consumers persist: the rules of each seat count
(`game_contract(n)`: `splendor-game-2p-v1`, `splendor-game-3p-v1`, `splendor-game-4p-v1`),
the staged decision surface (`splendor-staged-action-v1`) and the standard catalog
(`splendor-standard-catalog-v1`, with its payload digest). A behavior change to a seat
count's rules or to the surface is a new version.

A catalog's identity is the contract name it declares and the SHA-256 of its canonical
payload; the engine loads any catalog of the standard shape, and a consumer pins the one it
plays by digest. The seed law's domain-separation tag (`kSeedContract`) is the build
constant `SPLENDOR_GAME_SEED_NAMESPACE` (default `splendor-seed-derivation-v1`): every
derived seed, setup and draw depends on it, so a consumer that already published seeds
under another tag builds with that tag, and two builds with different tags are different
games for the same seeds.

## Game execution

[Staged actions](staged-actions.md) owns rules and transitions;
[observations](observations.md) owns seat knowledge. Catalog identity, choice order and
the seed law are shared by recorded and unrecorded execution: a recorded game and a
batched producer playing the same seeds and choices reach the same states.

[`TurnEpisode`](../../include/splendor/episode.hpp) is the recorded game: it resolves
chance and forced decisions, exposes the pending legal choices and records a replayable
trace of typed steps (the choice, the visible nobles it names, its draw facts); keys and
renderings derive from a step on demand, so recording costs no formatting and a recorded
game plays as fast as the trusted staged primitives. It borrows an immutable catalog that
MUST outlive it (the Python `Episode` keeps its `Catalog` alive). `view(seat)` is that seat's knowledge; `world()` is the host's
omniscient state, never an agent input. Views and references to legal choices expire at
selection. `identity()` and `revision()` are process-local mutation identity, not seeds:
a detached root taken from an episode can check that it still applies.

Native and columnar agents select by index into the current legal choices; untrusted or
persisted submissions select by key, checked against the current decision and rejected
before any state change.

[`TraceReplay`](../../include/splendor/replay.hpp) verifies persisted ply, actor, forced
status, legal key and public refill facts (`RecordedStep`) against generated traces before
rendering.
Completion requires exactly the recorded steps. Choice descriptions, indexes and text are
regenerated, not replay identity; a blind reservation's tier is already bound by its key.

The `splendor/detail/` headers are the trusted, buffer-reusing primitives of batched
producers and search. Their inputs are engine-produced decisions and their own legal
choices: they do not repeat full validation, format keys or JSON, and callers MUST NOT
hand them reconstructed or unchecked choices. Public `decision_choices` and
`validate_*` validate supplied input.

## Granularity-neutral choices

A decision's legal choices are its leaves. Any staged policy is a law over those leaves:
the engine never adds a decision for a consumer's staging. Two surfaces expose a
decision's structure without parsing keys:

- the [action tree](staged-actions.md#action-tree) (`action_tree.hpp`): the engine's own
  family, intent and parameter grouping;
- [`ChoiceColumns`](../../include/splendor/choice_columns.hpp) (`Episode.choice_table()` in
  Python): per-leaf columns in choice order — phase, tree family, intent and null take,
  primary family, net token delta, payment, tier, market and reserve slots, target card,
  eligible nobles, cleanup return and noble — absent fields marked by a fixed sentinel.

A consumer stages or groups leaves at its own granularity (one gem at a time, card then
payment, return then noble, …) by filtering these columns, then selects the remaining
leaf by index. Columns are typed buffers: no Python object per choice.

A consumer that plans whole turns composes the staged transitions: `apply_main` exposes a
choice's draw outcomes and cleanup support before any draw, `resolve_main` places a draw
and `apply_cleanup` completes the turn. No surface fixes a turn's cleanup before its
draw: that cleanup belongs to a decision taken after the draw, with what it reveals.

## Match clock and result

[`GameClock`](../../include/splendor/clock.hpp) owns elapsed time and per-seat timeout
policy. A turn is one actor at one game ply: MAIN and CLEANUP share its allowance.
`run`/`stop` delimit charged segments; pauses and pondering charge no match time. Hosts
supply monotonic milliseconds, not timestamps received from agents. Starting another ply
charges the completed turn once; `record` restores cumulative turn use while stopped, so
restoring MAIN then CLEANUP does not charge MAIN twice.

A bank has no increment and always forfeits at expiry. A per-turn allowance resets every
turn and either flags overtime or forfeits according to its seat's policy.
[`adjudicate_timeout`](../../include/splendor/match.hpp) reads the pending board and its
clock without mutating either. Hosts adjudicate before accepting a move and freeze the
clock when play ends. An already finished `TurnEpisode` cannot become a timeout.
`MatchResult` carries the end reason, the winners as a seat set, board scores/purchases
and, for a timeout, the timed-out seat. A timeout loses for the timed-out seat alone: every
other seat shares the win, regardless of prestige (with two seats, the other seat wins).
`match_utilities` and `terminal_utilities` read a result's winners under the
[outcome law](staged-actions.md#game-rules). An agent's own time allocation is the agent's;
a search that stops within its allocation is not proof that the match clock has not
expired.

## Performance

Engine hot paths are the trusted primitives: they reuse caller buffers, derive choice order
from fields rather than formatted keys, keep set-valued fields (eligible nobles, winners)
as bitmasks, and never revalidate engine-produced decisions.
[`playout_benchmark.cpp`](../../benchmarks/playout_benchmark.cpp) measures uniform playouts
at any seat count through the staged primitives and the recorded episode; its checksum
ties each surface's play to its seed, so a change that alters play is visible before its
timing is compared.

## Seat counts

A state carries its seat count (`State::player_count`, two to four); seat-indexed arrays
hold `kMaxPlayers` entries of which the first `player_count` are seated (`seated(state)`),
so one native layout and one binary interface serve every seat count. Seat arithmetic reads
the state's count: cyclic order (`next_seat`), round closure and equal-turn termination.
Consumers that support fewer seat counts (a model trained for two seats) reject other
states at their own boundary instead of assuming the engine's.
