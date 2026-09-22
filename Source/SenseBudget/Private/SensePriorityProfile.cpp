// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SensePriorityProfile.h"

USensePriorityProfile::USensePriorityProfile()
{
}

FPrimaryAssetId USensePriorityProfile::GetPrimaryAssetId() const
{
	// A named type so a project can cook and load profiles by rules rather than by hard reference. Guards
	// are usually spawned, and a spawner that has to hard-reference every profile pulls the whole enemy
	// roster into memory with the first one.
	return FPrimaryAssetId(TEXT("SensePriorityProfile"), GetFName());
}

float USensePriorityProfile::GetIntervalForTier(const ESenseTier Tier) const
{
	// The floor rate wins over every tier interval. This is the one line that keeps Parked from meaning
	// blind: however lazy a designer sets ParkedIntervalSeconds, the perceiver still gets a look every
	// MaxIntervalSeconds. A profile with a shorter parked interval than the floor keeps the shorter one -
	// the clamp only ever makes a perceiver look sooner, never later.
	const float Floor = FMath::Max(0.05f, MaxIntervalSeconds);

	switch (Tier)
	{
	case ESenseTier::Live:
		return 0.0f;

	case ESenseTier::Slow:
		return FMath::Min(FMath::Max(0.0f, SlowIntervalSeconds), Floor);

	case ESenseTier::Parked:
	default:
		return FMath::Min(FMath::Max(0.0f, ParkedIntervalSeconds), Floor);
	}
}

bool USensePriorityProfile::AllowsWakeReason(const ESenseWakeReason Reason) const
{
	switch (Reason)
	{
	case ESenseWakeReason::Noise:
		return bWakeOnNoise;

	case ESenseWakeReason::Damage:
		return bWakeOnDamage;

	case ESenseWakeReason::Manual:
	case ESenseWakeReason::Script:
	default:
		// Somebody asked by hand, or a script did. A profile does not get to overrule that: the two
		// reasons a designer types out explicitly are the two reasons that must always work, otherwise
		// debugging a scripted ambush turns into hunting for a checkbox on a data asset.
		return true;
	}
}
