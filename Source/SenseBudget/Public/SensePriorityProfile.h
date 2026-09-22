// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SenseBudgetTypes.h"
#include "SensePriorityProfile.generated.h"

/**
 * Per enemy type: how much this kind of perceiver matters, how slow it is allowed to get, and what wakes
 * it up again.
 *
 * A data asset rather than properties on the component, because the answer is almost never per instance.
 * A project has three or four kinds of AI - a patrolling grunt, an elite, a turret, a civilian - and every
 * instance of a kind wants the same answer. Sharing one asset also means a designer can retune every
 * grunt in the game at once, which is what actually happens the week before a milestone.
 *
 * Every rate here is a ceiling on laziness, not a promise of speed. The scheduler may serve a perceiver
 * sooner than its rate if there is budget going spare; it will never serve it later than
 * MaxIntervalSeconds, in any tier, for any reason.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Sense Priority Profile"))
class SENSEBUDGET_API USensePriorityProfile : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	USensePriorityProfile();

	//~ UPrimaryDataAsset interface
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;

	//~ Importance ---------------------------------------------------------------------------------------

	/**
	 * How much this kind of perceiver matters, relative to the others.
	 *
	 * Multiplies the distance and visibility terms of the ranking, so an elite at four thousand
	 * centimetres can outrank a grunt at two thousand. It does not multiply the waiting term - importance
	 * decides who goes first, it does not decide who is allowed to starve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Priority", meta = (ClampMin = "0.0", UIMax = "8.0"))
	float Importance = 1.0f;

	//~ Rates --------------------------------------------------------------------------------------------

	/** Seconds between looks in the Slow tier. Half a second is a good default for a humanoid guard. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rates", meta = (ClampMin = "0.0", UIMax = "5.0", Units = "Seconds"))
	float SlowIntervalSeconds = 0.5f;

	/** Seconds between looks in the Parked tier. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rates", meta = (ClampMin = "0.0", UIMax = "30.0", Units = "Seconds"))
	float ParkedIntervalSeconds = 3.0f;

	/**
	 * The floor rate. No tier ever schedules a look further ahead than this.
	 *
	 * This is the number that makes Parked mean "rarely" instead of "never". Set ParkedIntervalSeconds to
	 * thirty and this to five, and the perceiver still gets a look every five seconds - so a guard that
	 * has been standing in a corner for a minute still notices the corpse that appeared in front of it,
	 * without anybody having to remember to fire an event.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rates", meta = (ClampMin = "0.05", UIMax = "30.0", Units = "Seconds"))
	float MaxIntervalSeconds = 5.0f;

	//~ Distance -----------------------------------------------------------------------------------------

	/**
	 * Beyond this distance from the nearest viewer, this perceiver is Parked rather than Slow.
	 *
	 * Zero or less means never park by distance - use that for anything that has to keep reacting no
	 * matter where the player is, such as an objective guard or a scripted ambush.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Distance", meta = (ClampMin = "0.0", UIMax = "50000.0", Units = "Centimeters"))
	float ParkDistance = 6000.0f;

	//~ Waking -------------------------------------------------------------------------------------------

	/** A noise inside WakeRadius pulls this perceiver back to Live. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Waking")
	bool bWakeOnNoise = true;

	/** Damage to this perceiver's owner pulls it back to Live. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Waking")
	bool bWakeOnDamage = true;

	/**
	 * How far away an event still counts as "next to me".
	 *
	 * A shot inside this radius pulls a parked perceiver straight to Live. Set it generously: the cost of
	 * waking somebody who did not need it is one perception update, and the cost of not waking somebody
	 * who did is a player walking past an enemy that never looked up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Waking", meta = (ClampMin = "0.0", UIMax = "20000.0", Units = "Centimeters"))
	float WakeRadius = 2500.0f;

	//~ Helpers ------------------------------------------------------------------------------------------

	/** The scheduled gap for a tier, already clamped to MaxIntervalSeconds. Live is always zero. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Profile")
	float GetIntervalForTier(ESenseTier Tier) const;

	/** Whether this profile allows the given reason to wake a perceiver at all, ignoring distance. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|Profile")
	bool AllowsWakeReason(ESenseWakeReason Reason) const;
};
