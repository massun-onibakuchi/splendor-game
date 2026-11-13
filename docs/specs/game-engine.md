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
depends on one. The CMake package is `SplendorGame` (target `splendor::game`); it
installs the standard catalog and names it `SplendorGame_CATALOG`. A consumer that adds
this directory instead reads `SPLENDOR_GAME_CATALOG`.

The `splendor-game` Python distribution has no runtime dependencies. `splendor_game`
binds the engine and its clock; [`splendor_play`](play.md) hosts practical matches. A
downstream native extension `cimport`s [`splendor_game.engine`](../../python/splendor_game/engine.pxd),
reads an `Episode`'s or `Clock`'s native object directly and links the one shared library
under `NATIVE_PREFIX`; it MUST NOT embed a second engine. A bridge MUST NOT serialize a
game value or a selected choice into Python objects or JSON just to pass it back into
native code. JSON board, choice and trace rendering is presentation.

Contract identifiers name what consumers persist: the rules (`splendor-game-2p-blind-v1`),
the staged decision surface (`splendor-staged-action-v1`) and the standard catalog
(`splendor-standard-catalog-v1`, with its payload digest). A behavior change to rules or
surface is a new version.

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
trace. It borrows an immutable catalog that MUST outlive it (the Python `Episode` keeps
its `Catalog` alive). `view(seat)` is that seat's knowledge; `world()` is the host's
omniscient state, never an agent input. Views and references to legal choices expire at
selection. `identity()` and `revision()` are process-local mutation identity, not seeds:
a detached root taken from an episode can check that it still applies.

Native and columnar agents select by index into the current legal choices; untrusted or
persisted submissions select by key, checked against the current decision and rejected
before any state change.

[`TraceReplay`](../../include/splendor/replay.hpp) verifies persisted ply, actor, forced
status, legal key and public refill facts against generated traces before rendering.
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
`MatchResult` carries the end reason, seat outcome, board scores/purchases and, for a
timeout, the timed-out seat. The two-player timeout awards the other seat the win,
regardless of prestige. An agent's own time allocation is the agent's; a search that
stops within its allocation is not proof that the match clock has not expired.

## Performance

Engine hot paths are the trusted primitives: they reuse caller buffers, derive choice order
from fields rather than formatted keys, and never revalidate engine-produced decisions.
[`playout_benchmark.cpp`](../../benchmarks/playout_benchmark.cpp) measures uniform playouts
through the staged primitives, the recorded episode and the atomic adapter; its checksum
ties each surface's play to its seed, so a change that alters play is visible before its
timing is compared.

## Player-count readiness

Two-player rules and the public native layouts are explicit. Adding multiplayer requires
a game contract and outcome-law change; consumers rebuild against the matching headers
rather than assume a stable binary ABI.
