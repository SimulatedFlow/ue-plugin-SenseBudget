<!--
  PHASE 3 NOTE — do not publish until this block is resolved.

  Every number in this description is either a shipped default (budget 12, slow rate 0.5 s, starvation
  limit 2.0 s, park distance 6000 cm) or arithmetic derived from those defaults and shown in full, so it
  can be checked without a screenshot. There is deliberately NOT a single measured millisecond figure in
  here yet.

  Once the demo map screenshots exist, fill in ONLY from what is legible on them:
    - the "sense ms" figure with tiers on, and the same figure with tiers off (shot 2)
    - the guard count and the live/slow/parked split (shot 1)
  Quote the real number even where it is less flattering than the round one you were hoping for.
-->

# SenseBudget — A Budget For AI Perception Updates

**Documentation:** <https://wiki.teufel-engineering.com/en/SenseBudget/documentation>

**What the shipped demo map measures.** Ninety-six perceivers on one screen. With the tiers on:
`updates 12 / 12 this frame`, `live 12  slow 6  parked 78`. With the tiers off, the same scene in the same
frame: `updates 96 / 12`, everything live. **Ninety-six against twelve is the number this plugin is for.**

The millisecond line underneath moves from `0.133` to `0.180` — and that figure needs a warning rather than
a boast, because it is easy to sell wrongly. It is the **scheduler's own cost**: ranking the perceivers and
issuing the updates. It does **not** include the sight traces that were never fired, which is where the
actual saving lives and which depends entirely on your senses, your geometry and your collision channels.
Buy this for the ratio, then measure the traces in your own game. (Editor play-in-editor session, one
machine — not a promise.)

SenseBudget does not replace the engine's AIPerception and brings no senses of its own. It paces what is
already there, and it does not make any AI smarter — a guard that is far away and unimportant simply
reacts later.

---

## The problem it solves

`AIPerceptionComponent` gives every sense its own update rate. What the engine does not give you is a
**ceiling shared across all perceivers**. A hundred guards with sight and hearing are a hundred listeners
handed to the sight sense in the same frame, and that cost scales with how many enemies **exist**, not with
how many **matter** — the same whether the player is standing in front of them or two districts away.

The usual workaround is to switch perception off in the distance. That is exactly why an enemy sometimes
walks into the room without having seen anybody coming.

## What SenseBudget does

**A hard ceiling per frame.** Twelve perception updates by default. Whoever misses out is **not switched
off** — they move up the queue and go next. Nobody drops out, everybody slows down, and whoever is close
and important does not slow down at all.

**A rotation, not a round-robin.** Perceivers are ranked by distance to the nearest viewer, whether the
actor was recently rendered, an importance value from their profile, and **how far past their own due time
they are**. That last term is what stops anybody starving.

**Three tiers instead of on and off.**

* **Live** — looks every frame. The AI the player is actually interacting with.
* **Slow** — looks at the profile's rate, half a second by default. Still perceiving, just not every frame.
* **Parked** — looks rarely, and on events.

**Parked is not blind.** Two unconditional guarantees, and they are the reason this is not just "switch
distant perception off" with a nicer name:

* Every profile carries a **floor rate** that no tier ever undercuts. Set the parked interval to thirty
  seconds and the floor to five, and the guard still looks every five seconds — so it still notices the
  corpse that appeared in front of it, without anybody having to remember to fire an event.
* Any event inside the profile's **wake radius** — a shot, a shout, damage — pulls a parked perceiver
  straight back to Live before the next ranking runs.

**Numbers you can check.** A `UCanvas` counter box that survives a cooked **Shipping** build shows
perceivers, updates this frame against the ceiling, the live/slow/parked split, the longest wait *and* how
far overdue that perceiver actually is, the measured milliseconds the scheduler spent, and the five
perceivers the budget is failing worst.

Two waiting figures, because they answer two different questions: a Parked guard on a three-second rate
that is looked at every three seconds has waited three seconds and is **not** being neglected. Only
lateness is measured against the limit, and only lateness turns amber.

## A bound you can write down

Crossing the starvation line does not get a perceiver served instantly — it gets them to the back of the
queue of everybody else who has crossed, which drains at the budget per frame. So the worst case is:

```
worst lateness = starvation trigger + perceivers / (budget × frame rate)
```

With 500 perceivers, a budget of 12 and 60 Hz: `1.00 s + 500/720 = 1.69 s`, inside a 2.00 s promise.
That case is an automated test, not a claim — 900 simulated frames, asserting both the promise and the
arithmetic. If your own numbers push the sum over the limit, the counter box turns amber and tells you the
budget is too small for the crowd.

## Setup

Add a **Sense Budget** component to any AI actor that already has an `AIPerceptionComponent`, point it at a
`Sense Priority Profile` data asset, and set your GameMode's HUD class — or tick one box if you already
have a HUD class you would rather not reparent. That is the whole integration.

## What is in the box

* `USenseBudgetSubsystem` — the scheduler
* `USenseBudgetComponent` — the one component you add to an AI actor
* `USensePriorityProfile` — importance, rates, floor rate, park distance, wake rules, per enemy type
* `ASenseBudgetHUD` — the Canvas counter box
* `USenseBudgetStatics` — the ranking, tiering, wake and next-due maths as pure Blueprint-callable
  functions with no world, plus one node per demo button
* `USenseBudgetSettings` — project defaults
* Console: `Sense.Show`, `Sense.Budget`, `Sense.Stats`, `Sense.Tiers`, `Sense.Stress`, `Sense.Freeze`
* A demo map with 96 guards that colour themselves by tier, and buttons for budget, "fire a shot",
  "teleport player" and "tiers off"

## What it does not do

* It does **not** replace `AIPerception` and adds no senses.
* It does **not** change behaviour trees, EQS, navigation or how stimuli are handled.
* The milliseconds it reports are **its own** scheduler cost plus the listener updates it issues — not the
  engine's sight traces, which run later in the perception system's own tick. What bounds those is the
  updates count, and the plugin says so on the box and in the documentation rather than implying otherwise.

---

## Technical Details

**Documentation:** <https://wiki.teufel-engineering.com/en/SenseBudget/documentation>
**Support:** <mailto:teufelsilvan@gmail.com>

**Features**

* Hard per-frame ceiling on AI perception updates, with a starvation bound you can compute
* Three tiers — Live / Slow / Parked — instead of on and off
* Rotation ranked by distance, on-screen visibility, per-type importance and lateness
* Wake-on-event and a per-profile floor rate, so Parked is never blind
* Canvas counter box that survives Shipping, with a "waiting longest" list
* Ranking, tiering, wake and scheduling maths exposed as pure Blueprint functions and unit tested
* Six automation tests, including a 900-frame, 500-perceiver starvation simulation

**Code Modules**

* `SenseBudget` — Runtime, `LoadingPhase: PreDefault`

**Number of Blueprints:** 4 (demo content only)
**Number of C++ Classes:** 6
**Network Replicated:** No — the scheduler is a local cost decision and runs wherever the perceiver lives
**Supported Development Platforms:** Win64
**Supported Target Build Platforms:** Win64
**Engine Version:** 5.8

**Dependencies:** `Core`, `CoreUObject`, `Engine`, `AIModule`, `GameplayTasks`, `DeveloperSettings`,
`RenderCore`. No UMG, no editor module, no third-party code.

Built and verified on Win64 for both an Editor Development target and a Game Shipping target.

© 2026 Silvan Teufel. All Rights Reserved.
