// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "SenseBudgetTypes.h"
#include "SenseBudgetComponent.generated.h"

class UAIPerceptionComponent;
class USensePriorityProfile;

/** Fired when this perceiver moves between tiers. A demo guard recolours itself here instead of polling. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSenseBudgetTierChanged, ESenseTier, NewTier, ESenseTier, OldTier);

/**
 * Hangs on an AI actor next to its UAIPerceptionComponent and hands that component to the scheduler.
 *
 * Everything a project has to do to use this plugin is on this component: drop it on the pawn, point it
 * at a profile, done. It finds the perception component on its own owner, registers it on BeginPlay,
 * unregisters on EndPlay, answers "which tier am I in" and "how long since I last looked" for a
 * Blueprint, and forwards events into the wake rule.
 *
 * It deliberately owns no scheduling logic of its own. Every decision is made once, in the subsystem, for
 * everybody at once - a per-actor component that decided its own rate would be exactly the thing this
 * plugin exists to replace, because a hundred independent local decisions do not add up to a ceiling.
 */
UCLASS(ClassGroup = (AI), meta = (BlueprintSpawnableComponent, DisplayName = "Sense Budget"))
class SENSEBUDGET_API USenseBudgetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USenseBudgetComponent();

	//~ UActorComponent interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//~ Configuration ------------------------------------------------------------------------------------

	/**
	 * What kind of perceiver this is: importance, rates, park distance, what wakes it.
	 *
	 * Left empty, the subsystem substitutes its own default profile - ordinary importance, half-second
	 * slow rate, five-second floor - rather than refusing to manage the perceiver. A guard nobody got
	 * round to configuring should end up ordinary, not unmanaged and unbudgeted.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	TObjectPtr<USensePriorityProfile> Profile;

	/** Register with the scheduler on BeginPlay. Off means a Blueprint will call RegisterWithBudget itself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	bool bAutoRegister = true;

	/**
	 * The perception component to pace.
	 *
	 * Left empty, the first UAIPerceptionComponent on the owner is used, which is what almost every actor
	 * has. Set it explicitly only for the rare actor with more than one.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget")
	TObjectPtr<UAIPerceptionComponent> PerceptionOverride;

	//~ Events -------------------------------------------------------------------------------------------

	/** Fired when this perceiver changes tier. */
	UPROPERTY(BlueprintAssignable, Category = "SenseBudget")
	FSenseBudgetTierChanged OnTierChanged;

	//~ Registration -------------------------------------------------------------------------------------

	/** Hand this actor's perception component to the scheduler. Safe to call twice. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	bool RegisterWithBudget();

	/** Take it back off the scheduler, senses restored. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	void UnregisterFromBudget();

	/**
	 * Whether the scheduler is currently pacing this perceiver.
	 *
	 * NOT called IsRegistered(). UActorComponent::IsRegistered() already exists and means something else
	 * entirely - whether the component itself is registered with the world - and shadowing it produces a
	 * component that answers the wrong question to every piece of engine code that asks. Learned the hard
	 * way on an earlier plugin; the name is long on purpose.
	 */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	bool IsManagedBySenseBudget() const;

	//~ Reading ------------------------------------------------------------------------------------------

	/** The tier this perceiver is in right now. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	ESenseTier GetTier() const;

	/** Seconds since the scheduler last issued a perception update for this perceiver. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	float GetSecondsSinceLastUpdate() const;

	/** The colour for this perceiver's tier: Live bright, Slow middling, Parked dark. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	FLinearColor GetTierDisplayColor() const;

	/** The perception component being paced, resolved the same way registration resolves it. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	UAIPerceptionComponent* GetManagedPerception() const;

	//~ Waking -------------------------------------------------------------------------------------------

	/**
	 * Something happened to this perceiver - pull it back to Live if its profile agrees.
	 *
	 * Distance is how far away the event was; pass zero for something that happened to this actor itself,
	 * such as taking damage. Parked is not blind, and this is the door.
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	bool WakeUp(ESenseWakeReason Reason = ESenseWakeReason::Manual, float Distance = 0.0f);

	/** Called by the subsystem when this perceiver's tier changes. Not for Blueprint to call. */
	void NotifyTierChanged(ESenseTier NewTier, ESenseTier OldTier);

private:
	/** The tier last reported, so the delegate fires on changes rather than every frame. */
	ESenseTier LastReportedTier = ESenseTier::Slow;

	/** Whether RegisterWithBudget succeeded, so EndPlay knows whether there is anything to undo. */
	bool bManaged = false;
};
