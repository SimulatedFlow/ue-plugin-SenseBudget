// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SenseBudgetSubsystem.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GlobalRenderResources.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/StringBuilder.h"
#include "SenseBudgetComponent.h"
#include "SenseBudgetHUD.h"
#include "SenseBudgetLog.h"
#include "SenseBudgetSettings.h"
#include "SenseBudgetStatics.h"
#include "SensePriorityProfile.h"
#include "UObject/Package.h"

// Engine drift, guarded.
//
// The only AIModule surface this plugin uses is the public, BlueprintCallable half of
// UAIPerceptionComponent plus UAISenseConfig::GetSenseImplementation. Nothing from AIModule/Private is
// touched, no sense is subclassed and no internal container is reached into. The includes are still
// guarded: if a future engine moves these headers, the scheduler keeps compiling and keeps ranking,
// tiering and counting, and only the issuing of perception updates falls away with a warning at the top
// of the build - which is a plugin that reports honestly that it cannot pace anything, rather than a
// plugin that will not build at all.
#if __has_include("Perception/AIPerceptionComponent.h") && __has_include("Perception/AISenseConfig.h")
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense.h"
#include "Perception/AISenseConfig.h"
#define SENSEBUDGET_WITH_AIPERCEPTION 1
#else
#define SENSEBUDGET_WITH_AIPERCEPTION 0
#endif

#if !SENSEBUDGET_WITH_AIPERCEPTION
#pragma message("SenseBudget: AIModule perception headers not found. The scheduler will rank and report but will not throttle anything.")
#endif

namespace SenseBudgetPrivate
{
	/** Lines the counter box always draws, before the "waiting longest" rows. */
	static constexpr int32 FixedStatsLines = 7;

	static constexpr float LineHeight = 15.0f;
	static constexpr float BoxPadding = 8.0f;

	static const FLinearColor PanelBackground(0.0f, 0.0f, 0.0f, 0.62f);
	static const FLinearColor HeadingColor(0.42f, 0.78f, 1.0f, 1.0f);
	static const FLinearColor BodyColor(0.90f, 0.90f, 0.90f, 1.0f);
	static const FLinearColor GoodColor(0.55f, 0.95f, 0.55f, 1.0f);
	static const FLinearColor WarnColor(0.98f, 0.78f, 0.35f, 1.0f);
	static const FLinearColor DimColor(0.62f, 0.62f, 0.62f, 1.0f);

	static void DrawFilledRect(UCanvas* Canvas, const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color)
	{
		FCanvasTileItem Tile(Position, GWhiteTexture, Size, Color);
		Tile.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Tile);
	}

	static const TCHAR* TierName(const ESenseTier Tier)
	{
		switch (Tier)
		{
		case ESenseTier::Live:		return TEXT("live");
		case ESenseTier::Slow:		return TEXT("slow");
		case ESenseTier::Parked:	return TEXT("parked");
		default:					return TEXT("?");
		}
	}

	static USenseBudgetSubsystem* GetSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<USenseBudgetSubsystem>() : nullptr;
	}
}

//~ Lifetime -------------------------------------------------------------------------------------------

void USenseBudgetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	ApplySettings();

	if (USenseBudgetSettings::Get().bAutoDrawStatsOnAnyHUD)
	{
		HudPostRenderHandle = AHUD::OnHUDPostRender.AddUObject(this, &USenseBudgetSubsystem::HandleHUDPostRender);
	}

	UE_LOG(LogSenseBudget, Log,
		TEXT("SenseBudget up: %s, budget %d updates per frame, starvation limit %.2f s, park distance %.0f cm."),
		bSchedulerEnabled ? TEXT("enabled") : TEXT("disabled"),
		UpdateBudgetPerFrame, StarvationSeconds, DefaultParkDistance);
}

void USenseBudgetSubsystem::Deinitialize()
{
	if (HudPostRenderHandle.IsValid())
	{
		AHUD::OnHUDPostRender.Remove(HudPostRenderHandle);
		HudPostRenderHandle.Reset();
	}

	// Give every borrowed sense back before letting go. A world being torn down does not need this, but a
	// subsystem that is collected while an actor lives on in a seamless travel does - and a perceiver left
	// with its sight switched off by the plugin that promised never to blind anybody is the single worst
	// bug this plugin could ship.
	for (TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		if (UAIPerceptionComponent* Perception = Pair.Value.Perception.Get())
		{
			SetSensesOpen(Perception, Pair.Value.ManagedSenses, true);
		}
	}

	Perceivers.Reset();
	IdByPerception.Reset();
	RankEntries.Reset();
	RankedEntries.Reset();
	DueIds.Reset();
	ViewerLocations.Reset();
	PendingUnregisters.Reset();
	DefaultProfile = nullptr;

	Super::Deinitialize();
}

bool USenseBudgetSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game and PIE, deliberately not Editor. A level being built is not a level being played, and a
	// designer nudging a guard around a map does not want its senses being switched on and off underneath.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USenseBudgetSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USenseBudgetSubsystem, STATGROUP_Tickables);
}

USenseBudgetSubsystem* USenseBudgetSubsystem::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<USenseBudgetSubsystem>() : nullptr;
}

void USenseBudgetSubsystem::ApplySettings()
{
	const USenseBudgetSettings& Settings = USenseBudgetSettings::Get();

	bSchedulerEnabled = Settings.bEnabled;
	bStatsVisible = Settings.bShowStatsByDefault;
	UpdateBudgetPerFrame = FMath::Max(1, Settings.UpdateBudgetPerFrame);
	StarvationSeconds = FMath::Max(0.05f, Settings.StarvationSeconds);
	SenseWindowSeconds = FMath::Max(0.0f, Settings.SenseWindowSeconds);
	RecentlyRenderedTolerance = FMath::Max(0.0f, Settings.RecentlyRenderedTolerance);
	DefaultParkDistance = FMath::Max(0.0f, Settings.DefaultParkDistance);
	LongestWaitRows = FMath::Clamp(Settings.LongestWaitRows, 0, 16);

	Weights = Settings.RankWeights;

	// The promise lives on the settings; the trigger the ranking uses sits below it, with room for the
	// queue above the line to drain. Derived here rather than trusted from the weights struct, so an ini
	// that overrode only one of the two halves cannot leave them contradicting each other.
	Weights.StarvationTriggerSeconds = FMath::Max(0.01f, StarvationSeconds * FMath::Clamp(Settings.StarvationTriggerFraction, 0.05f, 1.0f));
}

USensePriorityProfile* USenseBudgetSubsystem::GetOrCreateDefaultProfile()
{
	if (!DefaultProfile)
	{
		// Class defaults, deliberately: importance 1, half-second slow rate, three-second parked rate,
		// five-second floor. A perceiver nobody configured comes out ordinary, not switched off.
		DefaultProfile = NewObject<USensePriorityProfile>(this, TEXT("SenseBudgetDefaultProfile"));
	}
	return DefaultProfile;
}

//~ Registration ---------------------------------------------------------------------------------------

int32 USenseBudgetSubsystem::Register(UAIPerceptionComponent* Perception, USensePriorityProfile* Profile, USenseBudgetComponent* Notify)
{
	if (!IsValid(Perception))
	{
		return INDEX_NONE;
	}

	if (const int32* Existing = IdByPerception.Find(Perception))
	{
		return *Existing;
	}

	FSensePerceiverState State;
	State.Id = NextId++;
	State.Perception = Perception;
	State.Profile = Profile ? Profile : GetOrCreateDefaultProfile();
	State.Notify = Notify;
	State.Owner = Perception->GetOwner();
	State.Tier = ESenseTier::Slow;
	State.bSensesOpen = true;
	State.Label = GetNameSafe(Perception->GetOwner());

	// Only ever touch what was on when we arrived. Everything else stays exactly as the project left it.
	CollectEnabledSenses(Perception, State.ManagedSenses);

	// Due immediately. A perceiver that has just been spawned into a firefight should not have to wait out
	// a slow interval before its first look, and the budget still bounds what that costs.
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	State.LastUpdateTime = Now;
	State.NextDueTime = Now;

	// Read the id out before the move: the order in which a call's arguments are evaluated is not
	// specified, so passing State.Id alongside MoveTemp(State) would be reading from a moved-from object
	// on some compilers and not on others.
	const int32 NewId = State.Id;

	IdByPerception.Add(Perception, NewId);
	Perceivers.Add(NewId, MoveTemp(State));

	return NewId;
}

void USenseBudgetSubsystem::Unregister(UAIPerceptionComponent* Perception)
{
	if (!Perception)
	{
		return;
	}

	const int32* Found = IdByPerception.Find(Perception);
	if (!Found)
	{
		return;
	}

	const int32 Id = *Found;

	// Unregistering in the middle of an assignment is the one reentrancy this plugin has to survive: an
	// actor destroyed by a tier-change handler, or a pooled AI recycled from a Blueprint. The removal is
	// queued and applied the moment the assignment is finished, so nothing that is being walked over
	// changes shape underneath.
	if (bApplying)
	{
		PendingUnregisters.AddUnique(Id);
		return;
	}

	RemovePerceiver(Id);
}

void USenseBudgetSubsystem::RemovePerceiver(const int32 Id)
{
	FSensePerceiverState State;
	if (!Perceivers.RemoveAndCopyValue(Id, State))
	{
		return;
	}

	IdByPerception.Remove(State.Perception);

	if (State.bSynthetic)
	{
		SyntheticCount = FMath::Max(0, SyntheticCount - 1);
		return;
	}

	// Hand the senses back on the way out.
	if (UAIPerceptionComponent* Perception = State.Perception.Get())
	{
		SetSensesOpen(Perception, State.ManagedSenses, true);
	}
}

void USenseBudgetSubsystem::FlushPendingUnregisters()
{
	if (PendingUnregisters.Num() == 0)
	{
		return;
	}

	// Copied out first: removing a perceiver can, through a component's EndPlay, queue another one.
	TArray<int32> Pending = MoveTemp(PendingUnregisters);
	PendingUnregisters.Reset();

	for (const int32 Id : Pending)
	{
		RemovePerceiver(Id);
	}
}

bool USenseBudgetSubsystem::IsPerceiverManaged(const UAIPerceptionComponent* Perception) const
{
	return Perception && IdByPerception.Contains(const_cast<UAIPerceptionComponent*>(Perception));
}

//~ Budget ---------------------------------------------------------------------------------------------

void USenseBudgetSubsystem::SetBudget(const int32 UpdatesPerFrame)
{
	// At least one. A budget of zero would be an off switch wearing a number, and this plugin does not
	// have an off switch for perception - it has bEnabled, which hands everything back instead.
	const int32 NewBudget = FMath::Max(1, UpdatesPerFrame);
	if (NewBudget == UpdateBudgetPerFrame)
	{
		return;
	}

	UpdateBudgetPerFrame = NewBudget;
	UE_LOG(LogSenseBudget, Log, TEXT("SenseBudget: budget is now %d perception updates per frame."), UpdateBudgetPerFrame);
}

//~ Waking ---------------------------------------------------------------------------------------------

bool USenseBudgetSubsystem::Nudge(UAIPerceptionComponent* Perception, const ESenseWakeReason Reason, const float Distance)
{
	if (!Perception)
	{
		return false;
	}

	const int32* Found = IdByPerception.Find(Perception);
	if (!Found)
	{
		return false;
	}

	FSensePerceiverState* State = Perceivers.Find(*Found);
	if (!State)
	{
		return false;
	}

	if (!USenseBudgetStatics::ShouldWake(State->Profile.Get(), Reason, Distance))
	{
		return false;
	}

	// PARKED IS NOT BLIND. These three lines are the whole of that promise: the wake flag puts the
	// perceiver in front of the entire queue on the next ranking, and the due time makes it eligible on
	// the very next frame rather than at the end of its parked interval. A shot next to a parked guard is
	// a guard that looks up. Without this the Parked tier would be switching perception off with a
	// friendlier name, and the first thing a player would notice is an enemy that never saw them coming.
	State->bWakePending = true;
	State->NextDueTime = 0.0f;

	++WakesSinceTick;
	return true;
}

int32 USenseBudgetSubsystem::NudgeAround(const FVector Location, const float Radius, const ESenseWakeReason Reason)
{
	const float Reach = FMath::Max(0.0f, Radius);
	int32 Woken = 0;

	for (TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		FSensePerceiverState& State = Pair.Value;

		// Synthetic stress perceivers have no place in the world, so an event with a place cannot reach
		// them. They still count towards the budget; they just cannot be shot at.
		if (State.bSynthetic)
		{
			continue;
		}

		const AActor* OwnerActor = State.Owner.Get();
		if (!IsValid(OwnerActor))
		{
			continue;
		}

		const float Distance = FVector::Dist(Location, OwnerActor->GetActorLocation());

		// Two gates, and both have to open: the event has a reach, and the perceiver has an
		// attentiveness. A quiet footstep does not carry, and a deaf turret does not care that it did.
		if (Distance > Reach)
		{
			continue;
		}

		if (!USenseBudgetStatics::ShouldWake(State.Profile.Get(), Reason, Distance))
		{
			continue;
		}

		State.bWakePending = true;
		State.NextDueTime = 0.0f;
		++Woken;
	}

	WakesSinceTick += Woken;

	if (Woken > 0)
	{
		UE_LOG(LogSenseBudget, Verbose, TEXT("SenseBudget: %d perceiver(s) woken by %s within %.0f cm."),
			Woken, *UEnum::GetValueAsString(Reason), Reach);
	}

	return Woken;
}

//~ Modes ----------------------------------------------------------------------------------------------

void USenseBudgetSubsystem::SetTiersEnabled(const bool bInEnabled)
{
	if (bTiersEnabled == bInEnabled)
	{
		return;
	}

	bTiersEnabled = bInEnabled;

	if (!bTiersEnabled)
	{
		// Comparison mode. Everything is handed straight back - senses open, tier Live - so the very next
		// frame is what the project would look like without this plugin at all. That is the whole point:
		// the claim can be measured against its own absence, on the same machine, in the same scene.
		for (TPair<int32, FSensePerceiverState>& Pair : Perceivers)
		{
			FSensePerceiverState& State = Pair.Value;
			State.Tier = ESenseTier::Live;
			State.bSensesOpen = true;

			if (UAIPerceptionComponent* Perception = State.Perception.Get())
			{
				SetSensesOpen(Perception, State.ManagedSenses, true);
			}
		}
	}

	UE_LOG(LogSenseBudget, Log, TEXT("SenseBudget: tiers %s."), bTiersEnabled ? TEXT("on") : TEXT("off (everybody Live, no ceiling)"));
}

void USenseBudgetSubsystem::SetFrozen(const bool bInFrozen)
{
	bFrozen = bInFrozen;
	UE_LOG(LogSenseBudget, Log, TEXT("SenseBudget: scheduler %s."), bFrozen ? TEXT("frozen") : TEXT("running"));
}

void USenseBudgetSubsystem::SetStatsVisible(const bool bVisible)
{
	bStatsVisible = bVisible;
}

//~ Reading --------------------------------------------------------------------------------------------

ESenseTier USenseBudgetSubsystem::GetTier(const UAIPerceptionComponent* Perception) const
{
	if (!Perception)
	{
		return ESenseTier::Parked;
	}

	if (const int32* Found = IdByPerception.Find(const_cast<UAIPerceptionComponent*>(Perception)))
	{
		if (const FSensePerceiverState* State = Perceivers.Find(*Found))
		{
			return State->Tier;
		}
	}

	return ESenseTier::Parked;
}

float USenseBudgetSubsystem::GetSecondsSinceUpdate(const UAIPerceptionComponent* Perception) const
{
	const UWorld* World = GetWorld();
	if (!World || !Perception)
	{
		return 0.0f;
	}

	if (const int32* Found = IdByPerception.Find(const_cast<UAIPerceptionComponent*>(Perception)))
	{
		if (const FSensePerceiverState* State = Perceivers.Find(*Found))
		{
			return FMath::Max(0.0f, World->GetTimeSeconds() - State->LastUpdateTime);
		}
	}

	return 0.0f;
}

//~ Stress ---------------------------------------------------------------------------------------------

int32 USenseBudgetSubsystem::AddSyntheticPerceivers(const int32 Count, const float SpreadDistance)
{
	const int32 ToAdd = FMath::Max(0, Count);
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	const float Spread = FMath::Max(1.0f, SpreadDistance);

	for (int32 Index = 0; Index < ToAdd; ++Index)
	{
		FSensePerceiverState State;
		State.Id = NextId++;
		State.bSynthetic = true;
		State.Profile = GetOrCreateDefaultProfile();
		State.Tier = ESenseTier::Slow;
		State.LastUpdateTime = Now;
		State.NextDueTime = Now;

		// Spread evenly out to SpreadDistance so the crowd covers all three tiers rather than piling into
		// one of them - a stress test where everybody is Parked proves nothing about the queue.
		State.SyntheticDistance = Spread * (static_cast<float>(Index) / FMath::Max(1.0f, static_cast<float>(ToAdd)));
		State.Label = FString::Printf(TEXT("synthetic %d"), Index);

		const int32 NewId = State.Id;
		Perceivers.Add(NewId, MoveTemp(State));
		++SyntheticCount;
	}

	UE_LOG(LogSenseBudget, Log, TEXT("SenseBudget: %d synthetic perceiver(s) added, %d in total, spread over %.0f cm."),
		ToAdd, SyntheticCount, Spread);

	return ToAdd;
}

int32 USenseBudgetSubsystem::ClearSyntheticPerceivers()
{
	TArray<int32> ToRemove;
	ToRemove.Reserve(SyntheticCount);

	for (const TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		if (Pair.Value.bSynthetic)
		{
			ToRemove.Add(Pair.Key);
		}
	}

	for (const int32 Id : ToRemove)
	{
		RemovePerceiver(Id);
	}

	return ToRemove.Num();
}

//~ The tick -------------------------------------------------------------------------------------------

void USenseBudgetSubsystem::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double TickStart = FPlatformTime::Seconds();
	const float Now = World->GetTimeSeconds();

	Stats.UpdatesThisFrame = 0;
	Stats.DueThisFrame = 0;
	Stats.DeferredThisFrame = 0;
	Stats.RankMilliseconds = 0.0f;
	Stats.IssueMilliseconds = 0.0f;

	// Wakes arrive from anywhere in the frame, so they are accumulated as they happen and moved onto the
	// stats here, at the start of the tick, where the counter box can still see them.
	Stats.WakesThisFrame = WakesSinceTick;
	WakesSinceTick = 0;

	if (!bSchedulerEnabled)
	{
		RefreshStats(Now);
		Stats.SenseMilliseconds = static_cast<float>((FPlatformTime::Seconds() - TickStart) * 1000.0);
		return;
	}

	if (bFrozen)
	{
		// Frozen means frozen: no reranking, no reassignment, no updates issued. The counters keep
		// reporting what was true when the freeze happened, which is the point - a screenshot of a moving
		// scene is a screenshot of nothing in particular.
		RefreshStats(Now);
		Stats.SenseMilliseconds = static_cast<float>((FPlatformTime::Seconds() - TickStart) * 1000.0);
		return;
	}

	if (!bTiersEnabled)
	{
		// Comparison mode: everybody Live, every frame, no ceiling. This is what the project costs
		// without this plugin, measured by the same clock as the budgeted case, so the two numbers on the
		// two screenshots are comparable.
		const double IssueStart = FPlatformTime::Seconds();

		bApplying = true;
		for (TPair<int32, FSensePerceiverState>& Pair : Perceivers)
		{
			FSensePerceiverState& State = Pair.Value;
			State.Tier = ESenseTier::Live;
			State.LastUpdateTime = Now;
			State.NextDueTime = Now;
			State.bWakePending = false;

			if (!State.bSynthetic)
			{
				if (UAIPerceptionComponent* Perception = State.Perception.Get())
				{
					IssueUpdate(Perception);
				}
			}

			++Stats.UpdatesThisFrame;
		}
		bApplying = false;
		FlushPendingUnregisters();

		Stats.DueThisFrame = Stats.UpdatesThisFrame;
		Stats.IssueMilliseconds = static_cast<float>((FPlatformTime::Seconds() - IssueStart) * 1000.0);

		RefreshStats(Now);
		Stats.SenseMilliseconds = static_cast<float>((FPlatformTime::Seconds() - TickStart) * 1000.0);
		return;
	}

	//
	// The ordinary path. Gather plain numbers, hand them to the pure functions, write the answers back.
	//
	const double RankStart = FPlatformTime::Seconds();

	GatherViewers();
	BuildRankEntries(Now);

	RankedEntries = USenseBudgetStatics::RankPerceivers(RankEntries, Weights);
	RankedEntries = USenseBudgetStatics::AssignTiers(RankedEntries, UpdateBudgetPerFrame);
	DueIds = USenseBudgetStatics::SelectUpdates(RankedEntries, UpdateBudgetPerFrame, Now);

	Stats.RankMilliseconds = static_cast<float>((FPlatformTime::Seconds() - RankStart) * 1000.0);

	// How many wanted a turn, before the ceiling. The gap between this and UpdatesThisFrame is the queue,
	// and it is shown on the box because a budget with nothing waiting behind it is a budget that is not
	// doing anything.
	for (const FSenseRankEntry& Entry : RankedEntries)
	{
		if (Entry.NextDueTime <= Now)
		{
			++Stats.DueThisFrame;
		}
	}
	Stats.DeferredThisFrame = FMath::Max(0, Stats.DueThisFrame - DueIds.Num());

	const double IssueStart = FPlatformTime::Seconds();
	ApplyAssignments(Now);
	Stats.IssueMilliseconds = static_cast<float>((FPlatformTime::Seconds() - IssueStart) * 1000.0);

	RefreshStats(Now);

	Stats.SenseMilliseconds = static_cast<float>((FPlatformTime::Seconds() - TickStart) * 1000.0);
}

void USenseBudgetSubsystem::GatherViewers()
{
	ViewerLocations.Reset();

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		if (!IsValid(PC))
		{
			continue;
		}

		// The camera, not the pawn, is where the player is actually looking from - a third-person camera
		// can sit twenty metres behind its pawn, and ranking off the pawn would quietly demote the guards
		// that are on screen.
		if (const APlayerCameraManager* Camera = PC->PlayerCameraManager)
		{
			ViewerLocations.Add(Camera->GetCameraLocation());
		}

		if (const APawn* Pawn = PC->GetPawn())
		{
			ViewerLocations.Add(Pawn->GetActorLocation());
		}
	}
}

float USenseBudgetSubsystem::DistanceToNearestViewer(const FSensePerceiverState& State) const
{
	if (State.bSynthetic)
	{
		return State.SyntheticDistance;
	}

	const AActor* OwnerActor = State.Owner.Get();
	if (!IsValid(OwnerActor))
	{
		return 0.0f;
	}

	if (ViewerLocations.Num() == 0)
	{
		// No viewers - a dedicated server before anybody joins, or a cinematic with no player. Everybody
		// counts as close, which keeps them out of Parked. The budget still bounds the cost; the only
		// thing lost is the ordering, and erring towards "close" errs towards nobody going blind.
		return 0.0f;
	}

	const FVector Location = OwnerActor->GetActorLocation();
	float Nearest = TNumericLimits<float>::Max();

	for (const FVector& Viewer : ViewerLocations)
	{
		Nearest = FMath::Min(Nearest, static_cast<float>(FVector::Dist(Viewer, Location)));
	}

	return Nearest;
}

void USenseBudgetSubsystem::BuildRankEntries(const float Now)
{
	RankEntries.Reset(Perceivers.Num());

	for (const TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		const FSensePerceiverState& State = Pair.Value;

		FSenseRankEntry Entry;
		Entry.Id = State.Id;
		Entry.DistanceToViewer = DistanceToNearestViewer(State);
		Entry.WaitSeconds = FMath::Max(0.0f, Now - State.LastUpdateTime);
		Entry.OverdueSeconds = FMath::Max(0.0f, Now - State.NextDueTime);
		Entry.NextDueTime = State.NextDueTime;
		Entry.bWakePending = State.bWakePending;

		const USensePriorityProfile* Profile = State.Profile.Get();
		Entry.Importance = Profile ? FMath::Max(0.0f, Profile->Importance) : 1.0f;

		// A profile that leaves the park distance at zero means "never park by distance". The project-wide
		// default only fills in for a perceiver with no profile at all, so an explicit zero on an asset
		// stays an explicit zero.
		Entry.ParkDistance = Profile ? Profile->ParkDistance : DefaultParkDistance;

		if (const AActor* OwnerActor = State.Owner.Get(); IsValid(OwnerActor))
		{
			Entry.bRecentlyRendered = OwnerActor->WasRecentlyRendered(RecentlyRenderedTolerance);
		}

		RankEntries.Add(Entry);
	}
}

void USenseBudgetSubsystem::ApplyAssignments(const float Now)
{
	// Tier changes are collected rather than broadcast inline. A Blueprint handler is allowed to do
	// anything - destroy the actor, unregister the component, spawn another one - and none of that may
	// happen while the registry is being walked.
	struct FPendingTierChange
	{
		TWeakObjectPtr<USenseBudgetComponent> Component;
		ESenseTier NewTier = ESenseTier::Slow;
		ESenseTier OldTier = ESenseTier::Slow;
	};
	TArray<FPendingTierChange, TInlineAllocator<16>> TierChanges;

	// Components whose window has closed. Collected for the same reason.
	struct FPendingClose
	{
		TWeakObjectPtr<UAIPerceptionComponent> Perception;
		TArray<TSubclassOf<UAISense>> Senses;
	};
	TArray<FPendingClose> Closes;

	bApplying = true;

	// (1) Write the tiers back. Ranking sorted the array, so the only way back to a perceiver is its id -
	//     and an id that is no longer in the registry simply finds nothing and is skipped. That is the
	//     whole defence against a component that unregistered halfway through this frame: the result is
	//     dropped rather than applied to whoever happens to sit at that index now.
	for (const FSenseRankEntry& Entry : RankedEntries)
	{
		FSensePerceiverState* State = Perceivers.Find(Entry.Id);
		if (!State)
		{
			continue;
		}

		if (State->Tier != Entry.Tier)
		{
			if (USenseBudgetComponent* Component = State->Notify.Get())
			{
				TierChanges.Add({ Component, Entry.Tier, State->Tier });
			}
			State->Tier = Entry.Tier;
		}
	}

	// (2) Serve this frame's due list, in ranked order, up to the ceiling.
	for (const int32 Id : DueIds)
	{
		FSensePerceiverState* State = Perceivers.Find(Id);
		if (!State)
		{
			continue;
		}

		UAIPerceptionComponent* Perception = State->Perception.Get();
		const USensePriorityProfile* Profile = State->Profile.Get();
		const ESenseTier Tier = State->Tier;
		const bool bSynthetic = State->bSynthetic;
		TArray<TSubclassOf<UAISense>> Senses = State->ManagedSenses;

		// Bookkeeping first, engine calls second, and the state pointer is never touched again after
		// this block. A TMap value pointer does not survive a rehash, and the cheapest way to be certain
		// it is never used across a call that could cause one is to finish with it before making any.
		State->LastUpdateTime = Now;
		State->NextDueTime = USenseBudgetStatics::NextDue(Tier, Profile, Now);
		State->WindowCloseTime = Now + SenseWindowSeconds;
		State->bWakePending = false;
		State->bSensesOpen = true;
		State = nullptr;

		if (!bSynthetic && IsValid(Perception))
		{
			SetSensesOpen(Perception, Senses, true);
			IssueUpdate(Perception);
		}

		++Stats.UpdatesThisFrame;
		++Stats.TotalUpdates;
	}

	// (3) Close the windows that have run out. A Live perceiver's window never closes - that is what Live
	//     means - and everybody else keeps their senses open for SenseWindowSeconds after being served,
	//     long enough that the sight sense, which only runs a handful of traces per tick, actually gets
	//     round to the look that was scheduled.
	for (TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		FSensePerceiverState& State = Pair.Value;

		if (State.bSynthetic || State.Tier == ESenseTier::Live || !State.bSensesOpen)
		{
			continue;
		}

		if (Now < State.WindowCloseTime)
		{
			continue;
		}

		State.bSensesOpen = false;

		if (UAIPerceptionComponent* Perception = State.Perception.Get())
		{
			Closes.Add({ Perception, State.ManagedSenses });
		}
	}

	bApplying = false;

	for (const FPendingClose& Close : Closes)
	{
		if (UAIPerceptionComponent* Perception = Close.Perception.Get())
		{
			SetSensesOpen(Perception, Close.Senses, false);
		}
	}

	FlushPendingUnregisters();

	for (const FPendingTierChange& Change : TierChanges)
	{
		if (USenseBudgetComponent* Component = Change.Component.Get())
		{
			Component->NotifyTierChanged(Change.NewTier, Change.OldTier);
		}
	}
}

void USenseBudgetSubsystem::RefreshStats(const float Now)
{
	Stats.Perceivers = Perceivers.Num();
	Stats.Budget = UpdateBudgetPerFrame;
	Stats.StarvationSeconds = StarvationSeconds;
	Stats.bTiersEnabled = bTiersEnabled;
	Stats.bFrozen = bFrozen;
	Stats.LiveCount = 0;
	Stats.SlowCount = 0;
	Stats.ParkedCount = 0;
	Stats.LongestWaitSeconds = 0.0f;
	Stats.MostOverdueSeconds = 0.0f;

	TArray<FSenseWaitEntry> Waits;
	Waits.Reserve(Perceivers.Num());

	for (const TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		const FSensePerceiverState& State = Pair.Value;

		switch (State.Tier)
		{
		case ESenseTier::Live:		++Stats.LiveCount; break;
		case ESenseTier::Slow:		++Stats.SlowCount; break;
		case ESenseTier::Parked:	++Stats.ParkedCount; break;
		default: break;
		}

		FSenseWaitEntry Wait;
		Wait.WaitSeconds = FMath::Max(0.0f, Now - State.LastUpdateTime);
		Wait.OverdueSeconds = FMath::Max(0.0f, Now - State.NextDueTime);
		Wait.Tier = State.Tier;
		Wait.Label = State.bSynthetic ? FString::Printf(TEXT("%s (synthetic)"), *State.Label) : State.Label;

		Stats.LongestWaitSeconds = FMath::Max(Stats.LongestWaitSeconds, Wait.WaitSeconds);
		Stats.MostOverdueSeconds = FMath::Max(Stats.MostOverdueSeconds, Wait.OverdueSeconds);
		Waits.Add(MoveTemp(Wait));
	}

	Stats.LongestWaits.Reset();

	if (LongestWaitRows > 0 && Waits.Num() > 0)
	{
		const int32 Rows = FMath::Min(LongestWaitRows, Waits.Num());

		// Ranked by lateness rather than by raw waiting time, because that is what the list is for:
		// showing who the budget is failing, not who has a slow profile and is perfectly happy with it.
		//
		// Partial sort by hand: the box only ever shows five rows, and sorting a thousand perceivers every
		// frame to find them would make the scheduler the cost it exists to remove.
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			int32 Worst = Row;
			for (int32 Index = Row + 1; Index < Waits.Num(); ++Index)
			{
				if (Waits[Index].OverdueSeconds > Waits[Worst].OverdueSeconds)
				{
					Worst = Index;
				}
			}
			Waits.Swap(Row, Worst);
			Stats.LongestWaits.Add(Waits[Row]);
		}
	}
}

//~ The engine surface -------------------------------------------------------------------------------------

void USenseBudgetSubsystem::CollectEnabledSenses(const UAIPerceptionComponent* Perception, TArray<TSubclassOf<UAISense>>& OutSenses) const
{
	OutSenses.Reset();

#if SENSEBUDGET_WITH_AIPERCEPTION
	if (!Perception)
	{
		return;
	}

	for (auto It = Perception->GetSensesConfigIterator(); It; ++It)
	{
		const UAISenseConfig* Config = *It;
		if (!Config)
		{
			continue;
		}

		const TSubclassOf<UAISense> SenseClass = Config->GetSenseImplementation();
		if (!SenseClass)
		{
			continue;
		}

		// Only what is on right now. A sense a project has deliberately switched off stays off, and this
		// plugin gives back exactly what it borrowed.
		if (Perception->IsSenseEnabled(SenseClass))
		{
			OutSenses.AddUnique(SenseClass);
		}
	}
#endif
}

void USenseBudgetSubsystem::SetSensesOpen(UAIPerceptionComponent* Perception, const TArray<TSubclassOf<UAISense>>& Senses, const bool bOpen) const
{
#if SENSEBUDGET_WITH_AIPERCEPTION
	if (!IsValid(Perception))
	{
		return;
	}

	// SetSenseEnabled moves the listener in and out of that sense's allow list. The engine early-outs when
	// the value has not changed, so calling this every time a perceiver is served costs nothing when it is
	// already open, and there is no need for this plugin to keep a second copy of the truth.
	for (const TSubclassOf<UAISense>& SenseClass : Senses)
	{
		if (SenseClass)
		{
			Perception->SetSenseEnabled(SenseClass, bOpen);
		}
	}
#endif
}

void USenseBudgetSubsystem::IssueUpdate(UAIPerceptionComponent* Perception) const
{
#if SENSEBUDGET_WITH_AIPERCEPTION
	if (IsValid(Perception))
	{
		Perception->RequestStimuliListenerUpdate();
	}
#endif
}

//~ The counter box ------------------------------------------------------------------------------------

void USenseBudgetSubsystem::HandleHUDPostRender(AHUD* HUD, UCanvas* Canvas)
{
	if (!HUD || !Canvas || !bStatsVisible)
	{
		return;
	}

	// The two drawing paths must not stack. An ASenseBudgetHUD draws the box itself, so this one stands
	// down for it - otherwise a project that both reparented its HUD and turned on the setting would get
	// the same box twice, half a pixel apart.
	if (HUD->IsA<ASenseBudgetHUD>())
	{
		return;
	}

	DrawStatsBox(Canvas, FVector2D(28.0f, 90.0f), 470.0f);
}

void USenseBudgetSubsystem::DrawStatsBox(UCanvas* Canvas, const FVector2D& Origin, const float Width) const
{
	using namespace SenseBudgetPrivate;

	if (!Canvas)
	{
		return;
	}

	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const int32 WaitRows = Stats.LongestWaits.Num();
	const int32 LineCount = FixedStatsLines + (WaitRows > 0 ? WaitRows + 1 : 0);

	const float BoxHeight = LineCount * LineHeight + BoxPadding * 2.0f;
	DrawFilledRect(Canvas,
		FVector2D(Origin.X - BoxPadding, Origin.Y - BoxPadding),
		FVector2D(Width, BoxHeight),
		PanelBackground);

	float LineY = static_cast<float>(Origin.Y);
	auto DrawLine = [&](FStringView Line, const FLinearColor& Color)
	{
		FCanvasTextStringViewItem Item(FVector2D(Origin.X, LineY), Line, Font, Color);
		Canvas->DrawItem(Item);
		LineY += LineHeight;
	};

	TStringBuilder<256> Line;

	Line.Reset();
	Line.Appendf(TEXT("SenseBudget%s%s"),
		Stats.bTiersEnabled ? TEXT("") : TEXT("   (tiers off - everybody Live)"),
		Stats.bFrozen ? TEXT("   [frozen]") : TEXT(""));
	DrawLine(Line.ToView(), Stats.bTiersEnabled ? HeadingColor : WarnColor);

	Line.Reset();
	Line.Appendf(TEXT("perceivers     %d"), Stats.Perceivers);
	DrawLine(Line.ToView(), BodyColor);

	// The line the whole plugin is judged on: how many updates were issued this frame, against the ceiling
	// they were measured against. A total would look impressive and prove nothing; what has to be shown is
	// that the per-frame number never goes above the number beside it.
	Line.Reset();
	Line.Appendf(TEXT("updates        %d / %d this frame"), Stats.UpdatesThisFrame, Stats.Budget);
	DrawLine(Line.ToView(),
		(Stats.bTiersEnabled && Stats.UpdatesThisFrame > Stats.Budget) ? WarnColor : GoodColor);

	Line.Reset();
	Line.Appendf(TEXT("tiers          live %d   slow %d   parked %d"), Stats.LiveCount, Stats.SlowCount, Stats.ParkedCount);
	DrawLine(Line.ToView(), BodyColor);

	Line.Reset();
	Line.Appendf(TEXT("queue          %d due   %d waiting for a later frame"), Stats.DueThisFrame, Stats.DeferredThisFrame);
	DrawLine(Line.ToView(), Stats.DeferredThisFrame > 0 ? WarnColor : BodyColor);

	// Two numbers, because they answer two different questions. The wait is how long ago the most
	// neglected perceiver last looked; the lateness is how far past its OWN due time that is. Only the
	// second one is a promise this plugin made, so only the second one is measured against the limit.
	Line.Reset();
	Line.Appendf(TEXT("longest wait   %.2f s   overdue %.2f s   (limit %.2f s)"),
		Stats.LongestWaitSeconds, Stats.MostOverdueSeconds, Stats.StarvationSeconds);
	DrawLine(Line.ToView(), Stats.MostOverdueSeconds > Stats.StarvationSeconds ? WarnColor : GoodColor);

	// Measured, and labelled for exactly what it measures. This is the scheduler's own tick plus the
	// listener updates it issued - not the engine's sight traces, which run later in the perception
	// system's own tick and are bounded by the updates line above.
	Line.Reset();
	Line.Appendf(TEXT("sense ms       %.3f   (rank %.3f / issue %.3f)"),
		Stats.SenseMilliseconds, Stats.RankMilliseconds, Stats.IssueMilliseconds);
	DrawLine(Line.ToView(), BodyColor);

	if (WaitRows > 0)
	{
		DrawLine(TEXT("waiting longest"), DimColor);

		for (const FSenseWaitEntry& Wait : Stats.LongestWaits)
		{
			Line.Reset();
			Line.Appendf(TEXT("   %-24s %6.2f s   +%5.2f late   %s"),
				*Wait.Label, Wait.WaitSeconds, Wait.OverdueSeconds, TierName(Wait.Tier));
			DrawLine(Line.ToView(), Wait.OverdueSeconds > Stats.StarvationSeconds ? WarnColor : DimColor);
		}
	}
}

void USenseBudgetSubsystem::LogStats() const
{
	UE_LOG(LogSenseBudget, Display,
		TEXT("SenseBudget: perceivers %d | updates %d/%d | live %d slow %d parked %d | longest wait %.2f s, overdue %.2f s (limit %.2f s) | sense ms %.3f (rank %.3f / issue %.3f) | tiers %s%s"),
		Stats.Perceivers,
		Stats.UpdatesThisFrame, Stats.Budget,
		Stats.LiveCount, Stats.SlowCount, Stats.ParkedCount,
		Stats.LongestWaitSeconds, Stats.MostOverdueSeconds, Stats.StarvationSeconds,
		Stats.SenseMilliseconds, Stats.RankMilliseconds, Stats.IssueMilliseconds,
		Stats.bTiersEnabled ? TEXT("on") : TEXT("off"),
		Stats.bFrozen ? TEXT(" [frozen]") : TEXT(""));

	for (const FSenseWaitEntry& Wait : Stats.LongestWaits)
	{
		UE_LOG(LogSenseBudget, Display, TEXT("  waiting longest: %-32s %6.2f s   +%5.2f late   %s"),
			*Wait.Label, Wait.WaitSeconds, Wait.OverdueSeconds, SenseBudgetPrivate::TierName(Wait.Tier));
	}
}

void USenseBudgetSubsystem::LogTiers() const
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;

	UE_LOG(LogSenseBudget, Display, TEXT("SenseBudget: %d perceiver(s), budget %d per frame."), Perceivers.Num(), UpdateBudgetPerFrame);

	for (const TPair<int32, FSensePerceiverState>& Pair : Perceivers)
	{
		const FSensePerceiverState& State = Pair.Value;

		UE_LOG(LogSenseBudget, Display,
			TEXT("  #%-5d %-32s %-7s  distance %8.0f cm  wait %5.2f s  next due in %5.2f s  senses %s%s"),
			State.Id,
			*State.Label,
			SenseBudgetPrivate::TierName(State.Tier),
			DistanceToNearestViewer(State),
			FMath::Max(0.0f, Now - State.LastUpdateTime),
			FMath::Max(0.0f, State.NextDueTime - Now),
			State.bSensesOpen ? TEXT("open") : TEXT("closed"),
			State.bWakePending ? TEXT("  [wake pending]") : TEXT(""));
	}
}

//~ Console commands -----------------------------------------------------------------------------------

namespace SenseBudgetPrivate
{
	static FAutoConsoleCommandWithWorldAndArgs CmdShow(
		TEXT("Sense.Show"),
		TEXT("Sense.Show [0|1] - show or hide the counter box. No argument flips it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			USenseBudgetSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				UE_LOG(LogSenseBudget, Warning, TEXT("Sense.Show: no SenseBudget subsystem in this world."));
				return;
			}

			const bool bVisible = Args.Num() > 0 ? (FCString::Atoi(*Args[0]) != 0) : !Subsystem->AreStatsVisible();
			Subsystem->SetStatsVisible(bVisible);
			UE_LOG(LogSenseBudget, Display, TEXT("Sense.Show: counter box %s."), bVisible ? TEXT("on") : TEXT("off"));
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdBudget(
		TEXT("Sense.Budget"),
		TEXT("Sense.Budget <n> - set how many perception updates one frame may issue. No argument prints it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			USenseBudgetSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				UE_LOG(LogSenseBudget, Warning, TEXT("Sense.Budget: no SenseBudget subsystem in this world."));
				return;
			}

			if (Args.Num() == 0)
			{
				UE_LOG(LogSenseBudget, Display, TEXT("Sense.Budget: %d perception updates per frame."), Subsystem->GetBudget());
				return;
			}

			Subsystem->SetBudget(FCString::Atoi(*Args[0]));
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdStats(
		TEXT("Sense.Stats"),
		TEXT("Sense.Stats - print the measured counters to the log."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			const USenseBudgetSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				UE_LOG(LogSenseBudget, Warning, TEXT("Sense.Stats: no SenseBudget subsystem in this world."));
				return;
			}
			Subsystem->LogStats();
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdTiers(
		TEXT("Sense.Tiers"),
		TEXT("Sense.Tiers [0|1] - list every perceiver's tier in the log. An argument turns tiering off or on instead."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			USenseBudgetSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				UE_LOG(LogSenseBudget, Warning, TEXT("Sense.Tiers: no SenseBudget subsystem in this world."));
				return;
			}

			if (Args.Num() > 0)
			{
				Subsystem->SetTiersEnabled(FCString::Atoi(*Args[0]) != 0);
				return;
			}

			Subsystem->LogTiers();
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdStress(
		TEXT("Sense.Stress"),
		TEXT("Sense.Stress <n> [spread cm] - add n synthetic perceivers to the queue. Sense.Stress 0 clears them."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			USenseBudgetSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				UE_LOG(LogSenseBudget, Warning, TEXT("Sense.Stress: no SenseBudget subsystem in this world."));
				return;
			}

			const int32 Count = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 100;
			if (Count <= 0)
			{
				const int32 Cleared = Subsystem->ClearSyntheticPerceivers();
				UE_LOG(LogSenseBudget, Display, TEXT("Sense.Stress: %d synthetic perceiver(s) cleared."), Cleared);
				return;
			}

			const float Spread = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 12000.0f;
			Subsystem->AddSyntheticPerceivers(Count, Spread);
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdFreeze(
		TEXT("Sense.Freeze"),
		TEXT("Sense.Freeze [0|1] - stop the scheduler where it stands, for a screenshot. No argument flips it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			USenseBudgetSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				UE_LOG(LogSenseBudget, Warning, TEXT("Sense.Freeze: no SenseBudget subsystem in this world."));
				return;
			}

			const bool bFreeze = Args.Num() > 0 ? (FCString::Atoi(*Args[0]) != 0) : !Subsystem->IsFrozen();
			Subsystem->SetFrozen(bFreeze);
		}));
}
