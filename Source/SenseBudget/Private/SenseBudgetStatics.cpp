// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SenseBudgetStatics.h"

#include "SenseBudgetSubsystem.h"
#include "SensePriorityProfile.h"

// Needed only so a UAIPerceptionComponent* can be used as a world context: without the definition the
// compiler cannot know it derives from UObject, and an upcast from a forward declaration is not a thing.
// Guarded like every other AIModule include in this plugin - see SenseBudgetSubsystem.cpp for why.
#if __has_include("Perception/AIPerceptionComponent.h")
#include "Perception/AIPerceptionComponent.h"
#define SENSEBUDGET_WITH_AIPERCEPTION 1
#else
#define SENSEBUDGET_WITH_AIPERCEPTION 0
#endif

#define LOCTEXT_NAMESPACE "SenseBudget"

namespace SenseBudgetStaticsPrivate
{
	/** Anything not finite is treated as the dullest possible value rather than allowed to poison a sort. */
	static float Sanitise(const float Value, const float Fallback = 0.0f)
	{
		return FMath::IsFinite(Value) ? Value : Fallback;
	}

	/** Profile defaults, repeated here so a null profile behaves like an ordinary one instead of like none. */
	static constexpr float FallbackSlowInterval = 0.5f;
	static constexpr float FallbackParkedInterval = 3.0f;
}

//~ The maths ----------------------------------------------------------------------------------------------

TArray<FSenseRankEntry> USenseBudgetStatics::RankPerceivers(const TArray<FSenseRankEntry>& Entries, const FSenseRankWeights& Weights)
{
	using namespace SenseBudgetStaticsPrivate;

	TArray<FSenseRankEntry> Ranked = Entries;

	const float ReferenceDistance = FMath::Max(1.0f, Sanitise(Weights.ReferenceDistance, 1500.0f));
	const float StarvationLine = FMath::Max(0.01f, Sanitise(Weights.StarvationTriggerSeconds, 1.0f));

	for (FSenseRankEntry& Entry : Ranked)
	{
		const float Distance = FMath::Max(0.0f, Sanitise(Entry.DistanceToViewer));
		const float Importance = FMath::Max(0.0f, Sanitise(Entry.Importance, 1.0f));

		// Lateness, not time-since-last-look. A Parked guard on a three-second rate that is looked at
		// every three seconds is not being neglected; a Live guard that missed two frames is.
		const float Overdue = FMath::Max(0.0f, Sanitise(Entry.OverdueSeconds));

		// Who matters. Distance falls off hyperbolically rather than linearly, because the difference
		// between five metres and ten metres is worth far more than the difference between fifty and a
		// hundred, and a linear term spends its whole range on distances nobody can see.
		const float DistanceTerm = FMath::Max(0.0f, Weights.DistanceWeight) / (1.0f + Distance / ReferenceDistance);
		const float VisibilityTerm = Entry.bRecentlyRendered ? FMath::Max(0.0f, Weights.VisibilityBonus) : 0.0f;

		float Score = FMath::Max(0.0f, Weights.ImportanceWeight) * Importance * (DistanceTerm + VisibilityTerm);

		// The gentle half of the rotation: being late is worth something to everybody, all the time.
		Score += FMath::Max(0.0f, Weights.WaitWeight) * Overdue;

		// The hard half. Past the line a perceiver stops competing on merit and jumps the entire queue,
		// and the extra Overdue keeps the late ones ordered among themselves so the latest goes first -
		// which makes the queue above the line first-in-first-out, and that is what makes the worst case
		// a number instead of a hope: trigger, plus however long it takes to drain the line at Budget a
		// frame.
		if (Overdue >= StarvationLine)
		{
			Score += FMath::Max(0.0f, Weights.StarvationBonus) + Overdue;
		}

		// Parked is not blind. A perceiver with an event pending goes in front of everything, including
		// the starving ones - a shot that happened this frame outranks a guard that has been idle for
		// three seconds.
		if (Entry.bWakePending)
		{
			Score += FMath::Max(0.0f, Weights.WakeBonus);
		}

		Entry.Score = Score;
	}

	// Ties broken by id, so the order is stable from frame to frame. An unstable order would make the
	// "waiting longest" list flicker and would make two screenshots of the same scene disagree.
	Ranked.Sort([](const FSenseRankEntry& A, const FSenseRankEntry& B)
	{
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		return A.Id < B.Id;
	});

	return Ranked;
}

TArray<FSenseRankEntry> USenseBudgetStatics::AssignTiers(const TArray<FSenseRankEntry>& Ranked, const int32 Budget)
{
	TArray<FSenseRankEntry> Tiered = Ranked;

	const int32 LiveSlots = FMath::Max(0, Budget);
	int32 LiveUsed = 0;

	for (FSenseRankEntry& Entry : Tiered)
	{
		// A park distance of zero or less means "never park this one by distance" - an objective guard, a
		// scripted ambush, anything that has to keep reacting wherever the player is.
		const bool bBeyondPark = Entry.ParkDistance > 0.0f && Entry.DistanceToViewer > Entry.ParkDistance;

		// A PENDING WAKE BEATS THE DISTANCE (07.09.2026).
		//
		// Until this line existed, the promise "Parked is not blind" was only half true, and the half that
		// was missing is the half people buy. RankPerceivers gives a woken perceiver a large score bonus -
		// but the bonus only decides ORDER, and order stops mattering the moment the tier is forced here.
		// A guard beyond its park distance therefore stayed Parked no matter what happened next to it, and
		// only noticed on its own three-second interval.
		//
		// Found because the demo could not photograph the claim: the screenshot pass reported that no
		// parked guard ever lit up after a shot, and traced it to exactly this branch. The demo profiles
		// hid it further by having WakeRadius below ParkDistance, so a shot never even reached a parked
		// guard - but that is a content accident on top of a code fault, and the code fault is this one.
		//
		// The wake is still gated: ShouldWake() has already decided that this reason and this distance are
		// worth reacting to. What was wrong was letting distance overrule a decision that had been made.
		if (bBeyondPark && !Entry.bWakePending)
		{
			Entry.Tier = ESenseTier::Parked;
			continue;
		}

		if (LiveUsed < LiveSlots)
		{
			Entry.Tier = ESenseTier::Live;
			++LiveUsed;
			continue;
		}

		// Out of budget but still inside its park distance: slower, not off. This is the tier that makes
		// the difference between this plugin and switching distant perception off.
		Entry.Tier = ESenseTier::Slow;
	}

	return Tiered;
}

bool USenseBudgetStatics::ShouldWake(const USensePriorityProfile* Profile, const ESenseWakeReason Reason, const float Distance)
{
	if (!Profile)
	{
		// The subsystem substitutes its default profile long before anything gets here, so a null at this
		// point means the maths is being called directly with no rule to apply. Inventing one would be
		// worse than saying no.
		return false;
	}

	if (!Profile->AllowsWakeReason(Reason))
	{
		return false;
	}

	const float Radius = FMath::Max(0.0f, Profile->WakeRadius);
	const float Reach = FMath::IsFinite(Distance) ? FMath::Max(0.0f, Distance) : TNumericLimits<float>::Max();

	// Inclusive at the radius. A gunshot exactly on the boundary should wake somebody: the cost of a wake
	// that was not needed is one perception update, and the cost of missing one is an enemy that never
	// looked up.
	return Reach <= Radius;
}

float USenseBudgetStatics::NextDue(const ESenseTier Tier, const USensePriorityProfile* Profile, const float Now)
{
	using namespace SenseBudgetStaticsPrivate;

	const float Start = Sanitise(Now);

	if (Tier == ESenseTier::Live)
	{
		return Start;
	}

	if (Profile)
	{
		// GetIntervalForTier has already clamped the tier's interval to the profile's floor rate, so no
		// tier can schedule a look further ahead than MaxIntervalSeconds. That single clamp is the whole
		// reason Parked means "rarely" rather than "never".
		return Start + FMath::Max(0.0f, Profile->GetIntervalForTier(Tier));
	}

	// No profile: fall back to the profile class defaults rather than to something slower. A perceiver
	// somebody forgot to configure ends up ordinary, never switched off.
	return Start + (Tier == ESenseTier::Slow ? FallbackSlowInterval : FallbackParkedInterval);
}

TArray<int32> USenseBudgetStatics::SelectUpdates(const TArray<FSenseRankEntry>& Tiered, const int32 Budget, const float Now)
{
	TArray<int32> Selected;

	const int32 Ceiling = FMath::Max(0, Budget);
	if (Ceiling == 0)
	{
		return Selected;
	}

	Selected.Reserve(FMath::Min(Ceiling, Tiered.Num()));

	// Ranked order, due now, up to the ceiling. Whoever does not fit is not dropped - their wait keeps
	// growing, which is exactly what pushes them up the ranking for the next frame.
	//
	// Live is always due, whatever time is stored on the entry. Live means every frame; a perceiver that
	// was Slow last frame and has just been promoted must not have to sit out the rest of the slow
	// interval it was given while it was still unimportant.
	for (const FSenseRankEntry& Entry : Tiered)
	{
		if (Selected.Num() >= Ceiling)
		{
			break;
		}

		if (Entry.Id == INDEX_NONE)
		{
			continue;
		}

		if (Entry.Tier == ESenseTier::Live || Entry.NextDueTime <= Now)
		{
			Selected.Add(Entry.Id);
		}
	}

	return Selected;
}

//~ Blueprint entry points ---------------------------------------------------------------------------------

FSenseBudgetStats USenseBudgetStatics::GetSenseBudgetStats(const UObject* WorldContextObject)
{
	if (const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject))
	{
		return Subsystem->GetStats();
	}
	return FSenseBudgetStats();
}

void USenseBudgetStatics::SetSenseBudget(const UObject* WorldContextObject, const int32 UpdatesPerFrame)
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject))
	{
		Subsystem->SetBudget(UpdatesPerFrame);
	}
}

int32 USenseBudgetStatics::GetSenseBudget(const UObject* WorldContextObject)
{
	const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject);
	return Subsystem ? Subsystem->GetBudget() : 0;
}

int32 USenseBudgetStatics::NudgeSensesAround(const UObject* WorldContextObject, const FVector Location, const float Radius, const ESenseWakeReason Reason)
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject))
	{
		return Subsystem->NudgeAround(Location, Radius, Reason);
	}
	return 0;
}

void USenseBudgetStatics::SetSenseTiersEnabled(const UObject* WorldContextObject, const bool bEnabled)
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject))
	{
		Subsystem->SetTiersEnabled(bEnabled);
	}
}

bool USenseBudgetStatics::AreSenseTiersEnabled(const UObject* WorldContextObject)
{
	const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject);
	return Subsystem ? Subsystem->AreTiersEnabled() : false;
}

void USenseBudgetStatics::SetSenseBudgetFrozen(const UObject* WorldContextObject, const bool bFrozen)
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject))
	{
		Subsystem->SetFrozen(bFrozen);
	}
}

void USenseBudgetStatics::SetSenseStatsVisible(const UObject* WorldContextObject, const bool bVisible)
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(WorldContextObject))
	{
		Subsystem->SetStatsVisible(bVisible);
	}
}

ESenseTier USenseBudgetStatics::GetTierForPerception(const UAIPerceptionComponent* Perception)
{
#if SENSEBUDGET_WITH_AIPERCEPTION
	if (const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(Perception))
	{
		return Subsystem->GetTier(Perception);
	}
#endif
	return ESenseTier::Parked;
}

FLinearColor USenseBudgetStatics::GetTierColor(const ESenseTier Tier)
{
	// Three brightnesses of one hue rather than three different hues. The tier is a rate, not a category,
	// so it should read as "more" and "less" at a glance, and it stays legible for a colour-blind reader
	// and in a greyscale screenshot.
	switch (Tier)
	{
	case ESenseTier::Live:
		return FLinearColor(1.00f, 0.86f, 0.35f, 1.0f);

	case ESenseTier::Slow:
		return FLinearColor(0.42f, 0.36f, 0.16f, 1.0f);

	case ESenseTier::Parked:
	default:
		return FLinearColor(0.10f, 0.09f, 0.06f, 1.0f);
	}
}

FText USenseBudgetStatics::GetTierDisplayName(const ESenseTier Tier)
{
	switch (Tier)
	{
	case ESenseTier::Live:
		return LOCTEXT("TierLive", "Live");

	case ESenseTier::Slow:
		return LOCTEXT("TierSlow", "Slow");

	case ESenseTier::Parked:
	default:
		return LOCTEXT("TierParked", "Parked");
	}
}

#undef LOCTEXT_NAMESPACE
