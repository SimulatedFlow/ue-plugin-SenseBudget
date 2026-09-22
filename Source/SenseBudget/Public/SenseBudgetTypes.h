// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SenseBudgetTypes.generated.h"

/**
 * What a perceiver is allowed to do this frame.
 *
 * Three tiers instead of on and off. The whole point of the middle one is that there is a middle one:
 * a guard behind the player does not stop perceiving, it perceives less often, and it can be pulled
 * back to the front by an event at any moment.
 */
UENUM(BlueprintType)
enum class ESenseTier : uint8
{
	/** Looks every frame. Close, visible or important - the AI the player is actually interacting with. */
	Live		UMETA(DisplayName = "Live"),

	/** Looks at the profile's slow rate, 0.5 s by default. Still perceives, just not every frame. */
	Slow		UMETA(DisplayName = "Slow"),

	/**
	 * Looks rarely, and on events.
	 *
	 * PARKED IS NOT BLIND. A parked perceiver still gets a look at the profile's floor rate, and any
	 * event inside its wake radius - a shot, a shout, damage - pulls it straight back to Live on the
	 * next tick. Without that rule this tier would just be switching perception off with a nicer name,
	 * and the first thing a player would notice is an enemy that never saw them coming.
	 */
	Parked		UMETA(DisplayName = "Parked")
};

/** Why somebody is being pulled back to Live. Passed to ShouldWake so a profile can refuse some reasons. */
UENUM(BlueprintType)
enum class ESenseWakeReason : uint8
{
	/** A noise was made nearby - a shot, a footstep, a shout. */
	Noise		UMETA(DisplayName = "Noise"),

	/** The perceiver's owner took damage. */
	Damage		UMETA(DisplayName = "Damage"),

	/** Somebody asked for it by hand, from Blueprint or from a console command. Always honoured. */
	Manual		UMETA(DisplayName = "Manual"),

	/** Scripted - a cutscene, a trigger volume, a level sequence. Always honoured. */
	Script		UMETA(DisplayName = "Script")
};

/**
 * The weights the ranking uses. One struct so a project can tune the order without touching code, and so
 * the ranking stays a pure function of its arguments - it never reads settings behind your back, which is
 * what makes it testable.
 */
USTRUCT(BlueprintType)
struct SENSEBUDGET_API FSenseRankWeights
{
	GENERATED_BODY()

	/** How much being close counts. The distance term is DistanceWeight / (1 + Distance / ReferenceDistance). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float DistanceWeight = 1.0f;

	/** The distance at which the distance term has fallen to half. Centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "1.0"))
	float ReferenceDistance = 1500.0f;

	/**
	 * Added when the perceiver was recently rendered.
	 *
	 * On screen is not the same as close: a sniper on a roof four thousand centimetres away is the one
	 * the player is looking at, and a guard two metres behind a wall is not.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float VisibilityBonus = 0.75f;

	/** Multiplies the distance and visibility terms by the profile's importance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float ImportanceWeight = 1.0f;

	/** Score added per second overdue, below the starvation line. The gentle half of the rotation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float WaitWeight = 0.5f;

	/**
	 * Where the starvation line sits, in seconds. Note: the TRIGGER, not the promise.
	 *
	 * Above this a perceiver stops competing on distance and importance and jumps ahead of everything
	 * below the line, oldest first. That is the term that turns "probably fair" into a bound you can
	 * write down - and the bound is not this number, it is
	 *
	 *     StarvationTriggerSeconds + Perceivers / (Budget * frame rate)
	 *
	 * because crossing the line puts you at the back of the queue of everybody else who has crossed it,
	 * and that queue drains Budget entries per frame. So the trigger has to sit BELOW the limit a project
	 * was promised, with room for the queue to drain. The subsystem sets it to half of the settings'
	 * StarvationSeconds for exactly that reason: at five hundred perceivers, a budget of twelve and 60 Hz
	 * the drain is 0.69 s, so a trigger at 1.0 s keeps the worst case at 1.7 s under a promise of 2.0 s.
	 *
	 * If that sum ever exceeds the promise, the counter box says so - the longest-wait line turns amber
	 * when it goes over the limit, which means the budget is too small for the crowd.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.01"))
	float StarvationTriggerSeconds = 1.0f;

	/** Score added on crossing the starvation line. Large enough that no ordinary score can outbid it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float StarvationBonus = 1000.0f;

	/** Score added while a wake event is pending, so a woken perceiver goes to the front of the queue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float WakeBonus = 5000.0f;
};

/**
 * One perceiver, reduced to the numbers the ranking needs.
 *
 * Nothing in here is a pointer and nothing in here needs a world. That is deliberate: the ranking, the
 * tier assignment and the selection of who updates this frame are the three places this plugin can be
 * quietly wrong, and all three are pure functions over arrays of this struct. They are unit tested with
 * five hundred entries and no engine running.
 */
USTRUCT(BlueprintType)
struct SENSEBUDGET_API FSenseRankEntry
{
	GENERATED_BODY()

	/**
	 * Stable identity of the perceiver, handed out by the subsystem at registration.
	 *
	 * Ranking sorts the array, so an index means nothing after the first stage. Everything downstream -
	 * including the subsystem writing results back onto live components - looks the perceiver up by this
	 * id. That is what makes unregistering during an assignment harmless: the id is simply no longer in
	 * the registry and the result is dropped, rather than landing on whoever moved into that slot.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	int32 Id = INDEX_NONE;

	/** Distance in centimetres to the nearest viewer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float DistanceToViewer = 0.0f;

	/** Whether the perceiver's owner was drawn recently. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	bool bRecentlyRendered = false;

	/** Importance from the profile. 1.0 is ordinary; a boss is 4.0; a background prop is 0.25. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float Importance = 1.0f;

	/**
	 * Seconds since this perceiver last had a perception update issued for it.
	 *
	 * Display only - the counter box and the log read this, the ranking does not. A perceiver whose
	 * profile asked to be looked at every three seconds and is being looked at every three seconds has a
	 * wait of three seconds and is not being neglected at all, so ranking on this number would punish the
	 * profiles that were honest about needing less.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float WaitSeconds = 0.0f;

	/**
	 * Seconds past the time this perceiver's own profile said it was due. Zero if it is on schedule.
	 *
	 * THIS is what the rotation is built on, and it is the difference between a queue that is fair and a
	 * queue that merely looks fair. Lateness is the only thing the budget is responsible for: a Parked
	 * guard on a three-second rate that gets looked at every three seconds is never late, however long
	 * its wait reads, while a Live guard that misses two frames is late by two frames and should climb.
	 * The promise on the counter box - nobody more than the starvation limit overdue - is a promise about
	 * this number.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float OverdueSeconds = 0.0f;

	/** Beyond this distance the perceiver is Parked rather than Slow. Zero or less means never park. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget", meta = (ClampMin = "0.0"))
	float ParkDistance = 0.0f;

	/** Absolute world time at which this perceiver is next allowed an update. Filled in by NextDue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	float NextDueTime = 0.0f;

	/** A wake event is pending. Set by Nudge, cleared once the perceiver has been served. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	bool bWakePending = false;

	/** Output of RankPerceivers. Higher goes first. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float Score = 0.0f;

	/** Output of AssignTiers. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	ESenseTier Tier = ESenseTier::Slow;
};

/** One line of the "waiting longest" list on the counter box. */
USTRUCT(BlueprintType)
struct SENSEBUDGET_API FSenseWaitEntry
{
	GENERATED_BODY()

	/** The owning actor's name, or a synthetic label for stress-test perceivers. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	FString Label;

	/** Seconds since this perceiver last had an update issued. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float WaitSeconds = 0.0f;

	/** How far past its own due time it is. Zero means it is being served exactly as its profile asked. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float OverdueSeconds = 0.0f;

	/** The tier it currently sits in. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	ESenseTier Tier = ESenseTier::Slow;
};

/**
 * Everything the counter box draws, and everything Sense.Stats prints.
 *
 * Measured, not estimated. The milliseconds are read from FPlatformTime around the work that was actually
 * done this frame, and the counts are the counts of that same frame. A budget plugin that cannot show a
 * measured number on a screenshot has not proved anything.
 */
USTRUCT(BlueprintType)
struct SENSEBUDGET_API FSenseBudgetStats
{
	GENERATED_BODY()

	/** How many perceivers are registered right now. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 Perceivers = 0;

	/** How many perception updates were issued this frame. Never above Budget while tiers are on. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 UpdatesThisFrame = 0;

	/** The per-frame ceiling those updates were measured against. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 Budget = 0;

	/** How many perceivers wanted an update this frame, before the ceiling was applied. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 DueThisFrame = 0;

	/** How many of those were pushed to a later frame. Nobody is dropped; this is the queue. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 DeferredThisFrame = 0;

	/** Perceivers in each tier. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 LiveCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 SlowCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 ParkedCount = 0;

	/** The worst time-since-last-look among all registered perceivers, in seconds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float LongestWaitSeconds = 0.0f;

	/**
	 * The worst lateness among all registered perceivers: how far past its own due time the most
	 * neglected perceiver is. This is the number the promise is about, and the one that turns amber.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float MostOverdueSeconds = 0.0f;

	/** The starvation limit the lateness is measured against. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float StarvationSeconds = 0.0f;

	/**
	 * Measured milliseconds of the whole SenseBudget tick: gathering, ranking, assigning tiers and
	 * issuing this frame's perception updates.
	 *
	 * This is the scheduler's own cost plus the cost of the listener updates it issues. It is NOT the
	 * cost of the engine's sight traces - those happen later, inside the perception system's own tick,
	 * and the number that bounds them is UpdatesThisFrame. Said plainly on the box and in the docs,
	 * because a plugin that lets a reader think otherwise is selling a number it did not measure.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float SenseMilliseconds = 0.0f;

	/** The ranking and tier-assignment half of SenseMilliseconds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float RankMilliseconds = 0.0f;

	/** The issuing half of SenseMilliseconds - the calls into UAIPerceptionComponent. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	float IssueMilliseconds = 0.0f;

	/** Wake events honoured this frame. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int32 WakesThisFrame = 0;

	/** Perception updates issued since the world started. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	int64 TotalUpdates = 0;

	/** False while the comparison mode is on: no tiers, no ceiling, everybody every frame. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	bool bTiersEnabled = true;

	/** True while the scheduler is frozen, for a screenshot or a breakpoint. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	bool bFrozen = false;

	/** The five perceivers that have waited longest, worst first. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SenseBudget")
	TArray<FSenseWaitEntry> LongestWaits;
};
