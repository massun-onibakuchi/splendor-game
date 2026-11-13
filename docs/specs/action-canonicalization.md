---
title: Effective Action Canonicalization
description: Effective action identity, equivalence, ordering, and alignment.
date: 2026-11-14
status: active
tags:
  - specs
  - actions
---

# Effective Action Canonicalization

## Contract

`splendor-effective-candidate-v1` defines complete-turn leaves for the
[atomic adapter](staged-actions.md#atomic-adapter). Native code generates them from
the actor's view: no blind-reservation leaf or buy of an unidentified card exists.
`action_key` identifies an action within its decision, not a global state or product.
Its encoding also names MAIN primaries, with MAIN's delta, no noble, and the additional
`reserve_blind` family.

Reservation generation takes gold exactly when bank gold is available, even if cleanup
returns it. Blind reservations follow the reserve-count and deck-availability rules in
[staged actions](staged-actions.md#game-rules).

## Candidate descriptor

| Field | Meaning |
| --- | --- |
| `action_key` | Canonical encoding of the effective semantics |
| `family` | `take`, `reserve_visible`, `reserve_blind`, `buy_visible`, `buy_reserved`, `pass_forced` |
| `token_delta` | Six-color take-minus-return delta, excluding payment |
| `payment` | Six-color buy payment; zero otherwise |
| `source_zone` | `market`, `deck`, `self_reserve`, `opponent_reserve`, `none` |
| `source_tier`, `source_slot` | Card or deck tier and visible market slot, when applicable |
| `reserve_owner`, `reserve_slot` | Reserve location, when applicable |
| `target_card_id` | Target card known to the actor, when applicable; a blind reservation has none |
| `chosen_noble_slot` | Precommitted public noble; absent only when none is claimable |

Equivalent traces retain no raw take/discard sequence, gold flag, or representative
engine action. `opponent_reserve` is zone vocabulary, not an emitted action.

## Key encoding

```text
family "|d=" i8x6 "|p=" u8x6 "|z=" zone
       ["|t=" tier] ["|s=" slot] ["|o=" owner]
       ["|r=" reserve_slot] ["|c=" card] ["|n=" noble]
```

`d` and `p` are always six comma-separated decimals. Optional fields appear only
when present, in the displayed order. Descriptor validation owns family-specific
presence rules and key projection.

Family constraints are closed:

| Family | Delta / payment | Source fields |
| --- | --- | --- |
| `take` | Any (zero only for the null move) / zero | Zone `none`; tier, market slot, owner, reserve slot, and card absent |
| `reserve_visible` | Any / zero | Zone `market`; tier, market slot, and card required; owner and reserve slot absent |
| `reserve_blind` | Any / zero | Zone `deck`; tier required; market slot, owner, reserve slot, and card absent |
| `buy_visible` | Zero / any legal payment | Zone `market`; tier, market slot, and card required; owner and reserve slot absent |
| `buy_reserved` | Zero / any legal payment | Zone `self_reserve`; tier, acting-seat owner, reserve slot, and card required; market slot absent |
| `pass_forced` | Zero / zero | Zone `none`; all source fields absent |

Every family follows the noble branching rule below.

## Canonicalization

- Takes follow the [gain/give normal form](staged-actions.md#turn): one candidate per net
  delta and noble choice. A token action netting to zero is the null move
  `take|d=0,0,0,0,0,0|…`.
- Reserve variants with the same card or tier consequence and net delta collapse.
- Distinct payments, target cards, source locations, tiers, or noble choices stay
  distinct.
- For each collapsed base action, native code applies its deterministic
  pre-chance consequence and computes the acting player's claimable public nobles.
  With none claimable, emit exactly one candidate without
  `chosen_noble_slot`; otherwise emit exactly one candidate per claimable slot and
  no no-claim variant. This applies to every family, including the null take and
  `pass_forced`.
- `pass_forced` exists only in a nonterminal state with no normal candidate;
  terminal states have no candidates.
- Representative choice is deterministic from the actor's view of the decision. After
  collapse and noble branching, sort by ascending lexicographic UTF-8 bytes of
  `action_key`.

## Batch invariants

Every batch MUST have contiguous indices, nonempty unique keys, and a stored key
equal to re-encoding its descriptor. Leaf-policy distributions and selections MUST
use this order without sorting or joining on another identity.

Any change to descriptors, key encoding, collapse, or order requires a new contract
identifier. Every consumer MUST preserve candidate alignment.
