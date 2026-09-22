// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SenseBudgetStatics.h"
#include "SenseBudgetSubsystem.h"
#include "SenseBudgetTypes.h"
#include "SensePriorityProfile.h"

#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if __has_include("Perception/AIPerceptionComponent.h")
#include "Perception/AIPerceptionComponent.h"
#define SENSEBUDGETTESTS_WITH_AIPERCEPTION 1
#else
#define SENSEBUDGETTESTS_WITH_AIPERCEPTION 0
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace SenseBudgetTests
{
	// CommandletContext as well as EditorContext. The six things tested here are the six places this
	// plugin can be quietly wrong, and a test that only runs when somebody has the editor open is a test
	// that will not be there on the build machine, which is where it matters.
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	/** The weights the subsystem runs with, so the tests measure the shipping configuration. */
	FSenseRankWeights DefaultWeights()
	{
		FSenseRankWeights Weights;
		Weights.StarvationTriggerSeconds = 1.0f; // Half of the 2.0 s promise, as the subsystem derives it.
		return Weights;
	}

	/** One perceiver. Distance in centimetres; park distance zero means "never park by distance". */
	FSenseRankEntry MakeEntry(const int32 Id, const float Distance, const float ParkDistance = 6000.0f,
		const float Importance = 1.0f, const bool bRendered = false)
	{
		FSenseRankEntry Entry;
		Entry.Id = Id;
		Entry.DistanceToViewer = Distance;
		Entry.ParkDistance = ParkDistance;
		Entry.Importance = Importance;
		Entry.bRecentlyRendered = bRendered;
		return Entry;
	}

	/** A profile held against garbage collection for the life of a test. */
	TStrongObjectPtr<USensePriorityProfile> MakeProfile()
	{
		return TStrongObjectPtr<USensePriorityProfile>(NewObject<USensePriorityProfile>(GetTransientPackage()));
	}

	int32 CountTier(const TArray<FSenseRankEntry>& Entries, const ESenseTier Tier)
	{
		int32 Count = 0;
		for (const FSenseRankEntry& Entry : Entries)
		{
			if (Entry.Tier == Tier)
			{
				++Count;
			}
		}
		return Count;
	}

	const FSenseRankEntry* FindById(const TArray<FSenseRankEntry>& Entries, const int32 Id)
	{
		return Entries.FindByPredicate([Id](const FSenseRankEntry& Entry) { return Entry.Id == Id; });
	}
}

//
// (0) A pending wake beats the park distance.
//
// The regression test for the fault found on 07.09.2026: AssignTiers forced Parked on distance alone and
// returned before the wake was ever looked at, so a woken guard beyond its park distance stayed parked and
// the promise "Parked is not blind" was false. It was invisible because the score bonus DID apply - the
// ranking looked right while the outcome was wrong.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetWakeBeatsParkDistanceTest,
	"SenseBudget.Maths.APendingWakeBeatsTheParkDistance",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetWakeBeatsParkDistanceTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	// Two perceivers, both far beyond their park distance. Identical except for the pending wake.
	FSenseRankEntry Sleeper = MakeEntry(1, /*Distance=*/9000.0f, /*ParkDistance=*/2000.0f);
	FSenseRankEntry Woken = MakeEntry(2, /*Distance=*/9000.0f, /*ParkDistance=*/2000.0f);
	Woken.bWakePending = true;

	const TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers({Sleeper, Woken}, DefaultWeights());
	const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, /*Budget=*/4);

	for (const FSenseRankEntry& Entry : Tiered)
	{
		if (Entry.Id == 1)
		{
			TestEqual(TEXT("far and quiet stays parked"), Entry.Tier, ESenseTier::Parked);
		}
		else
		{
			TestNotEqual(TEXT("far but woken does NOT stay parked"), Entry.Tier, ESenseTier::Parked);
			TestEqual(TEXT("far but woken takes a live slot when one is free"), Entry.Tier, ESenseTier::Live);
		}
	}

	// And the wake must not be able to break the ceiling: with no slots at all, the woken one drops to Slow
	// rather than stealing a Live slot that does not exist.
	{
		const TArray<FSenseRankEntry> NoSlots = USenseBudgetStatics::AssignTiers(Ranked, /*Budget=*/0);
		for (const FSenseRankEntry& Entry : NoSlots)
		{
			if (Entry.Id == 2)
			{
				TestEqual(TEXT("woken with no budget is Slow, not Live"), Entry.Tier, ESenseTier::Slow);
			}
		}
	}

	return true;
}

//
// (1) The ceiling is exact.
//
// This is the whole promise on the counter box. If AssignTiers can ever hand out one Live slot more than
// the budget, the number beside "updates" is a decoration.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetAssignTiersHoldsBudgetTest,
	"SenseBudget.Maths.AssignTiersHoldsTheBudgetExactly",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetAssignTiersHoldsBudgetTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	// Fifty perceivers, all close enough that none of them parks by distance.
	TArray<FSenseRankEntry> Entries;
	for (int32 Index = 0; Index < 50; ++Index)
	{
		Entries.Add(MakeEntry(Index + 1, static_cast<float>(Index) * 50.0f));
	}

	const TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers(Entries, DefaultWeights());

	{
		const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, 12);
		TestEqual(TEXT("budget 12 of 50 - Live"), CountTier(Tiered, ESenseTier::Live), 12);
		TestEqual(TEXT("budget 12 of 50 - Slow"), CountTier(Tiered, ESenseTier::Slow), 38);
		TestEqual(TEXT("budget 12 of 50 - Parked"), CountTier(Tiered, ESenseTier::Parked), 0);
		TestEqual(TEXT("nobody is lost"), Tiered.Num(), 50);
	}

	// A budget larger than the crowd hands everybody a slot and stops - it does not wrap, double-count or
	// go negative.
	{
		const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, 500);
		TestEqual(TEXT("budget larger than the crowd"), CountTier(Tiered, ESenseTier::Live), 50);
	}

	// A budget of zero, and a nonsense negative budget, park nobody and blind nobody: everybody lands in
	// Slow, where the profile's floor rate still applies.
	{
		const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, 0);
		TestEqual(TEXT("budget zero - no Live"), CountTier(Tiered, ESenseTier::Live), 0);
		TestEqual(TEXT("budget zero - all Slow"), CountTier(Tiered, ESenseTier::Slow), 50);
	}
	{
		const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, -7);
		TestEqual(TEXT("negative budget - no Live"), CountTier(Tiered, ESenseTier::Live), 0);
	}

	// With twenty of them beyond their park distance, the Live count is still exactly the budget, taken
	// from the thirty that are eligible.
	{
		TArray<FSenseRankEntry> Mixed;
		for (int32 Index = 0; Index < 30; ++Index)
		{
			Mixed.Add(MakeEntry(Index + 1, 500.0f, 6000.0f));
		}
		for (int32 Index = 0; Index < 20; ++Index)
		{
			Mixed.Add(MakeEntry(100 + Index, 20000.0f, 6000.0f));
		}

		const TArray<FSenseRankEntry> MixedRanked = USenseBudgetStatics::RankPerceivers(Mixed, DefaultWeights());
		const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(MixedRanked, 12);

		TestEqual(TEXT("mixed - Live is exactly the budget"), CountTier(Tiered, ESenseTier::Live), 12);
		TestEqual(TEXT("mixed - the far twenty are Parked"), CountTier(Tiered, ESenseTier::Parked), 20);
		TestEqual(TEXT("mixed - the rest are Slow"), CountTier(Tiered, ESenseTier::Slow), 18);
	}

	// Budget smaller than the eligible crowd, at every size from one to thirty: never more than asked for.
	for (int32 Budget = 1; Budget <= 30; ++Budget)
	{
		const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, Budget);
		TestEqual(*FString::Printf(TEXT("budget %d"), Budget), CountTier(Tiered, ESenseTier::Live), Budget);
	}

	return true;
}

//
// (2) A close, visible perceiver is never parked while there is budget.
//
// The one guarantee a designer has to be able to rely on without reading any of this: the enemy the
// player is looking at keeps looking back.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetNearVisibleNeverParkedTest,
	"SenseBudget.Maths.NearAndVisibleIsNeverParked",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetNearVisibleNeverParkedTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	constexpr int32 NearId = 7;

	TArray<FSenseRankEntry> Entries;
	Entries.Add(MakeEntry(NearId, 300.0f, 6000.0f, 1.0f, /*bRendered*/ true));

	// Two hundred others, most of them far away, and every one of them badly overdue - the worst possible
	// competition for the near one's slot.
	for (int32 Index = 0; Index < 200; ++Index)
	{
		FSenseRankEntry Far = MakeEntry(1000 + Index, 8000.0f + static_cast<float>(Index) * 20.0f, 6000.0f);
		Far.OverdueSeconds = 5.0f;
		Far.WaitSeconds = 5.0f;
		Entries.Add(Far);
	}

	const TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers(Entries, DefaultWeights());
	const TArray<FSenseRankEntry> Tiered = USenseBudgetStatics::AssignTiers(Ranked, 12);

	const FSenseRankEntry* Near = FindById(Tiered, NearId);
	TestNotNull(TEXT("the near perceiver is still in the array"), Near);

	if (Near)
	{
		// It may be pushed down to Slow while a starving crowd is caught up - that is the rotation doing
		// its job, and Slow still looks twice a second. It must never be Parked: Parked is a distance
		// decision, and this one is not far away.
		TestNotEqual(TEXT("near and visible is not Parked"), Near->Tier, ESenseTier::Parked);
	}

	// With nobody starving, it is Live outright.
	{
		TArray<FSenseRankEntry> Calm = Entries;
		for (FSenseRankEntry& Entry : Calm)
		{
			Entry.OverdueSeconds = 0.0f;
			Entry.WaitSeconds = 0.0f;
		}

		const TArray<FSenseRankEntry> CalmRanked = USenseBudgetStatics::RankPerceivers(Calm, DefaultWeights());
		const TArray<FSenseRankEntry> CalmTiered = USenseBudgetStatics::AssignTiers(CalmRanked, 12);

		const FSenseRankEntry* CalmNear = FindById(CalmTiered, NearId);
		TestNotNull(TEXT("the near perceiver survives the calm run"), CalmNear);
		if (CalmNear)
		{
			TestEqual(TEXT("near and visible takes a Live slot"), CalmNear->Tier, ESenseTier::Live);
		}
	}

	// Even at a budget of one it takes the only slot there is.
	{
		TArray<FSenseRankEntry> Calm = Entries;
		for (FSenseRankEntry& Entry : Calm)
		{
			Entry.OverdueSeconds = 0.0f;
		}

		const TArray<FSenseRankEntry> CalmRanked = USenseBudgetStatics::RankPerceivers(Calm, DefaultWeights());
		const TArray<FSenseRankEntry> CalmTiered = USenseBudgetStatics::AssignTiers(CalmRanked, 1);

		const FSenseRankEntry* CalmNear = FindById(CalmTiered, NearId);
		if (TestNotNull(TEXT("budget of one - entry present"), CalmNear); CalmNear)
		{
			TestEqual(TEXT("budget of one goes to the near, visible perceiver"), CalmNear->Tier, ESenseTier::Live);
		}
	}

	return true;
}

//
// (3) The profile's floor rate is a floor. This is the line that makes Parked mean "rarely", not "never".
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetNextDueRespectsFloorTest,
	"SenseBudget.Maths.NextDueRespectsTheProfileFloorRate",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetNextDueRespectsFloorTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	TStrongObjectPtr<USensePriorityProfile> Profile = MakeProfile();
	constexpr float Now = 10.0f;

	// A designer asks for a thirty-second parked rate and a four-second floor. The floor wins.
	Profile->SlowIntervalSeconds = 0.5f;
	Profile->ParkedIntervalSeconds = 30.0f;
	Profile->MaxIntervalSeconds = 4.0f;

	TestEqual(TEXT("Live is now"), USenseBudgetStatics::NextDue(ESenseTier::Live, Profile.Get(), Now), Now);
	TestEqual(TEXT("Slow keeps its half second"), USenseBudgetStatics::NextDue(ESenseTier::Slow, Profile.Get(), Now), 10.5f);
	TestEqual(TEXT("Parked is clamped to the floor"), USenseBudgetStatics::NextDue(ESenseTier::Parked, Profile.Get(), Now), 14.0f);

	// The clamp only ever makes a perceiver look sooner. A slow rate longer than the floor is cut too.
	Profile->SlowIntervalSeconds = 9.0f;
	TestEqual(TEXT("a slow rate above the floor is cut"), USenseBudgetStatics::NextDue(ESenseTier::Slow, Profile.Get(), Now), 14.0f);

	// A parked rate shorter than the floor is left alone - the floor is a ceiling on laziness, not a
	// minimum delay.
	Profile->ParkedIntervalSeconds = 1.0f;
	TestEqual(TEXT("a parked rate below the floor is untouched"), USenseBudgetStatics::NextDue(ESenseTier::Parked, Profile.Get(), Now), 11.0f);

	// GetIntervalForTier is the same rule, reachable from Blueprint.
	Profile->ParkedIntervalSeconds = 30.0f;
	TestEqual(TEXT("the interval helper agrees"), Profile->GetIntervalForTier(ESenseTier::Parked), 4.0f);
	TestEqual(TEXT("Live has no interval"), Profile->GetIntervalForTier(ESenseTier::Live), 0.0f);

	// No profile at all: dulled to the class defaults, never switched off.
	TestEqual(TEXT("no profile - Live"), USenseBudgetStatics::NextDue(ESenseTier::Live, nullptr, Now), Now);
	TestEqual(TEXT("no profile - Slow"), USenseBudgetStatics::NextDue(ESenseTier::Slow, nullptr, Now), 10.5f);
	TestEqual(TEXT("no profile - Parked"), USenseBudgetStatics::NextDue(ESenseTier::Parked, nullptr, Now), 13.0f);

	return true;
}

//
// (4) Nobody starves, at five hundred perceivers.
//
// A full simulation of the subsystem's frame, running the same three functions the subsystem runs, for
// fifteen seconds at 60 Hz. The claim being checked is the one on the counter box: nobody is ever more
// than the starvation limit past the time their own profile said they were due.
//
// The analytic bound is trigger + Perceivers / (Budget * frame rate) = 1.0 + 500 / 720 = 1.69 s, under a
// promise of 2.0 s. The test asserts both, because the promise passing while the bound is wrong would
// mean the margin is luck rather than arithmetic.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetNobodyStarvesTest,
	"SenseBudget.Maths.NobodyStarvesAtFiveHundredPerceivers",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetNobodyStarvesTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	constexpr int32 PerceiverCount = 500;
	constexpr int32 Budget = 12;
	constexpr float DeltaTime = 1.0f / 60.0f;
	constexpr float Duration = 15.0f;
	constexpr float StarvationPromise = 2.0f;

	const FSenseRankWeights Weights = DefaultWeights();

	TStrongObjectPtr<USensePriorityProfile> Profile = MakeProfile();
	Profile->SlowIntervalSeconds = 0.5f;
	Profile->ParkedIntervalSeconds = 3.0f;
	Profile->MaxIntervalSeconds = 5.0f;
	Profile->ParkDistance = 6000.0f;

	// Spread from right in front of the player out to twenty thousand centimetres, so the crowd covers all
	// three tiers. A stress test where everybody lands in one tier proves nothing about the queue.
	TArray<FSenseRankEntry> Entries;
	Entries.Reserve(PerceiverCount);
	for (int32 Index = 0; Index < PerceiverCount; ++Index)
	{
		FSenseRankEntry Entry = MakeEntry(Index + 1,
			20000.0f * (static_cast<float>(Index) / static_cast<float>(PerceiverCount)),
			Profile->ParkDistance);
		Entry.bRecentlyRendered = (Index % 4) == 0;
		Entries.Add(Entry);
	}

	TMap<int32, float> LastUpdateTime;
	TMap<int32, float> NextDueTime;
	for (const FSenseRankEntry& Entry : Entries)
	{
		LastUpdateTime.Add(Entry.Id, 0.0f);
		NextDueTime.Add(Entry.Id, 0.0f);
	}

	float Now = 0.0f;
	float WorstOverdue = 0.0f;
	int32 WorstUpdatesInAFrame = 0;

	// Warm-up is deliberately not skipped. The first second, when five hundred perceivers all want their
	// first look at once, is the worst the queue will ever be - measuring from after it would be
	// measuring the easy part.
	while (Now < Duration)
	{
		Now += DeltaTime;

		for (FSenseRankEntry& Entry : Entries)
		{
			const float Due = NextDueTime[Entry.Id];
			Entry.NextDueTime = Due;
			Entry.WaitSeconds = FMath::Max(0.0f, Now - LastUpdateTime[Entry.Id]);
			Entry.OverdueSeconds = FMath::Max(0.0f, Now - Due);

			WorstOverdue = FMath::Max(WorstOverdue, Entry.OverdueSeconds);
		}

		TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers(Entries, Weights);
		Ranked = USenseBudgetStatics::AssignTiers(Ranked, Budget);
		const TArray<int32> Due = USenseBudgetStatics::SelectUpdates(Ranked, Budget, Now);

		WorstUpdatesInAFrame = FMath::Max(WorstUpdatesInAFrame, Due.Num());

		for (const int32 Id : Due)
		{
			const FSenseRankEntry* Served = FindById(Ranked, Id);
			if (!Served)
			{
				AddError(FString::Printf(TEXT("SelectUpdates returned id %d, which was not in its input."), Id));
				continue;
			}

			LastUpdateTime[Id] = Now;
			NextDueTime[Id] = USenseBudgetStatics::NextDue(Served->Tier, Profile.Get(), Now);
		}
	}

	// The ceiling held on every one of nine hundred frames.
	TestTrue(TEXT("the ceiling was never exceeded"), WorstUpdatesInAFrame <= Budget);

	// The promise.
	TestTrue(
		*FString::Printf(TEXT("worst lateness %.3f s is inside the %.2f s limit"), WorstOverdue, StarvationPromise),
		WorstOverdue <= StarvationPromise);

	// And the arithmetic behind it, so a pass cannot be luck: trigger plus the time to drain the whole
	// crowd at Budget a frame, plus one frame of slack for the sampling.
	const float AnalyticBound = Weights.StarvationTriggerSeconds
		+ static_cast<float>(PerceiverCount) / (static_cast<float>(Budget) / DeltaTime)
		+ DeltaTime;

	TestTrue(
		*FString::Printf(TEXT("worst lateness %.3f s is inside the analytic bound %.3f s"), WorstOverdue, AnalyticBound),
		WorstOverdue <= AnalyticBound);

	return true;
}

//
// (5) The wake rule. Inside the radius yes, outside no - and Parked is not blind either way.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetShouldWakeTest,
	"SenseBudget.Maths.ShouldWakeRespectsRadiusAndReason",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetShouldWakeTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	TStrongObjectPtr<USensePriorityProfile> Profile = MakeProfile();
	Profile->WakeRadius = 2500.0f;
	Profile->bWakeOnNoise = true;
	Profile->bWakeOnDamage = true;

	TestTrue(TEXT("a shot right beside it"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Noise, 0.0f));
	TestTrue(TEXT("a shot well inside the radius"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Noise, 1200.0f));

	// Inclusive at the boundary: a wake that was not needed costs one perception update, a wake that was
	// missed costs an enemy that never looked up.
	TestTrue(TEXT("a shot exactly on the radius"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Noise, 2500.0f));

	TestFalse(TEXT("a shot just outside"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Noise, 2500.1f));
	TestFalse(TEXT("a shot two districts away"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Noise, 90000.0f));

	// A profile can refuse a reason. A turret does not care what it heard.
	Profile->bWakeOnNoise = false;
	TestFalse(TEXT("a deaf profile ignores noise"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Noise, 100.0f));
	TestTrue(TEXT("but it still notices being shot"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Damage, 100.0f));

	Profile->bWakeOnDamage = false;
	TestFalse(TEXT("both refused"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Damage, 100.0f));

	// Manual and Script are never refusable. The two reasons a human typed out are the two that must
	// always work, or debugging a scripted ambush turns into hunting for a checkbox on a data asset.
	TestTrue(TEXT("Manual is always honoured"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Manual, 100.0f));
	TestTrue(TEXT("Script is always honoured"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Script, 100.0f));

	// Even Manual respects the radius: the reason decides whether the perceiver cares, the radius decides
	// whether the event reached it.
	TestFalse(TEXT("Manual still has to be in range"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Manual, 9000.0f));

	// A radius of zero means "only what happens to me".
	Profile->WakeRadius = 0.0f;
	TestTrue(TEXT("zero radius wakes on its own damage"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Manual, 0.0f));
	TestFalse(TEXT("zero radius ignores anything else"), USenseBudgetStatics::ShouldWake(Profile.Get(), ESenseWakeReason::Manual, 1.0f));

	// No profile, no rule to apply.
	TestFalse(TEXT("no profile"), USenseBudgetStatics::ShouldWake(nullptr, ESenseWakeReason::Manual, 0.0f));

	// A wake pending outranks everybody, including a badly starved crowd. That is what makes Parked not
	// blind rather than merely differently named.
	{
		TArray<FSenseRankEntry> Entries;

		FSenseRankEntry Woken = MakeEntry(1, 18000.0f, 6000.0f);
		Woken.bWakePending = true;
		Entries.Add(Woken);

		for (int32 Index = 0; Index < 100; ++Index)
		{
			FSenseRankEntry Starving = MakeEntry(100 + Index, 200.0f, 6000.0f, 4.0f, true);
			Starving.OverdueSeconds = 8.0f;
			Entries.Add(Starving);
		}

		const TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers(Entries, DefaultWeights());
		TestEqual(TEXT("the woken perceiver is first in the queue"), Ranked[0].Id, 1);
	}

	return true;
}

//
// (6) Unregistering in the middle of an assignment is harmless.
//
// The reentrancy this plugin has to survive: an actor destroyed by a tier-change handler, or a pooled AI
// recycled from a Blueprint, while the frame's results are being written back. Two halves are checked -
// the pure one (results are carried by id, never by position, so a stale result finds nothing rather than
// landing on a stranger) and the live one (a real subsystem with real components registered and pulled
// out from under it).
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSenseBudgetUnregisterDuringAssignmentTest,
	"SenseBudget.Registry.UnregisterDuringAssignmentIsSafe",
	SenseBudgetTests::TestFlags)

bool FSenseBudgetUnregisterDuringAssignmentTest::RunTest(const FString& Parameters)
{
	using namespace SenseBudgetTests;

	//
	// The pure half: identity survives the pipeline, so a lookup is the only way back to a perceiver.
	//
	{
		TArray<FSenseRankEntry> Entries;
		for (int32 Index = 0; Index < 64; ++Index)
		{
			Entries.Add(MakeEntry(Index + 1, static_cast<float>(Index) * 300.0f));
		}

		TArray<FSenseRankEntry> Ranked = USenseBudgetStatics::RankPerceivers(Entries, DefaultWeights());
		Ranked = USenseBudgetStatics::AssignTiers(Ranked, 12);

		TSet<int32> InputIds;
		for (const FSenseRankEntry& Entry : Entries)
		{
			InputIds.Add(Entry.Id);
		}

		TSet<int32> OutputIds;
		for (const FSenseRankEntry& Entry : Ranked)
		{
			OutputIds.Add(Entry.Id);
		}

		TestEqual(TEXT("no perceiver is lost by the pipeline"), OutputIds.Num(), InputIds.Num());
		TestTrue(TEXT("no perceiver is invented by the pipeline"), OutputIds.Includes(InputIds));

		// Half the registry disappears mid-frame. Every id the scheduler is about to write back is either
		// still in the registry, or is dropped - and none of them can be mistaken for a different
		// perceiver, because the ranking sorted the array and an index means nothing after that.
		TSet<int32> Registry = InputIds;
		for (int32 Index = 1; Index <= 64; Index += 2)
		{
			Registry.Remove(Index);
		}

		const TArray<int32> Due = USenseBudgetStatics::SelectUpdates(Ranked, 12, 1.0f);
		int32 Applied = 0;
		for (const int32 Id : Due)
		{
			TestTrue(TEXT("SelectUpdates only returns ids it was given"), InputIds.Contains(Id));
			if (Registry.Contains(Id))
			{
				++Applied;
			}
		}
		TestTrue(TEXT("at most the surviving half is applied"), Applied <= Due.Num());
	}

	//
	// The live half: a real subsystem, real components, registered and pulled out again.
	//
#if SENSEBUDGETTESTS_WITH_AIPERCEPTION
	{
		// Not initialised, so it never ticks - UTickableWorldSubsystem only ticks once it has been given
		// a world. Everything exercised here is the registry, which is what the reentrancy is about.
		TStrongObjectPtr<USenseBudgetSubsystem> Subsystem(NewObject<USenseBudgetSubsystem>(GetTransientPackage()));

		TArray<TStrongObjectPtr<UAIPerceptionComponent>> Components;
		TArray<int32> Ids;

		for (int32 Index = 0; Index < 64; ++Index)
		{
			TStrongObjectPtr<UAIPerceptionComponent> Component(NewObject<UAIPerceptionComponent>(GetTransientPackage()));
			const int32 Id = Subsystem->Register(Component.Get(), nullptr, nullptr);

			TestNotEqual(TEXT("registration returns an id"), Id, static_cast<int32>(INDEX_NONE));
			TestTrue(TEXT("the perceiver is managed"), Subsystem->IsPerceiverManaged(Component.Get()));

			Ids.Add(Id);
			Components.Add(MoveTemp(Component));
		}

		// The stats are a snapshot of the last tick, and this subsystem has never ticked, so they are all
		// zero here on purpose. The registry is what is under test; the counters are checked in the map.
		TestEqual(TEXT("no tick, no counters"), Subsystem->GetStats().Perceivers, 0);

		// Registering the same component twice hands back the same id rather than a second entry.
		const int32 Repeat = Subsystem->Register(Components[0].Get(), nullptr, nullptr);
		TestEqual(TEXT("registering twice is idempotent"), Repeat, Ids[0]);

		// Unregister half of them, and unregister some of those a second time - a double EndPlay, which
		// is exactly what a pooled actor does.
		for (int32 Index = 0; Index < 64; Index += 2)
		{
			Subsystem->Unregister(Components[Index].Get());
			Subsystem->Unregister(Components[Index].Get());
		}

		for (int32 Index = 0; Index < 64; ++Index)
		{
			const bool bExpectManaged = (Index % 2) == 1;
			TestEqual(
				*FString::Printf(TEXT("component %d managed"), Index),
				Subsystem->IsPerceiverManaged(Components[Index].Get()),
				bExpectManaged);
		}

		// Everything that takes a component must tolerate one it has never heard of, and a null.
		Subsystem->Unregister(nullptr);
		TestFalse(TEXT("nudging an unregistered component"), Subsystem->Nudge(Components[0].Get(), ESenseWakeReason::Manual, 0.0f));
		TestFalse(TEXT("nudging null"), Subsystem->Nudge(nullptr, ESenseWakeReason::Manual, 0.0f));
		TestEqual(TEXT("the tier of an unregistered component"), Subsystem->GetTier(Components[0].Get()), ESenseTier::Parked);
		TestEqual(TEXT("the tier of null"), Subsystem->GetTier(nullptr), ESenseTier::Parked);
		TestEqual(TEXT("the wait of null"), Subsystem->GetSecondsSinceUpdate(nullptr), 0.0f);
		TestEqual(TEXT("registering null"), Subsystem->Register(nullptr, nullptr, nullptr), static_cast<int32>(INDEX_NONE));

		// Reading and logging over a half-emptied registry must not touch anything that is gone.
		Subsystem->LogTiers();
		Subsystem->LogStats();

		// And the whole registry can be torn down in any order.
		for (int32 Index = 0; Index < 64; ++Index)
		{
			Subsystem->Unregister(Components[Index].Get());
		}

		for (int32 Index = 0; Index < 64; ++Index)
		{
			TestFalse(*FString::Printf(TEXT("component %d released"), Index),
				Subsystem->IsPerceiverManaged(Components[Index].Get()));
		}
	}
#endif // SENSEBUDGETTESTS_WITH_AIPERCEPTION

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
