// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SenseBudgetTypes.h"
#include "SenseBudgetStatics.generated.h"

class UAIPerceptionComponent;
class USensePriorityProfile;

/**
 * The maths, and the Blueprint entry points.
 *
 * The first four functions here are the whole decision this plugin makes, and none of them touches a
 * world, an actor, an audio device or a perception system. That is not tidiness for its own sake - it is
 * the test surface. RankPerceivers, AssignTiers, ShouldWake and NextDue are pure functions over plain
 * structs, so the ordering, the ceiling, the wake rule and the floor rate are unit tested with five
 * hundred entries and no engine running. The subsystem calls exactly these functions; there is no second
 * copy of the logic that could drift from the one the tests cover.
 *
 * The remaining functions are the ones a Blueprint calls: one node per button on the demo panel.
 */
UCLASS(meta = (DisplayName = "Sense Budget Statics"))
class SENSEBUDGET_API USenseBudgetStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	//~ The maths --------------------------------------------------------------------------------------

	/**
	 * Sort perceivers into the order they should be served in, best first.
	 *
	 * The score is
	 *
	 *     Importance * (Distance term + Visibility bonus)      <- who matters
	 *   + WaitWeight * OverdueSeconds                          <- gentle rotation
	 *   + StarvationBonus + OverdueSeconds (above the line)    <- nobody starves
	 *   + WakeBonus                        (event pending)     <- Parked is not blind
	 *
	 * The last three terms use lateness, not time since the last look. A Parked guard on a three-second
	 * rate that is looked at every three seconds is exactly as well served as a Live guard looked at
	 * every frame, and ranking on raw waiting time would punish the profiles that were honest about
	 * needing less. WaitSeconds is carried on the entry for the counter box and is not read here.
	 *
	 * Without the starvation term the same twelve guards would be Live forever and the other eighty-four
	 * would fall further and further behind; with it, anybody past the line jumps the whole queue, oldest
	 * first, and the worst case becomes a number you can write down. Ties are broken by Id so the order is
	 * stable frame to frame and a screenshot means the same thing twice.
	 *
	 * Entries with a negative importance, a negative lateness or a NaN distance are treated as zero rather
	 * than sorted to a random place: bad data should make somebody dull, never make somebody vanish.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Maths")
	static TArray<FSenseRankEntry> RankPerceivers(const TArray<FSenseRankEntry>& Entries, const FSenseRankWeights& Weights);

	/**
	 * Hand out tiers to an already-ranked array. Exactly min(Budget, eligible) entries come back Live.
	 *
	 * Walking the ranked list once: the first Budget entries that are not beyond their own park distance
	 * become Live, anything beyond its park distance becomes Parked whatever its rank, and everything else
	 * becomes Slow. A close, visible perceiver therefore cannot be parked while there is budget left -
	 * it is at the front of the list and it is not beyond its park distance, so it takes a Live slot.
	 *
	 * Note what does NOT happen here: nothing gets a Live slot for free. A perceiver woken by a shot got
	 * its head start in the ranking, not here, which is what keeps the ceiling exact - the promise is
	 * "never more than Budget", and an exception carved out for waking would quietly break it in the one
	 * situation (a firefight) where the frame is already busiest.
	 *
	 * A Budget of zero or less parks the distant ones and leaves the rest Slow; it does not blind anybody,
	 * because the profile's floor rate still applies downstream in NextDue.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Maths")
	static TArray<FSenseRankEntry> AssignTiers(const TArray<FSenseRankEntry>& Ranked, int32 Budget);

	/**
	 * Would this event pull this perceiver back to Live?
	 *
	 * True when the profile allows the reason and the event happened inside the profile's wake radius.
	 * Manual and Script are always allowed - the two reasons a human typed out are the two that must
	 * always work.
	 *
	 * A null profile returns false. The subsystem substitutes its own default profile before anything
	 * reaches here, so a null at this point means somebody is calling the maths directly with nothing to
	 * apply, and inventing a rule would be worse than saying no.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Maths")
	static bool ShouldWake(const USensePriorityProfile* Profile, ESenseWakeReason Reason, float Distance);

	/**
	 * The absolute time at which a perceiver in this tier is next allowed a look.
	 *
	 * Live returns Now - every frame, subject to the budget. Slow and Parked return Now plus the profile's
	 * interval for that tier, already clamped to the profile's floor rate, so no tier can ever schedule a
	 * look further ahead than MaxIntervalSeconds. That clamp is the reason Parked is not blind.
	 *
	 * A null profile falls back to a half-second slow rate and a three-second parked rate, which are the
	 * profile defaults - a perceiver with no profile is dulled, never switched off.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Maths")
	static float NextDue(ESenseTier Tier, const USensePriorityProfile* Profile, float Now);

	/**
	 * Which perceivers get their update this frame: ranked order, due now, up to Budget of them.
	 *
	 * Live counts as due whatever its stored time says - that is what Live means - so a perceiver just
	 * promoted from Slow does not sit out the remainder of the interval it was given while it was still
	 * unimportant.
	 *
	 * Returns Ids, not indices, and only Ids that were in the input. The subsystem looks each one back up
	 * in its registry and drops any that are no longer there, which is what makes unregistering a
	 * component in the middle of an assignment harmless.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Maths")
	static TArray<int32> SelectUpdates(const TArray<FSenseRankEntry>& Tiered, int32 Budget, float Now);

	//~ Blueprint entry points -------------------------------------------------------------------------

	/** Everything the counter box shows, for a widget that wants to show it differently. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static FSenseBudgetStats GetSenseBudgetStats(const UObject* WorldContextObject);

	/** Set the per-frame ceiling. This is what a "Budget 4 / 12 / 96" button calls. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static void SetSenseBudget(const UObject* WorldContextObject, int32 UpdatesPerFrame);

	/** Read the per-frame ceiling back. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static int32 GetSenseBudget(const UObject* WorldContextObject);

	/**
	 * An event happened here - pull everybody nearby back to Live. This is what a "fire a shot" button calls.
	 *
	 * Returns how many perceivers were actually woken, so a demo can print it and a designer can tell the
	 * difference between "nothing was in range" and "the wake rule is broken".
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static int32 NudgeSensesAround(const UObject* WorldContextObject, FVector Location, float Radius, ESenseWakeReason Reason);

	/**
	 * Turn tiering off entirely: everybody Live, every frame, no ceiling.
	 *
	 * The comparison mode. It exists so the claim can be checked rather than believed - flip it, watch the
	 * measured milliseconds on the counter box, flip it back.
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static void SetSenseTiersEnabled(const UObject* WorldContextObject, bool bEnabled);

	/** Whether tiering is on. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static bool AreSenseTiersEnabled(const UObject* WorldContextObject);

	/** Stop the scheduler where it stands, for a screenshot or a breakpoint. Nothing is reassigned. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static void SetSenseBudgetFrozen(const UObject* WorldContextObject, bool bFrozen);

	/** Show or hide the counter box. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static void SetSenseStatsVisible(const UObject* WorldContextObject, bool bVisible);

	/** The tier this perception component is currently in. Parked for anything not registered. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	static ESenseTier GetTierForPerception(const UAIPerceptionComponent* Perception);

	/**
	 * The colour a demo actor paints itself with: Live bright, Slow middling, Parked dark.
	 *
	 * On the box the tiers are three numbers; in the map they have to be three brightnesses, otherwise a
	 * screenshot of ninety-six guards proves nothing at all.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	static FLinearColor GetTierColor(ESenseTier Tier);

	/** "Live" / "Slow" / "Parked", for a label. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	static FText GetTierDisplayName(ESenseTier Tier);
};
