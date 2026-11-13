---
title: Practical Play
description: One human-versus-agent match host, its agent contract, clocks, log, archive, restart and loopback server.
date: 2026-11-14
status: active
tags: [specs, game, play]
---

# Practical play

## Scope

[`splendor_play`](../../python/splendor_play/__init__.py) hosts one human-versus-agent
match on the [engine](game-engine.md) and its native clock. It owns turn order, clocks,
submissions, pausing, analysis visibility, the match log, archive, restart and a loopback
board server. It depends only on `splendor_game` and the standard library. A composer
supplies the setup, the agents and the board's static files; the host does not know how an
agent searches or what model it runs.

## Agent contract

[agents.py](../../python/splendor_play/agents.py) owns the protocols.

- A `MatchSetup` offers actors (`PlayerRef`: label, stable `identity`, logged `summary`)
  and its play conditions (`max_game_plies`, and the opaque `search` and `leaf_batch`
  values logged verbatim). `logged_model` resolves a log's actor summary to an offered
  actor or rejects it.
- An `Agent` warms its session before any clock runs, detaches a root from the live
  episode, builds the timer that ends its move searches from the running clock, searches a
  root in slices that publish JSON reports, and applies its retained selection to the live
  episode. It rejects a decision its root was not taken from.
- Reports are display and evidence only, never commands. The host marks each report made
  while the agent held a card the human cannot identify (`concealed`).

Only one search runs at a time: each search waits for its predecessor, and only the
current job may publish or play. A human commit stops pondering; pause stops a search and
discards its selection; the agent keeps whatever it retains for resume. The host
adjudicates time and discards stale jobs before asking the agent to apply its selection.
Human submissions and persisted keys remain validated inputs.

Finished-game review may analyze a reviewed position with a fresh search, stopped by a
position change, replacement or the agent's own limit. Review analysis is display-only,
never logged or archived, and cannot run during a live game.

## Clocks

The engine's [match clock and adjudicator](game-engine.md#match-clock-and-result) keep
time. Settings map onto native policies:

| Time control | Allowance | Out of time |
| --- | --- | --- |
| Per turn | Fresh human and model allowances each turn | Human: decision flagged or `timeout` loss, as chosen; model: nothing |
| Bank | One bank per player for the game, no increment | Either side: `timeout` loss |

- Pondering spends no bank.
- Closing or replacing a match adjudicates expiry at its stopped clock before archiving.
  Restart never refunds the interrupted turn's archived time.

## Match

User uint64 seed `s` derives setup/episode through `versus-setup`/`versus-episode`.
Seat 0 starts; the human chooses first/second.

Submissions name the current revision and a legal key; stale or illegal requests have no
effect. Composition and undo stay client-local. Paused/finished review steps through
snapshots, never rewinding the live game. The log numbers each chosen move by the review
position it leads to (forced steps unnumbered), position 0 being the setup.

Analysis is hidden until completion unless enabled: hidden evaluation, live search and
pondering are absent from state and exports, not merely visually concealed. Enabled
analysis marks assisted play. Assisted games and timeout losses are not ordinary results.

## Hidden cards

- Boards use the human seat's observation. Concealed opponent reserves have null identity
  and visible tier; the human sees their own draws. Blind-draw traces name only the tier.
- While unfinished, a model-root report made while the model held an identity the human
  could not know is withheld: every player-facing state and export shows only
  `{concealed: true}`, live and historical alike. Human-root pondering is not withheld.
  Think time is the accepted side channel.
- Completion reveals full reports; the board remains the human's view. Archives always
  retain full reports.

## Log `splendor-versus-log-v1`

JSON is authoritative; text is derived. [match.py](../../python/splendor_play/match.py)
owns the schema (`VERSUS_LOG_SCHEMA`); a composer persists its logs under its own contract
name for it (`MatchSetup.log_contract`), which restore and archive listings require. It retains rules/catalog/safety limit, game and actor identity, UTC start,
setup/episode seeds, seats, the setup's logged search and leaf batch, time control and
analysis conditions, pauses, ordered steps and result. Each step binds ply, actor, phase,
legal key, forced status, choice and draw trace; chosen steps add their turn's elapsed
time, overtime and any search/ponder report. `pending_elapsed_ms` is the pending turn's
cumulative use, null when the game is terminal.

Replay from setup/episode seeds and non-forced keys regenerates forced steps and the final
board. Native `TraceReplay` verifies persisted game facts first; then clock and report
evidence is adopted. Choice descriptions, text, phase and history fields are regenerated;
editing presentation cannot invalidate the game. Logs are operator evidence.

## Archive and restart

[`Archive`](../../python/splendor_play/archive.py) rewrites the full, unredacted log
atomically at start and after each decision, pause, resume, finish or close, with board and
clock aligned to the same pending decision. Never-started matches are absent. Listings
group results by the composer's actor identity, separating ordinary human/model wins and
draws from other finished and unfinished games.

Restart requires this log contract, the server's ply cap and an offered actor. It replays
non-forced keys and requires exactly the logged steps (every field but clocks and reports)
before adopting logged clocks and reports. A logged result must equal the replayed terminal
result, or a `timeout` loss at the pending decision of a seat that can lose on time. An
archived log is read only after the replaced match has closed. Unfinished matches restart
paused, rebuilding completed turns from logged decisions and adopting the pending turn's
use without charging its MAIN twice or letting it precede its last logged decision.
Finished matches reopen for review. Mutations and replacement are serialized; the old
search is closed and joined before its successor is created.

## Server

[server.py](../../python/splendor_play/server.py) is a loopback-only IPv4 service for the
composer's board directory and the JSON API. It serves only that directory's own files of
known types, `index.html` at `/`. It rejects a foreign Host, a present foreign Origin and
non-JSON mutations: binding to localhost alone does not stop cross-origin mutation.

## Extension readiness

Board panels, log seats and per-seat snapshots take their size from the native board; the
estimate's mover is the current player. First/second seat choice and a timeout's win for
one opponent are two-player rules, not container assumptions.
