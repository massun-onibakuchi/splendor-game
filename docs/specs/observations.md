---
title: Observations and Seat Knowledge
description: One state type with seat knowledge, the observation map, the view boundary, and noninterference across hidden worlds.
date: 2026-11-14
status: active
tags: [specs, game, data]
---

# Observations and seat knowledge

## Scope

This contract owns seat knowledge and noninterference.
[state.hpp](../../include/splendor/state.hpp) owns `State`, observation and
validation; [turn.hpp](../../include/splendor/turn.hpp) owns `PlayerView`.
Blind-reserved identities are private to their owner until purchase.

## One state, knowledge in null identities

- A `State` is a complete world or any seat's knowledge of one; there is no observer
  field. A reserve is `{tier, card_id?, concealed}`: `concealed` says the identity is the
  owner's alone, a null `card_id` says this state does not know it. The `unknown_card_ids`
  set lists every card whose place this state does not know: the deck in a complete world,
  plus every concealed card of other seats in a seat's view.
- Invariant: every catalog card is in exactly one place (market, a known reserve, a
  purchase count, or the unknown set); deck counts are derived in one pass per state
  (`tier_counts(state).deck` = unknown of the tier − concealed-and-unknown reserves of the
  tier).
  `omniscient(state)` holds when no identity is null; `knows_own_reserves(state, seat)`
  holds when every reserve of `seat` carries its identity.
- The environment (episodes and the hosts and producers that run them) owns complete
  worlds and draws from them with the episode's own seed, never an agent's
  ([staged actions](staged-actions.md#chance-and-knowledge)). Supplied starts are omniscient.

## Observation

`observe(state, seat)` is idempotent and monotone in knowledge: it keeps every identity
the seat may know and nulls the identity of every other seat's concealed reserve, adding
that card to the unknown set in ascending order. A seat's own view is a valid `State`
under `validate_state`; the actor of a decision always knows its own reserves. Observing
a state that lacks knowledge the observer holds is an error, never a guess.

`PlayerView` is the only form an agent-facing boundary accepts (a consumer's features,
evaluator inputs, search roots, policies and records), and only `observe(Decision, seat)`,
`observe_actor(decision)` and `reveal(decision)` construct it. A `PlayerView` whose seat
is not the acting seat is still that seat's knowledge of the decision; consumers that act
must use `observe_actor`.

`reveal(decision)` is the actor's view of an omniscient decision with every seat's
concealed identity kept; a decision that has lost any identity cannot be revealed. It is
a training-time input only -- a critic or label that may condition on the present hidden
state -- and nothing that informs a seat's play takes it: policies, evaluators, search
roots, records and boards see only `observe` views. It reveals the present, never the
deck order or a future draw: those are chance, not hidden state.

A view borrows its decision when no projection is needed; projected views and views
from temporaries own it. Holders must preserve the decision's lifetime. Knowledge-only
checks use `observed_by` without constructing a projection.

## Noninterference

Worlds differing only in identities a seat may not know produce identical observations,
legal choices and keys for that seat, and an agent given only that seat's views cannot
tell them apart. Consumers keep the property for everything they derive from a view
(features, search results, records); a report made from one seat's knowledge is that
seat's, and [play](play.md#hidden-cards) owns its disclosure to a human. Observation cannot
recover a world: consumers that need one sample it.

## Player-count readiness

Observation takes an observer seat and every reserve an owner; concealed cards of several
other seats are allocated jointly from one unknown set without replacement. Nothing here
assumes two seats.
