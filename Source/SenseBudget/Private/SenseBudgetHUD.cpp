// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SenseBudgetHUD.h"

#include "Engine/Canvas.h"
#include "SenseBudgetSubsystem.h"

ASenseBudgetHUD::ASenseBudgetHUD()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ASenseBudgetHUD::ToggleStats()
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this))
	{
		Subsystem->SetStatsVisible(!Subsystem->AreStatsVisible());
	}
}

bool ASenseBudgetHUD::AreStatsVisible() const
{
	const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this);
	return Subsystem && Subsystem->AreStatsVisible();
}

void ASenseBudgetHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	// Everything drawn is read from the subsystem on the frame it is drawn. Nothing is cached here, so the
	// box cannot claim one thing while the scheduler does another.
	const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this);
	if (!Subsystem || !Subsystem->AreStatsVisible())
	{
		return;
	}

	Subsystem->DrawStatsBox(Canvas, StatsBoxOrigin, StatsBoxWidth);
}
