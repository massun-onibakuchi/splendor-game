---
title: Practical Play
description: One native match host for seats of any kind, its agent schedule, clocks, log, archive, restart and loopback server.
date: 2026-11-14
status: active
tags: [specs, game, play]
---

# Practical play

## Scope

[`splendor::play::Match`](../../include/splendor/play/match.hpp) hosts one match between seats
of any kind: humans, who submit validated keys, and agents, whose searches the host runs.
It owns turn order, clocks, submissions, pausing, the agents' job schedule, analysis
visibility, the match log, its replay and archive listings, and renders every board the
same way for every host language. Python
([`splendor_play`](../../python/splendor_play/__init__.py)) and a WebAssembly build drive the
same native host; each supplies only wall and monotonic time, runs scheduled searches on
its agents, persists logs and serves the board. No host keeps match logic of its own.

`splendor_play` depends only on `splendor_game` and the standard library. A composer
supplies the offered actors, their agents, the play conditions and the board's static
files; the host does not know how an agent searches or what model it runs.

## Seats and agents

A match has two to four seats; seat 0 starts. The board's request names each seat:
`{kind: "human"}` or `{kind: "agent", actor, ponder}`, where `actor` is an offered actor's
name. The host resolves it to the actor's stable `identity` (game IDs carry it; archives
group results by it) and logged summary. Humans are named by seat order when several play.

[agents.py](../../python/splendor_play/agents.py) owns the agent protocol. An agent warms
its session before any clock runs, detaches a root from a read-only view of the episode a
job searches, builds the timer that ends its move searches from a read-only view of the
running clock, searches in slices that publish JSON reports, and names its retained move as
an index into the live decision's legal choices, rejecting a decision its root was not taken
from. Reports are display and evidence only, never commands.

The native host schedules one job at a time and the host language runs it:

- **Move:** the acting seat is an agent.
- **Ponder:** a human decides and an agent seat pondering is next in turn order; a human's
  commit records the latest pondered report with the move.
- **Review:** a finished game's reviewed position, searched by its actor when that is an
  agent, else by the first agent seat. Review is display-only, never logged or archived.

Only the current job may publish or select; a stale job's work is discarded. Pause stops a
search and discards its selection; agents keep whatever trees they retain. The host
adjudicates time before accepting any selection. Human submissions name the decision's
revision and are validated keys; agents' selections are indexes of the current job's
decision. Persisted keys remain validated inputs.

## Clocks

The engine's [match clock and adjudicator](game-engine.md#match-clock-and-result) keep
time. Settings map onto native per-seat policies:

| Time control | Allowance | Out of time |
| --- | --- | --- |
| Per turn | `human_seconds` for human seats, `agent_seconds` for agent seats, fresh each turn | Human: flagged or `timeout` loss, as chosen; agent: nothing (its timer ends its search) |
| Bank | One bank per seat for the game, no increment | Any seat: `timeout` loss |

- Pondering spends no bank. A timeout loses for the timed-out seat alone; every other seat
  shares the win.
- Closing or replacing a match adjudicates expiry at its stopped clock before archiving.
  Restart never refunds the interrupted turn's archived time.

## Perspective and hidden cards

Every state, export and history snapshot is rendered for one seat's perspective: its
observation of the board, never the world. The board chooses the perspective (by default
the first human seat, else seat 0); several humans sharing one board each act from their
own perspective. Legal choices are offered only to the acting human's perspective.

- A report made by searching a decision whose actor held a card another seat could not
  identify is that actor's knowledge: until the game is finished, every other perspective
  sees only `{kind, seat, concealed: true}`, live and historical alike. Think time is the
  accepted side channel. Completion reveals full reports; archives always retain them.
- Analysis is hidden until completion unless enabled: hidden live searches, reports and
  pondering are absent from state and exports, not merely visually concealed. Enabled
  analysis marks a game with a human seat assisted.

## Match

User uint64 seed `s` derives setup/episode through `match-setup`/`match-episode`.
Submissions name the current revision and a legal key; stale or illegal requests have no
effect. Composition and undo stay client-local. Paused/finished review steps through
snapshots, never rewinding the live game. The log numbers each chosen move by the review
position it leads to (forced steps unnumbered), position 0 being the setup.

## Log `splendor-match-log-v1`

JSON is authoritative; text is derived. The native host owns the schema; a composer
persists its logs under its own contract name for it (`MatchHost::log_contract`), which
restore and archive listings require. A log retains rules/catalog/safety limit, game and
seat identities, UTC start, setup/episode seeds, the composer's logged conditions, time
control and analysis conditions, pauses, ordered steps and result (`{reason, winners,
final_prestige, purchased_cards, timed_out_seat}`). Each step binds ply, actor, phase,
legal key, forced status, choice and draw trace; chosen steps add their turn's elapsed
time, overtime and any search/ponder report (with its searching `seat`, the root `actor`
and `concealed`). `pending_elapsed_ms` is the pending turn's cumulative use, null when the
game is terminal.

Replay from setup/episode seeds and non-forced keys regenerates forced steps and the final
board. Native `TraceReplay` verifies persisted game facts as they are regenerated; then
clock and report evidence is adopted. Choice descriptions, text, phase and history fields
are regenerated; editing presentation cannot invalidate the game. Logs are operator
evidence.

## Archive and restart

[`Archive`](../../python/splendor_play/archive.py) rewrites the full, unredacted log
atomically at start and after each decision, pause, resume, finish or close, with board and
clock aligned to the same pending decision. Never-started matches are absent. Listings come
from the native summaries: each agent identity's record against human seats separates
ordinary human/agent wins and draws from other finished and unfinished games; games
without a human seat are listed but not counted.

Restart requires this log contract, the host's ply cap and offered actors for every logged
agent identity. It replays non-forced keys and requires exactly the logged steps (every
field but clocks and reports) before adopting logged clocks and reports. A logged result
must equal the replayed terminal result, or a `timeout` loss at the pending decision of a
seat that can lose on time. An archived log is read only after the replaced match has
closed. Unfinished matches restart paused, rebuilding completed turns from logged decisions
and adopting the pending turn's use without charging its MAIN twice or letting it precede
its last logged decision. Finished matches reopen for review. Mutations and replacement are
serialized; the old search is closed and joined before its successor is created.

## Server

[server.py](../../python/splendor_play/server.py) is a loopback-only IPv4 service for the
composer's board directory and the JSON API (`seat` selects a state's or export's
perspective). It serves only that directory's own files of known types, `index.html` at
`/`. It rejects a foreign Host, a present foreign Origin and non-JSON mutations: binding to
localhost alone does not stop cross-origin mutation.

## Seat counts

Boards, logs, clocks, schedules and archives take their size from the seat list; nothing
assumes two seats or one human.
