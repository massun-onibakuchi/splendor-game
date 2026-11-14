---
title: Specs
description: Canonical index for the Splendor engine and practical-play contracts.
date: 2026-11-14
status: active
tags: [specs]
---

# Specs

`MUST` and `MUST NOT` state requirements. `SHOULD` states a recommendation whose tradeoff
must be understood before deviation.

| Area | Owner |
| --- | --- |
| Library boundary, game execution, choice surfaces, clocks and match adjudication | [Game engine](game-engine.md) |
| Rules, turn phases, primary effects, chance, staged keys/order and action tree | [Staged actions](staged-actions.md) |
| Seat knowledge, views and noninterference | [Observations](observations.md) |
| Seat-agnostic native match host, agent schedule, log, archive and server | [Practical play](play.md) |

Specs define the intended end state, not an implementation inventory or task history.
Each concept has one owner; other specs link to it. Exact schemas, layouts and fields are
referenced at their executable owner, not copied.
