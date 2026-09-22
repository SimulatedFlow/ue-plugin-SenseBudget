// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SenseBudgetTypes.h"
#include "SenseBudgetSettings.generated.h"

/**
 * Project-wide defaults for SenseBudget, under Project Settings -> Plugins -> SenseBudget.
 *
 * Everything here is what a project decides once: what one frame may cost, where the tier boundaries sit
 * for perceivers without a profile, and how long anybody is ever allowed to wait. What a particular kind
 * of enemy wants lives on its USensePriorityProfile, because that is what a designer retunes all
 * afternoon.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "SenseBudget"))
class SENSEBUDGET_API USenseBudgetSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USenseBudgetSettings();

	//~ UDeveloperSettings interface
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;

	/** The settings object, never null. */
	static const USenseBudgetSettings& Get();

	//~ Master switch ------------------------------------------------------------------------------------

	/**
	 * Run the scheduler at all.
	 *
	 * Off leaves the subsystem alive - components still register, the counter box still draws, the stats
	 * are still gathered - but no perceiver is ever tiered or throttled. Everybody's senses are left
	 * enabled and the engine schedules them as it always did. That is the honest fallback, and it is what
	 * a project wants while it is bisecting a perception problem.
	 */
	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bEnabled = true;

	//~ Budget -------------------------------------------------------------------------------------------

	/**
	 * The hard ceiling: how many perception updates may be issued in one frame.
	 *
	 * Twelve by default. A hundred guards with sight and hearing are, with no ceiling, a hundred listeners
	 * asking the sight sense for traces in the same frame - several hundred rays, whether or not the
	 * player is anywhere near. With this at twelve it is twelve updates, and the rest wait no longer than
	 * the number the counter box is showing.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1", UIMax = "128"))
	int32 UpdateBudgetPerFrame = 12;

	/**
	 * The promise: nobody waits longer than this, in seconds. Shown on the counter box next to the
	 * measured longest wait, so the promise and the reality are always side by side.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0.05", UIMax = "10.0", Units = "Seconds"))
	float StarvationSeconds = 2.0f;

	/**
	 * How far below the promise the ranking actually starts jumping people up the queue.
	 *
	 * Crossing the line does not get you served immediately - it gets you to the back of the queue of
	 * everybody else who has crossed it, and that queue drains UpdateBudgetPerFrame entries per frame. So
	 * the real worst case is
	 *
	 *     StarvationSeconds * this fraction + Perceivers / (UpdateBudgetPerFrame * frame rate)
	 *
	 * and the trigger has to leave room for the second term. At half, with five hundred perceivers, a
	 * budget of twelve and 60 Hz, the drain is 0.69 s and the worst case lands at 1.7 s under a promise
	 * of 2.0 s. Raise it towards 1.0 for a small cast where the drain is negligible; lower it if the
	 * counter box shows the longest wait going over the limit.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float StarvationTriggerFraction = 0.5f;

	/** The weights the ranking uses. The starvation trigger above is copied into these at startup. */
	UPROPERTY(config, EditAnywhere, Category = "Budget")
	FSenseRankWeights RankWeights;

	//~ Tiers --------------------------------------------------------------------------------------------

	/**
	 * Park distance for a perceiver whose profile does not set one, in centimetres.
	 *
	 * Used when a component registers without a profile, or when a profile leaves ParkDistance at zero
	 * and the project would rather have a distance than none.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tiers", meta = (ClampMin = "0.0", UIMax = "50000.0", Units = "Centimeters"))
	float DefaultParkDistance = 6000.0f;

	/**
	 * How long a Slow or Parked perceiver's senses stay enabled after it has been served, in seconds.
	 *
	 * Not zero, and not one frame, for a mechanical reason worth knowing: the engine's sight sense
	 * processes a bounded number of traces per tick, so a listener that is enabled and disabled inside a
	 * single frame may never actually get its trace. Holding the window open for a few frames means the
	 * look that was scheduled is a look that happened. Fifty milliseconds is about three frames at 60 Hz.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tiers", meta = (ClampMin = "0.0", UIMax = "1.0", Units = "Seconds"))
	float SenseWindowSeconds = 0.05f;

	/** Tolerance passed to AActor::WasRecentlyRendered when deciding whether a perceiver is on screen. */
	UPROPERTY(config, EditAnywhere, Category = "Tiers", meta = (ClampMin = "0.0", UIMax = "1.0", Units = "Seconds"))
	float RecentlyRenderedTolerance = 0.2f;

	//~ Counter box --------------------------------------------------------------------------------------

	/** Draw the counter box from the first frame. Sense.Show flips it. */
	UPROPERTY(config, EditAnywhere, Category = "Counter Box")
	bool bShowStatsByDefault = true;

	/**
	 * Draw the counter box even when the project's HUD is not an ASenseBudgetHUD.
	 *
	 * A project that already has its own HUD class does not have to reparent it: turn this on and the same
	 * box is drawn through AHUD::OnHUDPostRender instead. The two paths know about each other and cannot
	 * stack.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Counter Box")
	bool bAutoDrawStatsOnAnyHUD = false;

	/** How many of the longest-waiting perceivers the box lists under the counters. */
	UPROPERTY(config, EditAnywhere, Category = "Counter Box", meta = (ClampMin = "0", UIMax = "16"))
	int32 LongestWaitRows = 5;
};
