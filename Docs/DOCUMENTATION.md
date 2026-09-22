# SenseBudget — Documentation

**A budget for AI perception updates.**

**Online:** <https://wiki.teufel-engineering.com/en/SenseBudget/documentation>
**Support:** <mailto:teufelsilvan@gmail.com>

| | |
| --- | --- |
| **Engine** | Unreal Engine **5.8** |
| **Target platforms** | Win64 |
| **Development platforms** | Win64 |
| **Modules** | one — `SenseBudget`, Runtime, `LoadingPhase: PreDefault` |
| **Dependencies** | `Core`, `CoreUObject`, `Engine`, `AIModule`, `GameplayTasks`, `DeveloperSettings`, `RenderCore` |
| **Not used** | UMG, UnrealEd, Niagara, Chaos, third-party code |
| **C++ / Blueprint** | Both. Everything the plugin decides is callable from Blueprint. |
| **Network replicated** | No — the scheduler is a local cost decision and runs wherever the perceiver lives |

---

## Contents

1. [What the plugin does not do](#1-what-the-plugin-does-not-do)
2. [The problem](#2-the-problem)
3. [Installation](#3-installation)
4. [Quick start — five minutes](#4-quick-start--five-minutes)
5. [How it works](#5-how-it-works)
6. [Why Parked is not blind](#6-why-parked-is-not-blind)
7. [Class overview](#7-class-overview)
8. [The Sense Priority Profile](#8-the-sense-priority-profile)
9. [Project settings](#9-project-settings)
10. [The counter box](#10-the-counter-box)
11. [Blueprint API](#11-blueprint-api)
12. [C++ API and code examples](#12-c-api-and-code-examples)
13. [Console commands](#13-console-commands)
14. [The demo map](#14-the-demo-map)
15. [The maths, and its bounds](#15-the-maths-and-its-bounds)
16. [Tests](#16-tests)
17. [Multiplayer, and what is measured](#17-multiplayer-and-what-is-measured)
18. [Supported platforms and engine versions](#18-supported-platforms-and-engine-versions)
19. [Troubleshooting](#19-troubleshooting)

---

## 1. What the plugin does not do

Read this first; it will save you an evening.

* **SenseBudget does not replace `AIPerception`.** It brings no senses of its own. It does not see, hear,
  or feel damage. It decides **who among your existing perceivers is allowed to run a perception update
  this frame**, and it does that by switching the senses your `UAIPerceptionComponent` already has on and
  off in a rotation.
* **It does not make AI smarter.** A guard that is far away and unimportant reacts *later*. Nothing about
  what it does when it reacts changes.
* **It does not change behaviour trees, EQS, navigation, or stimuli handling.** Your
  `OnTargetPerceptionUpdated` handlers fire exactly as they did.
* **It does not measure the engine's sight traces.** The milliseconds on the counter box are the
  scheduler's own tick plus the listener updates it issues. What bounds the traces is the *updates* line,
  which is a count, not a time. See [§17](#17-multiplayer-and-what-is-measured) — this distinction is
  deliberate and the box says so.

---

## 2. The problem

`UAIPerceptionComponent` gives every sense its own update rate — `UAISenseConfig_Sight` has one,
`UAISenseConfig_Hearing` has one. What the engine does not have is a **ceiling shared across all
perceivers**. A hundred guards with sight and hearing are a hundred listeners handed to the sight sense in
the same frame, and each of them is line traces. That cost scales with **how many enemies exist**, not with
**how many matter**, so it is the same whether the player is standing in front of them or two districts
away.

The `Significance Manager` is the engine's general answer to "far away means less work", but it knows
nothing about perception and you build the connection yourself.

The usual workaround is to switch perception off past some distance. It works, right up until the moment
an enemy walks into the room without having seen anybody coming — because for the last thirty seconds it
was not looking at all.

---

## 3. Installation

### From Fab

1. Install **SenseBudget** for Unreal Engine 5.8 from the Epic Games Launcher (*Library → Fab Library →
   Install to Engine*).
2. Open your project. **Edit → Plugins → Artificial Intelligence → SenseBudget → Enabled.**
3. Restart the editor when prompted.

### Into a project (source install)

1. Copy the `SenseBudget` folder into `<YourProject>/Plugins/SenseBudget/` so that
   `<YourProject>/Plugins/SenseBudget/SenseBudget.uplugin` exists.
2. **Blueprint-only project:** right-click the `.uproject` → *Generate Visual Studio project files*, then
   open the project — the editor offers to build the missing module. A C++ project builds it with the
   rest of the solution.
3. Enable the plugin under *Edit → Plugins* and restart.

### Adding it to a C++ module

Only needed if you want to call the API from C++. Add the module to your own `Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core", "CoreUObject", "Engine",
    "AIModule",       // you already need this for UAIPerceptionComponent
    "SenseBudget",
});
```

All public headers live at the top level of the module's include path:

```cpp
#include "SenseBudgetSubsystem.h"
#include "SenseBudgetComponent.h"
#include "SenseBudgetStatics.h"
#include "SensePriorityProfile.h"
#include "SenseBudgetSettings.h"
#include "SenseBudgetHUD.h"
#include "SenseBudgetTypes.h"   // enums and structs; pulled in by all of the above
```

### Verifying the install

Play in editor and type `Sense.Stats` in the console. If the log prints a counter line, the subsystem is
alive. If it prints *"no SenseBudget subsystem in this world"*, you are not in a game world — the
subsystem only exists for `Game`, `PIE` and `GamePreview` worlds, never for an editor world.

---

## 4. Quick start — five minutes

1. **Make a profile.** Content Browser → right-click → *Miscellaneous → Data Asset* →
   **Sense Priority Profile**. Name it `DA_SenseProfile_Grunt`. The defaults are already sensible:
   importance 1.0, slow rate 0.5 s, parked rate 3 s, floor rate 5 s, park distance 6000 cm, wakes on
   noise and damage inside 2500 cm.

2. **Add the component.** Open an AI actor that already has a `UAIPerceptionComponent`. *Add Component →
   **Sense Budget***. Set its `Profile` to the asset from step 1.

   That is the whole integration. The component finds the perception component on its own owner,
   registers on `BeginPlay` and unregisters on `EndPlay`.

   * Leave `Profile` empty and the subsystem substitutes an ordinary default rather than refusing to
     manage the perceiver.
   * If an actor has more than one perception component, set `PerceptionOverride` explicitly.

3. **Show the numbers.** Either set your GameMode's HUD class to **Sense Budget HUD**, or — if you already
   have a HUD class you would rather not reparent — turn on *Project Settings → Plugins → SenseBudget →
   Auto Draw Stats On Any HUD*. The two paths know about each other and cannot draw the box twice.

4. **Prove it.** Play. In the console:

   ```
   Sense.Stress 200      add 200 synthetic perceivers to the queue
   Sense.Budget 4        watch "updates" drop to 4/4 and the tiers redistribute
   Sense.Tiers 0         comparison mode: everybody Live, no ceiling — watch sense ms climb
   Sense.Tiers 1         back on
   Sense.Stress 0        clear the synthetic crowd
   ```

5. **Tune.** *Project Settings → Plugins → SenseBudget → Update Budget Per Frame* is the one knob that
   matters. Everything else has a working default.

---

## 5. How it works

Every frame, the `USenseBudgetSubsystem` does four things:

1. **Gather.** For each registered perceiver: distance to the nearest viewer (the player camera *and* the
   player pawn, whichever is closer), whether the owning actor was recently rendered, an importance value
   from its profile, how long since it last looked, and **how far past its own due time it is**.
2. **Rank.** `USenseBudgetStatics::RankPerceivers` sorts them:

   ```
   Score =   Importance × (DistanceTerm + VisibilityBonus)     ← who matters
           + WaitWeight × OverdueSeconds                       ← gentle rotation
           + StarvationBonus + OverdueSeconds  (past the line) ← nobody starves
           + WakeBonus                         (event pending) ← Parked is not blind
   ```

   `DistanceTerm` falls off hyperbolically: `DistanceWeight / (1 + Distance / ReferenceDistance)`. The
   difference between five metres and ten is worth far more than the difference between fifty and a
   hundred. Ties break by registration id, so the order is stable frame to frame and two screenshots of
   the same scene agree.

3. **Assign tiers.** `AssignTiers` walks the ranked list once. The first `Budget` entries that are not
   beyond their own park distance become **Live**; anything beyond its park distance becomes **Parked**
   whatever its rank; everything else becomes **Slow**. Exactly `min(Budget, eligible)` entries come back
   Live — never one more.
4. **Serve.** `SelectUpdates` takes the due ones, in ranked order, up to the budget. Serving a perceiver
   means: enable the senses this plugin borrowed from it, call `RequestStimuliListenerUpdate()`, and
   schedule its next look with `NextDue`. Its senses stay open for `SenseWindowSeconds` (0.05 s ≈ three
   frames at 60 Hz) and are then switched off again until its next turn.

   The window is not one frame on purpose: the engine's sight sense processes a bounded number of traces
   per tick, so a listener enabled and disabled inside a single frame might never actually get its trace.
   A few frames means the look that was scheduled is a look that happened.

**Whoever does not get a turn is not switched off.** Their lateness grows, which raises their score, which
is what puts them at the front next frame. Nobody drops out, everybody slows down, and whoever is close
and important does not slow down at all.

### The three tiers

| Tier | Meaning | Senses |
| --- | --- | --- |
| **Live** | Looks every frame, subject to the budget | Always open |
| **Slow** | Looks at the profile's `SlowIntervalSeconds` (0.5 s default) | Open in a short window around each look |
| **Parked** | Looks at the profile's `ParkedIntervalSeconds` (3 s default), and on events | Open in a short window around each look |

### What is borrowed, and what is given back

SenseBudget only ever toggles **the senses that were enabled on a component at the moment it registered**.
That set is snapshotted per perceiver. On `Unregister` — and on world teardown — exactly that set is
switched back on. A project that had deliberately disabled hearing on one guard before registering gets
that guard back with hearing still disabled, rather than the plugin handing back a different configuration
than it borrowed.

---

## 6. Why Parked is not blind

This is the section that matters, and it is why this plugin is not just "switch distant perception off"
with a nicer name.

**A parked perceiver is still looking.** Two mechanisms guarantee it, and both are unconditional:

**(a) The floor rate.** Every `SensePriorityProfile` has a `MaxIntervalSeconds`. **No tier ever schedules
a look further ahead than that**, whatever the tier interval says. Set `ParkedIntervalSeconds` to thirty
and `MaxIntervalSeconds` to five, and the guard still looks every five seconds. So a guard that has been
standing in a corner for a minute still notices the corpse that appeared in front of it, without anybody
having to remember to fire an event.

**(b) The wake rule.** Any event inside the profile's `WakeRadius` pulls a parked perceiver **straight back
to Live**, before any of the ranking runs. Call it from Blueprint:

```
Nudge Senses Around  (Location, Radius, Reason)   ← "a shot was fired here"
Wake Up              (Reason, Distance)           ← on the perceiver itself, e.g. from a damage handler
```

The woken perceiver gets `WakeBonus` — larger than any distance, importance or starvation score can
reach — so it is first in the queue on the very next frame. `ShouldWake` decides: the **reason** must be
one the profile accepts (`bWakeOnNoise`, `bWakeOnDamage`; `Manual` and `Script` are never refusable), and
the **distance** must be inside `WakeRadius`. Two gates, both must open — a quiet footstep does not carry,
and a deaf turret does not care that it did.

Without this rule, the Parked tier would be switching perception off with a friendlier name, and the first
thing a player would notice is an enemy that never saw them coming.

> One thing the wake rule deliberately does **not** do: it does not carve out a free Live slot above the
> budget. A woken perceiver gets a head start in the *ranking*, not an exemption from the *ceiling*. The
> promise on the box is "never more than Budget", and an exception for waking would quietly break it in
> the one situation — a firefight, where twenty guards are woken at once — where the frame is busiest.
> Twenty woken guards fill the next two frames' budgets; none of them waits longer than that.

---

## 7. Class overview

Six C++ classes. Two of them are the only ones most projects ever touch: the component and the profile.

| Class | Base | Where it lives | What it is for |
| --- | --- | --- | --- |
| `USenseBudgetComponent` | `UActorComponent` | On each AI actor | **The one thing you add.** Hands the actor's `UAIPerceptionComponent` to the scheduler, reports the tier, takes wake events. Owns no scheduling logic. |
| `USensePriorityProfile` | `UPrimaryDataAsset` | Content Browser, one per enemy type | Importance, slow/parked/floor rates, park distance, wake rules. Shared by every instance of a kind, so a designer retunes every grunt at once. |
| `USenseBudgetSubsystem` | `UTickableWorldSubsystem` | One per game world, automatic | The scheduler. Ranks, tiers, serves, measures. `Register` / `Unregister` / `SetBudget` / `Nudge` / `GetStats`. |
| `USenseBudgetStatics` | `UBlueprintFunctionLibrary` | Static | The maths as pure world-free functions — `RankPerceivers`, `AssignTiers`, `ShouldWake`, `NextDue`, `SelectUpdates` — plus one Blueprint node per demo button. The subsystem calls exactly these; there is no second copy of the logic. |
| `ASenseBudgetHUD` | `AHUD` | GameMode's HUD class | The `UCanvas` counter box. Survives a cooked Shipping build. |
| `USenseBudgetSettings` | `UDeveloperSettings` | *Project Settings → Plugins → SenseBudget* | Project-wide defaults: budget, starvation limit, rank weights, box options. |

### Types

| Type | What it is |
| --- | --- |
| `ESenseTier` | `Live` / `Slow` / `Parked` |
| `ESenseWakeReason` | `Noise` / `Damage` / `Manual` / `Script`. A profile may refuse the first two; the last two are always honoured. |
| `FSenseRankWeights` | Distance weight, reference distance, visibility bonus, importance weight, wait weight, starvation trigger, starvation bonus, wake bonus |
| `FSenseRankEntry` | One perceiver reduced to numbers: id, distance, recently-rendered, importance, wait, **overdue**, park distance, next due, wake pending — plus the outputs `Score` and `Tier`. No pointers, no world. |
| `FSenseBudgetStats` | Everything the counter box draws: counts, tier split, longest wait, most overdue, measured milliseconds, and the `LongestWaits` list |
| `FSenseWaitEntry` | One row of the "waiting longest" list: label, wait, overdue, tier |

---

## 8. The Sense Priority Profile

| Property | Default | What it means |
| --- | --- | --- |
| `Importance` | 1.0 | Multiplies the distance and visibility terms. A boss is 4.0, a background civilian 0.25. It does **not** multiply lateness — importance decides who goes first, not who is allowed to starve. |
| `SlowIntervalSeconds` | 0.5 | Seconds between looks in the Slow tier. |
| `ParkedIntervalSeconds` | 3.0 | Seconds between looks in the Parked tier. |
| `MaxIntervalSeconds` | 5.0 | **The floor rate.** No tier ever schedules further ahead than this. See [§6](#6-why-parked-is-not-blind). |
| `ParkDistance` | 6000 cm | Beyond this from the nearest viewer, the perceiver is Parked rather than Slow. **Zero or less means never park by distance** — use it for objective guards and scripted ambushes. |
| `bWakeOnNoise` | true | A noise inside `WakeRadius` wakes it. |
| `bWakeOnDamage` | true | Damage to its owner wakes it. |
| `WakeRadius` | 2500 cm | How far away an event still counts as "next to me". Set it generously: a wake that was not needed costs one perception update; a wake that was missed costs an enemy that never looked up. |

Two helpers on the profile, both `BlueprintPure`: `GetIntervalForTier(Tier)` returns the scheduled gap for
a tier already clamped to the floor rate (Live is always zero), and `AllowsWakeReason(Reason)` answers the
reason half of the wake rule without the distance half.

The clamp to `MaxIntervalSeconds` only ever makes a perceiver look **sooner**. A parked interval shorter
than the floor is left alone.

### Three profiles that cover most projects

| | Importance | Slow | Parked | Floor | Park distance |
| --- | --- | --- | --- | --- | --- |
| **Elite / boss** | 3.0 – 4.0 | 0.25 s | 1.0 s | 2.0 s | 0 (never park) |
| **Patrolling grunt** | 1.0 | 0.5 s | 3.0 s | 5.0 s | 6000 cm |
| **Civilian / ambience** | 0.25 | 1.0 s | 5.0 s | 8.0 s | 3000 cm |

---

## 9. Project settings

*Project Settings → Plugins → SenseBudget*

| Setting | Default | What it means |
| --- | --- | --- |
| `Enabled` | true | Off leaves the subsystem alive — components register, counters gather, the box draws — but nobody is tiered or throttled and every sense stays open. The honest fallback while you bisect a perception problem. |
| `Update Budget Per Frame` | 12 | The hard ceiling. |
| `Starvation Seconds` | 2.0 | **The promise**: nobody is ever more than this far past their own due time. Shown on the box beside the measured value. |
| `Starvation Trigger Fraction` | 0.5 | How far below the promise the ranking actually starts jumping people up the queue. See [§15](#15-the-maths-and-its-bounds) — this is not a knob to leave at 1.0. |
| `Rank Weights` | — | Distance weight, reference distance, visibility bonus, importance weight, lateness weight, starvation bonus, wake bonus. |
| `Default Park Distance` | 6000 cm | Used for a perceiver registered with no profile at all. |
| `Sense Window Seconds` | 0.05 | How long a Slow/Parked perceiver's senses stay open around each look. |
| `Recently Rendered Tolerance` | 0.2 | Passed to `AActor::WasRecentlyRendered`. |
| `Show Stats By Default` | true | Draw the counter box from the first frame. |
| `Auto Draw Stats On Any HUD` | false | Draw it through `AHUD::OnHUDPostRender` for projects with their own HUD class. |
| `Longest Wait Rows` | 5 | How many of the latest perceivers the box lists. |

Settings are read once, at subsystem `Initialize`. Changing them in the editor while playing takes effect
on the next PIE session; changing the budget *live* is what `Sense.Budget <n>` and `Set Sense Budget` are
for.

---

## 10. The counter box

Drawn on `UCanvas` from `AHUD`, so it survives a cooked **Shipping** build. A plugin whose whole claim is a
number cannot afford that number to be stripped in the build that ships.

```
SenseBudget
perceivers     96
updates        12 / 12 this frame
tiers          live 8   slow 61   parked 27
queue          31 due   19 waiting for a later frame
longest wait   3.02 s   overdue 0.11 s   (limit 2.00 s)
sense ms       0.310   (rank 0.080 / issue 0.230)
waiting longest
   BP_Guard_C_47             3.02 s   + 0.11 late   parked
   ...
```

*(Layout example. The figures on your own box are measured on your own scene.)*

**Two waiting numbers, because they answer two different questions.** *Longest wait* is how long ago the
most neglected perceiver last looked. *Overdue* is how far past **its own** due time that is. A parked
guard on a three-second rate that is looked at every three seconds has a wait of three seconds and is
**not** being neglected — so only the second number is measured against the limit, and only the second
number turns amber.

`sense ms` is the scheduler's own tick plus the listener updates it issued. It is **not** the engine's
sight traces; those run later, in the perception system's own tick. See
[§17](#17-multiplayer-and-what-is-measured).

Position and width are properties on `ASenseBudgetHUD`: `StatsBoxOrigin` (default 28, 90) and
`StatsBoxWidth` (default 470, wide enough for the "waiting longest" names).

There are no buttons on the box on purpose. An `AHUD` hit box is tested against
`UGameViewportClient::GetMousePosition`, which reports nothing on a machine with no mouse attached — a
capture rig, a build agent, a headless test — so the click never lands. Numbers live on the Canvas, where
they always draw; controls belong in a widget, where they always receive the click.

---

## 11. Blueprint API

### The maths (pure, no world needed)

| Node | Returns |
| --- | --- |
| `Rank Perceivers (Entries, Weights)` | The entries sorted best-first, `Score` filled in |
| `Assign Tiers (Ranked, Budget)` | The entries with `Tier` filled in; exactly `min(Budget, eligible)` are Live |
| `Should Wake (Profile, Reason, Distance)` | Whether this event pulls this perceiver back to Live |
| `Next Due (Tier, Profile, Now)` | The absolute time of the next allowed look, already clamped to the floor rate |
| `Select Updates (Tiered, Budget, Now)` | The ids to serve this frame |

### The scheduler

| Node | What it does |
| --- | --- |
| `Get Sense Budget Stats` | Everything on the counter box, for your own widget |
| `Set Sense Budget (n)` / `Get Sense Budget` | The per-frame ceiling — what a *Budget 4 / 12 / 96* button calls |
| `Nudge Senses Around (Location, Radius, Reason)` | "A shot was fired here." Returns how many were woken |
| `Set Sense Tiers Enabled (bool)` / `Are Sense Tiers Enabled` | Comparison mode: everybody Live, every frame, no ceiling |
| `Set Sense Budget Frozen (bool)` | Stop the scheduler where it stands, for a screenshot |
| `Set Sense Stats Visible (bool)` | Show/hide the box |
| `Get Tier For Perception (Perception)` | The tier a given perception component sits in |
| `Get Tier Color (Tier)` | Live bright, Slow middling, Parked dark |
| `Get Tier Display Name (Tier)` | "Live" / "Slow" / "Parked" |

### On the Sense Budget component

`Register With Budget`, `Unregister From Budget`, `Is Managed By Sense Budget`, `Get Tier`,
`Get Seconds Since Last Update`, `Get Tier Display Color`, `Get Managed Perception`,
`Wake Up (Reason, Distance)`, and the `On Tier Changed (New, Old)` event — bind it to recolour a guard
instead of polling every frame.

> The "am I being managed" query is called `IsManagedBySenseBudget`, **not** `IsRegistered`.
> `UActorComponent::IsRegistered()` already exists and means something entirely different — whether the
> component itself is registered with the world — and shadowing it produces a component that answers the
> wrong question to every piece of engine code that asks.

### On the subsystem, for Blueprints that want it directly

`Get` (static, world context), `Register`, `Unregister`, `Is Perceiver Managed`, `Set Budget`,
`Get Budget`, `Nudge`, `Nudge Around`, `Set Tiers Enabled`, `Set Frozen`, `Set Stats Visible`,
`Get Stats`, `Get Tier`, `Get Seconds Since Update`, and under *Debug*: `Add Synthetic Perceivers`,
`Clear Synthetic Perceivers`.

---

## 12. C++ API and code examples

### Adding the component from C++

```cpp
// MyGuard.h
#include "GameFramework/Character.h"
#include "MyGuard.generated.h"

class UAIPerceptionComponent;
class USenseBudgetComponent;

UCLASS()
class MYGAME_API AMyGuard : public ACharacter
{
    GENERATED_BODY()

public:
    AMyGuard();

protected:
    UPROPERTY(VisibleAnywhere, Category = "AI")
    TObjectPtr<UAIPerceptionComponent> Perception;

    UPROPERTY(VisibleAnywhere, Category = "AI")
    TObjectPtr<USenseBudgetComponent> SenseBudget;
};
```

```cpp
// MyGuard.cpp
#include "MyGuard.h"
#include "Perception/AIPerceptionComponent.h"
#include "SenseBudgetComponent.h"

AMyGuard::AMyGuard()
{
    Perception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("Perception"));

    // The Sense Budget component finds the perception component on its own owner, so the only thing
    // left to set is the profile - and even that is optional; without one the subsystem substitutes
    // an ordinary default rather than leaving the perceiver unmanaged.
    SenseBudget = CreateDefaultSubobject<USenseBudgetComponent>(TEXT("SenseBudget"));
}
```

Assign `SenseBudget->Profile` in a Blueprint subclass, or load a profile in the constructor with
`ConstructorHelpers::FObjectFinder<USensePriorityProfile>`.

### Reacting to a tier change instead of polling

```cpp
// In BeginPlay
SenseBudget->OnTierChanged.AddDynamic(this, &AMyGuard::HandleTierChanged);

// UFUNCTION(), because the delegate is dynamic
void AMyGuard::HandleTierChanged(ESenseTier NewTier, ESenseTier OldTier)
{
    // Turn off the expensive stuff that only matters while this guard is looking every frame.
    GetMesh()->SetComponentTickInterval(NewTier == ESenseTier::Live ? 0.0f : 0.1f);

    if (UMaterialInstanceDynamic* Mid = GetMesh()->CreateAndSetMaterialInstanceDynamic(0))
    {
        Mid->SetVectorParameterValue(TEXT("Tint"), USenseBudgetStatics::GetTierColor(NewTier));
    }
}
```

### Waking a perceiver when something happens

```cpp
#include "SenseBudgetStatics.h"

// A weapon fired. Everybody nearby whose profile accepts a noise comes back to Live on the next tick.
void AMyWeapon::OnFired(const FVector& MuzzleLocation)
{
    const int32 Woken = USenseBudgetStatics::NudgeSensesAround(
        this, MuzzleLocation, /*Radius=*/ 4000.0f, ESenseWakeReason::Noise);

    UE_LOG(LogTemp, Verbose, TEXT("Shot woke %d perceivers"), Woken);
}
```

```cpp
// This particular guard was hit. Distance zero: it happened to me.
float AMyGuard::TakeDamage(float Damage, const FDamageEvent& Event, AController* Causer, AActor* Source)
{
    if (SenseBudget)
    {
        SenseBudget->WakeUp(ESenseWakeReason::Damage, 0.0f);
    }
    return Super::TakeDamage(Damage, Event, Causer, Source);
}
```

`NudgeSensesAround` returns the number actually woken, so you can tell "nothing was in range" apart from
"the wake rule is not firing".

### Driving the scheduler

```cpp
#include "SenseBudgetSubsystem.h"

void AMyDirector::EnterCombat()
{
    if (USenseBudgetSubsystem* Budget = USenseBudgetSubsystem::Get(this))
    {
        Budget->SetBudget(24);           // busier scene, wider ceiling
    }
}

void AMyDirector::PlayCutscene()
{
    if (USenseBudgetSubsystem* Budget = USenseBudgetSubsystem::Get(this))
    {
        Budget->SetTiersEnabled(false);  // everybody Live until the cutscene is over
    }
}
```

`USenseBudgetSubsystem::Get` returns `nullptr` outside a game world — always check it.

### Registering by hand

Only needed if you are not using the component (a perceiver on an actor you do not own, a pooled
perception component, a perceiver you want to manage for part of its life):

```cpp
if (USenseBudgetSubsystem* Budget = USenseBudgetSubsystem::Get(this))
{
    const int32 Id = Budget->Register(MyPerceptionComponent, MyProfile);
    // Id == INDEX_NONE means the component was null or already registered.
}

// ... later, before the component goes away. Senses are restored to exactly what was borrowed.
if (USenseBudgetSubsystem* Budget = USenseBudgetSubsystem::Get(this))
{
    Budget->Unregister(MyPerceptionComponent);
}
```

Unregistering in the middle of an assignment is safe: results are looked back up by stable id, and an id
that has left the registry is dropped rather than applied to whoever moved into that slot.

### Reading the numbers into your own UI

```cpp
#include "SenseBudgetStatics.h"

void UMyDebugWidget::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
    Super::NativeTick(Geometry, DeltaTime);

    const FSenseBudgetStats Stats = USenseBudgetStatics::GetSenseBudgetStats(this);

    PerceiverText->SetText(FText::AsNumber(Stats.Perceivers));
    UpdateText->SetText(FText::FromString(
        FString::Printf(TEXT("%d / %d"), Stats.UpdatesThisFrame, Stats.Budget)));

    // Only lateness is measured against the promise. Raw wait is not a fault.
    const bool bOverBudget = Stats.MostOverdueSeconds > Stats.StarvationSeconds;
    OverdueText->SetColorAndOpacity(bOverBudget ? FLinearColor::Yellow : FLinearColor::White);
}
```

### Using the maths on its own

The four decision functions are pure, take no world and touch nothing — which is what makes them
unit-testable, and also means you can run them over your own data:

```cpp
#include "SenseBudgetStatics.h"

TArray<FSenseRankEntry> Entries;
for (int32 Index = 0; Index < 100; ++Index)
{
    FSenseRankEntry& Entry = Entries.AddDefaulted_GetRef();
    Entry.Id                = Index;
    Entry.DistanceToViewer  = Index * 150.0f;
    Entry.bRecentlyRendered = Index < 10;
    Entry.Importance        = (Index == 0) ? 4.0f : 1.0f;
    Entry.OverdueSeconds    = 0.0f;
    Entry.ParkDistance      = 6000.0f;
}

FSenseRankWeights Weights;                                  // defaults are the shipped ones
const TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers(Entries, Weights);
const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, /*Budget=*/ 12);
const TArray<int32> ToServe          = USenseBudgetStatics::SelectUpdates(Tiered, 12, /*Now=*/ 0.0f);

check(ToServe.Num() <= 12);   // the ceiling, with no exceptions anywhere
```

### Reading the project settings

```cpp
#include "SenseBudgetSettings.h"

const USenseBudgetSettings& Settings = USenseBudgetSettings::Get();   // never null
const int32 DefaultBudget = Settings.UpdateBudgetPerFrame;
```

---

## 13. Console commands

| Command | What it does |
| --- | --- |
| `Sense.Show [0\|1]` | Show or hide the counter box. No argument flips it. |
| `Sense.Budget <n>` | Set the per-frame ceiling. No argument prints it. |
| `Sense.Stats` | Print the measured counters to the log. |
| `Sense.Tiers` | List every perceiver: tier, distance, wait, next due, senses open/closed. |
| `Sense.Tiers 0` / `1` | Turn tiering off (everybody Live, no ceiling) or back on. |
| `Sense.Stress <n> [spread]` | Add *n* **synthetic** perceivers spread over *spread* cm. `Sense.Stress 0` clears them. |
| `Sense.Freeze [0\|1]` | Stop the scheduler where it stands. No argument flips it. |

Synthetic perceivers have no component behind them. They are ranked, tiered, counted and they **take
budget**, so the queue behaves exactly as it would with that many real guards — but nothing is issued to
them, because there is nothing to issue to. They are labelled `(synthetic)` on the box so a screenshot
cannot be mistaken for a real crowd.

Log category: `LogSenseBudget`.

---

## 14. The demo map

`/SenseBudget/SenseBudget/Maps/L_SenseBudgetDemo`

An open, lit area with sight blockers, viewed so that near and far guards are in frame at the same time.
Everything ships under `Content/SenseBudget/` in a single pack folder, so it can be deleted in one go
without touching the plugin code.

| Asset | What it is |
| --- | --- |
| `Maps/L_SenseBudgetDemo` | The demo level. Sets `BP_SenseBudgetDemoGameMode` as its GameMode override and holds the director. |
| `Blueprints/BP_SenseBudgetDemoGameMode` | HUD class `BP_SenseBudgetDemoHUD`, default pawn `BP_SenseBudgetWalker`. |
| `Blueprints/BP_SenseBudgetDemoHUD` | Derives from `ASenseBudgetHUD` — so the Canvas counter box draws — and creates `WBP_SenseBudgetPanel`. |
| `Blueprints/BP_SenseBudgetDemoDirector` | Places the crowd: spawns the patrol, elite and civilian guards across the area. |
| `Blueprints/BP_SenseGuardPatrol` | The ordinary guard: `AIPerceptionComponent` (sight + hearing) plus a `SenseBudgetComponent` on `DA_SenseProfile_Patrol`. **Colours itself by tier** through `M_SenseBudgetTier` — Live bright, Slow middling, Parked dark — so the tier is visible *in the scene*, not only in the box. |
| `Blueprints/BP_SenseGuardElite` | The same, on `DA_SenseProfile_Elite`: higher importance, faster rates, never parks by distance. |
| `Blueprints/BP_SenseGuardCivilian` | The same, on `DA_SenseProfile_Civilian`: low importance, parks early. |
| `Blueprints/BP_SenseBudgetWalker` | The player pawn you move the "nearest viewer" around with. |
| `UI/WBP_SenseBudgetPanel` | The buttons. Each one is a single call into `USenseBudgetStatics`: set the budget, fire a shot (`Nudge Senses Around` — the bystanders light up, which is the proof that Parked is not blind), and tiers off (`Set Sense Tiers Enabled (false)`, which is the before/after comparison). |
| `Profiles/DA_SenseProfile_{Patrol,Elite,Civilian}` | The three profiles. |
| `Materials/M_SenseBudgetTier` | The tier-coloured guard material. |
| `Materials/M_SenseBudgetGround`, `MI_SenseBudgetWalker` | Ground and player pawn materials. |

**What to look at.** Walk the pawn towards the crowd and watch the colours move with you: guards ahead of
you go bright, guards behind you dim. Drop the budget to 4 and the bright band narrows; raise it and it
widens. Press *tiers off* and everything goes bright at once — that is the unbudgeted baseline, and the
`sense ms` figure beside it is the only number that measures what the plugin is worth on your machine.

---

## 15. The maths, and its bounds

### The ceiling

`AssignTiers` returns exactly `min(Budget, eligible)` Live entries, where *eligible* means "not beyond its
own park distance". `SelectUpdates` returns at most `Budget` ids. Nothing anywhere grants an exemption.
This is unit tested at every budget from 1 to 30 and at budgets of 0, −7 and 500.

### The starvation bound

Crossing the starvation line does **not** get a perceiver served immediately. It gets it to the **back of
the queue of everybody else who has crossed**, ordered by lateness, and that queue drains `Budget` entries
per frame. So the real worst case is:

```
worst lateness  =  StarvationTrigger  +  Perceivers / (Budget × frame rate)
```

That is why `StarvationTriggerFraction` exists and why it defaults to **0.5** rather than 1.0: the trigger
has to sit below the promise with room for the queue to drain. Worked example, which is the case the test
covers:

```
Perceivers      500
Budget          12 per frame
Frame rate      60 Hz          →  720 updates per second
Promise         2.00 s
Trigger         2.00 × 0.5     =  1.00 s
Drain           500 / 720      =  0.69 s
Worst case      1.00 + 0.69    =  1.69 s      ✓ inside the 2.00 s promise
```

If your own numbers push that sum over the promise, the counter box tells you: the *overdue* figure turns
amber when it crosses the limit. The fix is a larger budget, not a larger promise.

### Why lateness and not waiting time

The rotation ranks on **how far past its own due time** a perceiver is, never on raw time-since-last-look.
A Parked guard on a three-second rate that is looked at every three seconds is exactly as well served as a
Live guard looked at every frame. Ranking on raw waiting time would punish the profiles that were honest
about needing less, and would make the box's amber warning fire on a perfectly healthy scene.

---

## 16. Tests

Six automation tests under `SenseBudget.*` in the Session Frontend, or:

```
UnrealEditor-Cmd.exe <YourProject>.uproject -ExecCmds="Automation RunTests SenseBudget;Quit" -unattended -nopause -testexit="Automation Test Queue Empty"
```

| Test | What it proves |
| --- | --- |
| `Maths.AssignTiersHoldsTheBudgetExactly` | The ceiling is exact at every budget from 1 to 30, and sane at 0, −7 and larger-than-the-crowd |
| `Maths.NearAndVisibleIsNeverParked` | A close, rendered perceiver is never Parked — even against 200 badly starved competitors — and takes the only slot when the budget is 1 |
| `Maths.NextDueRespectsTheProfileFloorRate` | `MaxIntervalSeconds` clamps every tier, and only ever makes a perceiver look sooner |
| `Maths.NobodyStarvesAtFiveHundredPerceivers` | 500 perceivers, budget 12, 900 simulated frames: the ceiling never breaks and nobody exceeds either the promise or the analytic bound |
| `Maths.ShouldWakeRespectsRadiusAndReason` | Wakes inside the radius, not outside, inclusive at the boundary; profiles can refuse reasons but not `Manual`/`Script`; a woken perceiver outranks a starved crowd |
| `Registry.UnregisterDuringAssignmentIsSafe` | Identity survives the pipeline, so a result for a perceiver that unregistered mid-frame is dropped rather than applied to a stranger; double-unregister, null and unknown components are all harmless |

---

## 17. Multiplayer, and what is measured

**Multiplayer.** The scheduler runs wherever the perceiver lives, which for AI is the server. Ranking uses
the local player controllers' cameras and pawns, so on a listen server it ranks against that machine's
player; on a dedicated server with several players it ranks against **whichever is nearest**, which is the
answer you want. With no player controllers at all, every perceiver counts as close — that keeps everybody
out of Parked, which errs towards nobody going blind. The budget still applies.

Nothing in the plugin replicates. There is no state a client needs: the tier a guard is in is a local
scheduling decision on the machine that owns the perceiver, and the stimuli it produces replicate exactly
as they did before.

**What `sense ms` measures.** The scheduler's own tick — gathering, ranking, assigning, and the
`SetSenseEnabled` / `RequestStimuliListenerUpdate` calls it issues — measured with `FPlatformTime` around
the work that was actually done that frame. It is split on the box into `rank` and `issue`.

**What it does not measure.** The engine's sight traces. Those run later, inside the perception system's
own tick, and there is no supported hook to time them from a plugin. The number that bounds them is
`updates this frame`: a listener that was not handed an update does not get traced for. If you want the
trace cost itself, `stat AI` and Unreal Insights already report it, and turning tiering off with
`Sense.Tiers 0` gives you the unbudgeted baseline to compare against on your own scene, on your own
machine.

That distinction is stated on the box, in this section, and in the store description, because a plugin
that lets a reader believe it measured something it did not is selling a number it does not have.

---

## 18. Supported platforms and engine versions

| | |
| --- | --- |
| **Engine version** | Unreal Engine **5.8** (`"EngineVersion": "5.8.0"`) |
| **Supported target build platforms** | **Win64** |
| **Supported development platforms** | **Win64** |
| **Module** | `SenseBudget` — `Runtime`, `LoadingPhase: PreDefault`, `PlatformAllowList: ["Win64"]` |
| **Build targets verified** | Editor Development and **Game Shipping**, both Win64, via `RunUAT BuildPlugin -Rocket -TargetPlatforms=Win64` |
| **Project types** | C++ and Blueprint-only (a Blueprint-only project builds the module on first open) |
| **Third-party code** | None. No GPL or otherwise restrictive dependencies. |

**Why Win64 only.** The plugin itself is platform-neutral C++ — there is no platform code, no intrinsic,
no OS call anywhere in it — but Win64 is the only platform it has actually been built and run on, and the
`PlatformAllowList` says exactly that rather than promising more. Everything it uses (`UCanvas`, `AHUD`,
`UAIPerceptionComponent`, `FPlatformTime`) is available on every platform Unreal ships for, so adding a
platform is a matter of extending the allow list and building — but the list is the honest record of what
has been tested, not a guess.

**Shipping builds.** Everything ships. There is no editor module, nothing behind `WITH_EDITOR`, and the
counter box is Canvas rather than a debug drawing call, so what a designer places in the editor is what
the packaged game runs — including the numbers.

---

## 19. Troubleshooting

**"My AI stopped noticing things."**
Check `Sense.Tiers` in the console. If a guard is Parked and its profile has a large
`ParkedIntervalSeconds` *and* a large `MaxIntervalSeconds`, it is looking rarely by design — lower
`MaxIntervalSeconds`. If it should never park at all, set its profile's `ParkDistance` to 0.

**"The overdue figure is amber."**
Your budget is too small for the crowd. Either raise `Update Budget Per Frame`, or lower
`Starvation Trigger Fraction`, or reduce the number of registered perceivers. The arithmetic is in
[§15](#15-the-maths-and-its-bounds).

**"The counter box does not appear."**
Your GameMode's HUD class is not `ASenseBudgetHUD`. Either set it, or turn on
`Auto Draw Stats On Any HUD`. Check `Sense.Show 1` too.

**"A guard registers but nothing happens."**
`SenseBudget` only manages senses that were **enabled when the component registered**. If a sense was
already disabled, it stays disabled — the plugin gives back exactly what it borrowed. Check the log for
`has a SenseBudgetComponent but no AIPerceptionComponent`.

**"`Sense.Stats` says there is no subsystem in this world."**
You are not in a game world. The subsystem exists for `Game`, `PIE` and `GamePreview` worlds only, never
for an editor world — there is nothing to schedule when nothing is ticking.

**"I disabled the plugin mid-game and everyone is blind."**
That cannot happen. `Unregister` and `Deinitialize` both re-enable every sense the plugin borrowed. If you
are seeing it, the senses were disabled by something else.

**"I need this off for a cutscene."**
`Set Sense Tiers Enabled (false)` — everybody goes Live and stays Live until you turn it back on.

**"I want to bisect a perception bug without removing the plugin."**
*Project Settings → Plugins → SenseBudget → Enabled* off. The subsystem stays alive and the box keeps
drawing, but nobody is tiered or throttled and every sense stays open.

---

© 2026 Silvan Teufel. All Rights Reserved.
Documentation: <https://wiki.teufel-engineering.com/en/SenseBudget/documentation>
Support: <mailto:teufelsilvan@gmail.com>
