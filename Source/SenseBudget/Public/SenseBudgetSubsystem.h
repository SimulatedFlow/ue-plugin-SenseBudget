// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SenseBudgetTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/SubclassOf.h"
#include "SenseBudgetSubsystem.generated.h"

class AHUD;
class UAIPerceptionComponent;
class UAISense;
class UCanvas;
class USenseBudgetComponent;
class USensePriorityProfile;

/**
 * Live state of one registered perceiver.
 *
 * Deliberately not a USTRUCT: none of this is saved, replicated, edited or shown in a details panel, and
 * making it reflected would only invite somebody to serialise a frame-local scheduling decision. The
 * object pointers are all weak - a perceiver belongs to its actor, and this registry must never be the
 * reason an actor cannot be garbage collected.
 */
struct FSensePerceiverState
{
	/** Stable id. Everything downstream refers to a perceiver by this, never by an array index. */
	int32 Id = INDEX_NONE;

	TWeakObjectPtr<UAIPerceptionComponent> Perception;
	TWeakObjectPtr<USensePriorityProfile> Profile;
	TWeakObjectPtr<USenseBudgetComponent> Notify;
	TWeakObjectPtr<AActor> Owner;

	/** Where this perceiver currently sits. */
	ESenseTier Tier = ESenseTier::Slow;

	/** World time of the last update actually issued for this perceiver. */
	float LastUpdateTime = 0.0f;

	/** World time at which it may next be served. */
	float NextDueTime = 0.0f;

	/** World time at which its senses are closed again, if it is not Live. */
	float WindowCloseTime = 0.0f;

	/** Whether its senses are currently enabled on the perception component. */
	bool bSensesOpen = true;

	/** A wake event is pending and has not been served yet. */
	bool bWakePending = false;

	/**
	 * The senses that were enabled on this component when it registered.
	 *
	 * Only these are ever toggled, and on unregister exactly these are switched back on. A project that
	 * had deliberately disabled hearing on one guard before registering gets that guard back with hearing
	 * still disabled, instead of the plugin quietly handing back a different configuration than it
	 * borrowed. Sense classes are class objects and are always referenced by the engine, so holding them
	 * here cannot keep anything alive that would otherwise be collected.
	 */
	TArray<TSubclassOf<UAISense>> ManagedSenses;

	/** A stress-test perceiver with no component behind it. Ranked and counted, never issued to. */
	bool bSynthetic = false;

	/** Distance used for synthetic perceivers, which have no owner to measure from. */
	float SyntheticDistance = 0.0f;

	/** Name shown on the counter box's "waiting longest" list. */
	FString Label;
};

/**
 * The scheduler. Decides who is allowed to perceive this frame.
 *
 * THE NUMBER THIS EXISTS FOR. A hundred guards with sight and hearing are, with no shared ceiling, a
 * hundred listeners handed to the sight sense in the same frame - several hundred line traces, and they
 * cost the same whether the player is standing in front of them or two districts away. AIPerception gives
 * each sense its own update rate, but there is no rate across all perceivers, so the cost scales with how
 * many enemies exist rather than with how many matter. With a budget of 12 it is twelve perception
 * updates per frame, and nobody waits longer than the number on the counter box - by default two seconds
 * plus however long it takes to drain the queue, which at ninety-six perceivers and twelve a frame is
 * eight frames.
 *
 * The important half of that is what it is NOT. Whoever misses out this frame is not switched off; they
 * move up the queue because waiting adds to their score, and they go next. Nobody drops out, everybody
 * slows down, and whoever is close and important does not slow down at all. Switching distant perception
 * off is the usual workaround and it is why an enemy walks into the room without having seen anybody
 * coming.
 *
 * Each frame the subsystem gathers plain numbers for every perceiver, hands them to the pure functions in
 * USenseBudgetStatics - RankPerceivers, AssignTiers, SelectUpdates - and writes the answers back by id.
 * There is no second copy of the decision anywhere; what ships is what the tests cover.
 */
UCLASS(meta = (DisplayName = "Sense Budget Subsystem"))
class SENSEBUDGET_API USenseBudgetSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	//~ UWorldSubsystem interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	//~ FTickableGameObject interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The subsystem for this world, or null outside a game world. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget", meta = (WorldContext = "WorldContextObject"))
	static USenseBudgetSubsystem* Get(const UObject* WorldContextObject);

	//~ Registration -------------------------------------------------------------------------------------

	/**
	 * Take over the pacing of this perception component.
	 *
	 * Returns the perceiver's stable id, or INDEX_NONE if the component was null or already registered.
	 * A null profile is replaced with the subsystem's default profile rather than refused: a perceiver
	 * somebody forgot to configure should end up ordinary, not unmanaged.
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	int32 Register(UAIPerceptionComponent* Perception, USensePriorityProfile* Profile, USenseBudgetComponent* Notify = nullptr);

	/**
	 * Stop pacing this component, and put its senses back the way they were found.
	 *
	 * Restoring the senses is not politeness, it is correctness: a component whose sight was closed for a
	 * Slow window and which is then unregistered - because the actor is being destroyed, or the plugin is
	 * being switched off mid-game - would otherwise be left permanently blind by the plugin that promised
	 * never to blind anybody.
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	void Unregister(UAIPerceptionComponent* Perception);

	/** Whether this component is currently being paced. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	bool IsPerceiverManaged(const UAIPerceptionComponent* Perception) const;

	//~ Budget -------------------------------------------------------------------------------------------

	/** Set the per-frame ceiling. Clamped to at least one: a budget of zero would be an off switch. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	void SetBudget(int32 UpdatesPerFrame);

	/** The per-frame ceiling. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	int32 GetBudget() const { return UpdateBudgetPerFrame; }

	//~ Waking -------------------------------------------------------------------------------------------

	/**
	 * An event concerns this perceiver - pull it back to Live if its profile agrees.
	 *
	 * PARKED IS NOT BLIND, and this is the function that means it. A shot fired next to a parked guard
	 * puts the guard back on Live for the next tick, before any of the ranking runs. Without this the
	 * Parked tier would be nothing but switching perception off with a friendlier name, and the first
	 * thing a player would notice is an enemy that never looked up. This sentence is in the docs too, in
	 * a section of its own, because it is the difference between this plugin and the workaround it
	 * replaces.
	 *
	 * Returns true if the perceiver was woken.
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	bool Nudge(UAIPerceptionComponent* Perception, ESenseWakeReason Reason, float Distance = 0.0f);

	/** Something happened here. Wakes every registered perceiver whose profile says the event reaches it. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	int32 NudgeAround(FVector Location, float Radius, ESenseWakeReason Reason);

	//~ Modes --------------------------------------------------------------------------------------------

	/** Off means no tiers and no ceiling: everybody Live, every frame. The comparison mode. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	void SetTiersEnabled(bool bInEnabled);

	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	bool AreTiersEnabled() const { return bTiersEnabled; }

	/** Stop reassigning anything, for a screenshot or a breakpoint. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	void SetFrozen(bool bInFrozen);

	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	bool IsFrozen() const { return bFrozen; }

	/** Show or hide the counter box. One flag, read by every drawing path. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget")
	void SetStatsVisible(bool bVisible);

	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	bool AreStatsVisible() const { return bStatsVisible; }

	//~ Reading ------------------------------------------------------------------------------------------

	/** Everything measured on the frame that just ran. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	FSenseBudgetStats GetStats() const { return Stats; }

	/** The tier this component sits in. Parked for anything not registered. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	ESenseTier GetTier(const UAIPerceptionComponent* Perception) const;

	/** Seconds since an update was last issued for this component. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget")
	float GetSecondsSinceUpdate(const UAIPerceptionComponent* Perception) const;

	//~ Stress -------------------------------------------------------------------------------------------

	/**
	 * Add synthetic perceivers, for Sense.Stress.
	 *
	 * These have no component behind them. They are ranked, tiered, counted and they take budget, so the
	 * queue behaves exactly as it would with that many real guards - but nothing is issued to them,
	 * because there is nothing to issue to. Labelled "(synthetic)" on the counter box so a screenshot
	 * cannot be mistaken for a real crowd.
	 */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget|Debug")
	int32 AddSyntheticPerceivers(int32 Count, float SpreadDistance = 12000.0f);

	/** Remove every synthetic perceiver. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget|Debug")
	int32 ClearSyntheticPerceivers();

	//~ Output -------------------------------------------------------------------------------------------

	/** Draw the counter box. Called by ASenseBudgetHUD, or through AHUD::OnHUDPostRender. */
	void DrawStatsBox(UCanvas* Canvas, const FVector2D& Origin, float Width) const;

	/** Print the counters to the log. Sense.Stats. */
	void LogStats() const;

	/** Print one line per perceiver: tier, distance, wait, next due. Sense.Tiers. */
	void LogTiers() const;

private:
	/** Collect viewer locations - the camera of every local player, plus their pawn. */
	void GatherViewers();

	/** Build one FSenseRankEntry per registered perceiver from the live world. */
	void BuildRankEntries(float Now);

	/** Write tiers back onto the registry and open or close senses as the assignment demands. */
	void ApplyAssignments(float Now);

	/** Turn the senses we borrowed from this component on or off. */
	void SetSensesOpen(UAIPerceptionComponent* Perception, const TArray<TSubclassOf<UAISense>>& Senses, bool bOpen) const;

	/** The senses currently enabled on a component, which is the set this plugin is allowed to touch. */
	void CollectEnabledSenses(const UAIPerceptionComponent* Perception, TArray<TSubclassOf<UAISense>>& OutSenses) const;

	/** Ask the perception system to refresh this listener now. */
	void IssueUpdate(UAIPerceptionComponent* Perception) const;

	/** Distance from the nearest viewer to this perceiver. */
	float DistanceToNearestViewer(const FSensePerceiverState& State) const;

	/** Read the settings into the runtime fields. Called once at Initialize. */
	void ApplySettings();

	/** The profile used for perceivers registered without one. Created on demand. */
	USensePriorityProfile* GetOrCreateDefaultProfile();

	/** Drop everything queued up by an Unregister that arrived while the assignment was running. */
	void FlushPendingUnregisters();

	/** Put a perceiver's senses back and forget it. */
	void RemovePerceiver(int32 Id);

	/** Recompute the counters and the longest-wait list. */
	void RefreshStats(float Now);

	/** Bound to AHUD::OnHUDPostRender when bAutoDrawStatsOnAnyHUD is on. */
	void HandleHUDPostRender(AHUD* HUD, UCanvas* Canvas);

private:
	/** Id -> state. A map rather than an array because ids outlive positions and removals are common. */
	TMap<int32, FSensePerceiverState> Perceivers;

	/** Component -> id, so Register and Unregister do not have to walk the map. */
	TMap<TWeakObjectPtr<UAIPerceptionComponent>, int32> IdByPerception;

	/** Scratch arrays, kept between frames so a hundred perceivers cost no allocations per tick. */
	TArray<FSenseRankEntry> RankEntries;
	TArray<FSenseRankEntry> RankedEntries;
	TArray<int32> DueIds;
	TArray<FVector> ViewerLocations;

	/** Unregisters that arrived while ApplyAssignments was running, applied once it is finished. */
	TArray<int32> PendingUnregisters;

	/** The default profile, kept alive because perceivers registered without one point at it weakly. */
	UPROPERTY(Transient)
	TObjectPtr<USensePriorityProfile> DefaultProfile;

	FSenseBudgetStats Stats;
	FSenseRankWeights Weights;

	FDelegateHandle HudPostRenderHandle;

	int32 NextId = 1;
	int32 UpdateBudgetPerFrame = 12;
	int32 SyntheticCount = 0;

	/**
	 * Wakes counted since the last tick.
	 *
	 * A wake can arrive from anywhere in the frame - a damage handler, a weapon Blueprint, a console
	 * command - so it is accumulated here and moved onto the stats at the start of the next tick. Zeroing
	 * it at the end of the tick instead would mean the counter box, which draws after the tick, only ever
	 * showed zero.
	 */
	int32 WakesSinceTick = 0;

	float SenseWindowSeconds = 0.05f;
	float RecentlyRenderedTolerance = 0.2f;
	float DefaultParkDistance = 6000.0f;
	float StarvationSeconds = 2.0f;

	bool bSchedulerEnabled = true;
	bool bTiersEnabled = true;
	bool bFrozen = false;
	bool bStatsVisible = true;
	int32 LongestWaitRows = 5;

	/** True while ApplyAssignments is walking the registry. Unregisters are deferred while it is. */
	bool bApplying = false;
};
