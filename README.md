# SenseBudget — A Budget For AI Perception Updates

A hard ceiling on how many AI perception updates run per frame, spread over a rotation by distance and
importance instead of all at once. The AI that matters keeps seeing every frame, the AI behind the player
looks every half second, and a counter box shows which is which.

**Documentation:** <https://wiki.teufel-engineering.com/en/SenseBudget/documentation>

---

## What it is not

SenseBudget **does not replace** the engine's `AIPerception` and brings **no senses of its own**. It paces
what is already there. It also does not make any AI smarter — a guard that is far away and unimportant
reacts later, that is all. If you are looking for behaviour, this is the wrong plugin; this one is about
the cost of perceiving.

## The problem

`AIPerceptionComponent` gives every sense its own update rate, but there is no shared ceiling across all
perceivers. A hundred guards with sight and hearing are a hundred listeners handed to the sight sense in
the same frame — several hundred line traces — and they cost the same whether the player is standing in
front of them or two districts away.

The usual workaround is to switch perception off in the distance. That is why an enemy sometimes walks
into the room without having seen anybody coming.

## What SenseBudget does

* **A hard budget per frame** (12 updates by default). Whoever misses out is **not switched off** — they
  move up the queue and go next. Nobody drops out, everybody slows down, and whoever is close and
  important does not slow down at all.
* **A rotation, not a round-robin.** Ranked by distance to the nearest viewer, whether the actor was
  recently rendered, an importance value from the profile, and **how late it is**. That last term is what
  stops anybody starving: a perceiver past its own due time climbs until it gets a turn.
* **Three tiers instead of on and off.** `Live` (every frame), `Slow` (the profile's rate, 0.5 s by
  default), `Parked` (rarely, and on events).
* **Parked is not blind.** A shot next to a parked guard pulls it straight back to Live, and every profile
  carries a floor rate that no tier ever undercuts. Otherwise this would just be switching perception off
  with a nicer name.
* **Measured numbers on a Canvas box that survives Shipping:**
  `perceivers 96 | updates 12/12 | live 8 slow 61 parked 27 | longest wait 0.42 s overdue 0.11 s | sense ms 0.31`

## Getting started

1. Enable the plugin.
2. Create a `Sense Priority Profile` data asset per enemy type (Content Browser → Miscellaneous → Data
   Asset → `SensePriorityProfile`).
3. Add a **Sense Budget** component to each AI actor that has an `AIPerceptionComponent`, and point it at
   a profile.
4. Set your GameMode's HUD class to `Sense Budget HUD`, or turn on
   *Project Settings → Plugins → SenseBudget → Auto Draw Stats On Any HUD*.

That is all. Defaults are in *Project Settings → Plugins → SenseBudget*.

## Console commands

| Command | What it does |
| --- | --- |
| `Sense.Show [0\|1]` | Show or hide the counter box |
| `Sense.Budget <n>` | Set the per-frame ceiling |
| `Sense.Stats` | Print the measured counters to the log |
| `Sense.Tiers [0\|1]` | List every perceiver's tier — or turn tiering off/on for comparison |
| `Sense.Stress <n>` | Add *n* synthetic perceivers to the queue (`0` clears them) |
| `Sense.Freeze [0\|1]` | Stop the scheduler where it stands, for a screenshot |

## Requirements

Unreal Engine 5.8, Win64. One runtime module. Depends on `AIModule`, `GameplayTasks`,
`DeveloperSettings` and `RenderCore`. No UMG, no editor module, no third-party code.

## Support

<mailto:teufelsilvan@gmail.com>

© 2026 Silvan Teufel. All Rights Reserved.
