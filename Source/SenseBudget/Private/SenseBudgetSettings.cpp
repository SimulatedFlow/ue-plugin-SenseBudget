// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SenseBudgetSettings.h"

USenseBudgetSettings::USenseBudgetSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("SenseBudget");

	// The promise lives on the settings, where a project sets it once and reads it back on the counter
	// box. The trigger lives inside the weights, where the pure ranking needs it without being allowed to
	// read settings. Derived here so a project that only edits the obvious one still gets what it asked
	// for, and derived again in the subsystem in case an ini overrode either half.
	RankWeights.StarvationTriggerSeconds = StarvationSeconds * StarvationTriggerFraction;
}

FName USenseBudgetSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName USenseBudgetSettings::GetSectionName() const
{
	return TEXT("SenseBudget");
}

const USenseBudgetSettings& USenseBudgetSettings::Get()
{
	const USenseBudgetSettings* Settings = GetDefault<USenseBudgetSettings>();
	check(Settings);
	return *Settings;
}
