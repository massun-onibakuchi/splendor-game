---
title: Staged Actions
description: Turn phases, legal choices, chance, keys, and the atomic adapter.
date: 2026-11-14
status: active
tags:
  - specs
  - game
---

# Staged actions

## Scope

`splendor-staged-action-v1` defines decisions for the games of every seat count
(`game_contract(n)` for two to four seats). Games start from ordinary setup or a
validated supplied MAIN state.
[turn.hpp](../../include/splendor/turn.hpp) owns the executable boundary;
[observations](observations.md) owns seat knowledge.

## Game rules

Colors are `[white, blue, green, red, black, gold]`; costs/bonuses omit gold.
The supply is four of each gem color with two seats, five with three and seven with four,
and five gold; setup reveals one noble more than the seats. One setup seed deals the same
decks and noble order at every seat count. The game fixes:

- A take is three tokens of different colors (one of each available non-gold color when
  fewer than three are available, never fewer), or two of one color from a pile of at
  least four. Held tokens never block a take: the excess over ten is returned.
- A visible reserve needs an occupied slot; a blind reserve needs a tier whose deck
  holds a card; both need fewer than three reserves. Gold acquisition and effective
  equivalence follow [action canonicalization](action-canonicalization.md).
- A blind reservation draws the top card of the named tier's deck into the actor's
  reserve face down: its identity is the owner's alone until it is bought.
- All legal payments after bonuses are choices; gold may pay for any color, even one the
  payer holds. Overflow returns reach exactly ten tokens. Noble choice and draw placement
  follow the staged turn below.
- Reaching 15 prestige triggers the final round. Finish when every seat has had equal
  turns, immediately before returning to the starting player. The winners are the seats of
  greatest prestige and, among them, fewest purchased cards; seats still equal share the
  win (with two seats, a draw).
- `max_game_plies` is an explicit positive uint32 safety limit counting completed
  turns from zero, not the objective. Equal-turn target termination wins precedence
  over a simultaneous cap; cap-only termination has no winner.
- Outcome law: a seat's utility is `u_i = (N r_i - 1) / (N - 1)` for its win share
  `r_i = 1/m` of the `m` winners among `N` seats (`seat_utilities`). A sole winner scores 1
  and every other seat `-1/(N-1)`; no winner is an N-way tie worth 0 to every seat.

## Turn

```text
MAIN -> primary effect -> owed draw, when any -> joint CLEANUP -> turn end
```

| Family | MAIN commits | Owed draw | Left to CLEANUP |
| --- | --- | --- | --- |
| Take | Gain and return in gain/give normal form (below) | none | Noble |
| Visible reserve | Target and its gold, when the bank has gold | Public refill of the slot | Overflow return; noble |
| Blind reserve | Tier and its gold, when the bank has gold | Private reveal to the owner | Overflow return; noble |
| Visible buy | Target and its complete payment | Public refill of the slot | Noble |
| Reserved buy | Target and its complete payment | none | Noble |
| Forced pass | The canonical primary, only when no other choice exists | none | Noble |

- A take is one choice per afterstate, in gain/give normal form: a gain `G` (any
  sub-multiset of a legal take, the empty one included) and a return `R` of colors
  outside `G`, gold allowed, with `|R| = max(0, held + |G| − 10)`. A gain that is not a
  legal take is legal only if the hand ends at exactly ten. `(G, R)` is the positive and
  negative part of the net delta of any legal take followed by any return
  (`StagedTurn.TakesAreTheGainGiveNormalFormOfOfficialTraces`,
  [turn_test.cpp](../../tests/turn_test.cpp)). A platform that
  stages take-then-discard plays `(G, R)` as any legal take `T ⊇ G` returning `(T − G) + R`.
- The null move is the empty gain: legal exactly when the actor holds ten tokens and some
  take is legal.
- A MAIN choice's meaning depends only on its family: never on the draw, tier size,
  cleanup count, or whether the turn ends the game. No MAIN choice is a complete leaf.
- A MAIN choice selects no noble. It exposes the nobles eligible once its primary
  applies, carried-over eligibility included, at any cardinality.
- A vacated market slot is refilled before CLEANUP while its tier holds cards; the
  placed card leaves the unknown set in the same step. An empty tier leaves the slot
  empty. A blind reservation's draw fills the owner's slot with the drawn identity,
  concealed; it never touches the market.
- A joint cleanup is a complete return to ten tokens crossed with an eligible noble;
  with neither obligation there is exactly one empty cleanup. It applies return, then at
  most one noble, then end-of-turn adjudication and exactly one actor transition.
- A CLEANUP decision is the same actor after primary and draw: the actor may hold up to
  thirteen tokens while every other seat holds at most ten, an unrecorded reached target
  is valid, no draw is owed, the actor knows its own reserves, and every other state
  invariant holds. A MAIN decision is a stable turn start.
- A decision with one legal choice, MAIN or CLEANUP, is applied without policy, search or
  training record. A forced MAIN may still be followed by a discretionary CLEANUP.
- Terminal facts read prestige, purchases and ply, never the market or reserves, and
  arise only when a turn completes. A turn whose only cleanup ends the game is
  adjudicated without drawing; a discretionary cleanup is exposed after the draw even if
  every cleanup ends the game.
- The [action tree](#action-tree) groups a decision's legal choices; it is neither a
  rule, an input, nor a key. Certified outcomes belong to consumers that prove them.
- Transitions trust their inputs: a valid decision and one of its legal choices (or an
  engine-produced `MainOutcome`) yield a valid successor, so transitions check only their
  own operation. Full validation happens where outside input enters: supplied starts,
  `decision_choices` and consumers' decoded inputs.

## Action tree

[action_tree.hpp](../../include/splendor/action_tree.hpp) owns it. A decision
keeps one joint law over its legal choices (the leaves); the tree only groups them and
never adds a decision:

| Family | Intent (gain side) | Parameter (give side) |
| --- | --- | --- |
| `take` | The gain `G` (the null move: empty) | The return `R` |
| `reserve` | The market card or deck tier, with its forced gold | none |
| `buy` | The card, from the market or the actor's reserve | The complete payment |
| `pass` | The forced pass | none |

- A CLEANUP decision is one family, `cleanup`, each choice its own intent.
- Intents are local to a decision and each belongs to one family. Family and intent
  probabilities are sums over their leaves.
- A policy of any granularity over the tree is a law over its leaves; the engine's
  [granularity-neutral surfaces](game-engine.md#granularity-neutral-choices) expose each
  leaf's components for consumers that stage differently.

## Keys

| Phase | Key |
| --- | --- |
| MAIN | `main\|<effective action key of the primary>` |
| CLEANUP | `cleanup\|d=w,b,g,r,k,gold\|n=<noble slot or ->` |

Choices are ordered by ascending key. A key from another phase or decision is rejected
before any state change. Keys are alignment metadata, never trainable inputs. A blind
reservation's key names its tier and no card; a buy from a concealed reserve names the
card, so its key exists only in its owner's decisions.

`choice_key` derives keys from choices; field ordering must reproduce key-byte ordering.
Counts use single decimal digits at supported player counts; card IDs may be longer.
Changing the grammar must preserve this equivalence or revise the ordering contract.

## Chance and knowledge

- An owed draw's law is the state's unknown cards of the tier, each equiprobable
  (`MainOutcome::outcome_card_ids`): the physical deck in a complete world, the seat's
  marginal law in its own view, where the other seats' concealed cards of that tier are
  among the outcomes. Terminal facts never read a draw, so an ending turn is adjudicated
  before it: its public refill is left undrawn, while its blind reservation is still
  drawn into the final world, which names every card (`StagedTurn.EndingBlindReservationStillDrawsForItsOwner`).
- The environment draws with the episode's own seed domain
  ([seed law](../../include/splendor/seed.hpp), [episode seeds](../../include/splendor/detail/episode.hpp)), never with an agent's
  decision seed; a leaf played as a staged path draws the same card.
- MAIN observations, choices and keys depend only on the turn start as the actor
  knows it; no draw preview exists at MAIN. `MainOutcome` enumerates draw outcomes for
  consumers that evaluate them.
- A CLEANUP observation shows the actual refilled market, or the actor's revealed
  reservation, with the correspondingly depleted unknown set.
- Every decision is presented to its actor as the actor's view
  ([observations](observations.md)); perspective changes only at the actor transition.

## Atomic adapter

An [effective leaf](action-canonicalization.md) precommits a MAIN primary and CLEANUP
before any draw. Leaves biject with non-blind (MAIN, CLEANUP) key pairs and reach the
atomic successor under every draw, except that an ending atomic turn leaves its refill
undrawn. Blind reservations have no leaf. Players of
complete leaves use this adapter; they do not choose again at CLEANUP.
Consumers that project leaf targets or evaluate staged choices own those projections.

## Seat counts

- Actor, observer and owner are distinct seat arguments in the engine, observation,
  and their consumers; nothing in the engine assumes `other == 1 - seat`.
- Cyclic seat order and equal-turn termination are the only seat arithmetic.
- Terminal utilities are seat-indexed vectors and winners a seat set; nothing flips signs.
- The gain/give normal form and the action tree read only the bank and the hand: larger
  supplies change only which gains exist and more nobles only CLEANUP, never the tree's form.
