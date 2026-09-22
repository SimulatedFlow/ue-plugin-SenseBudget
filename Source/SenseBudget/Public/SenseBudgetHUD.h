// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "SenseBudgetHUD.generated.h"

class UCanvas;

/**
 * The counter box for SenseBudget, drawn on UCanvas from AHUD.
 *
 * Canvas rather than UMG, and no buttons on it. Two reasons, pulling the same way:
 *
 *   - The box has to survive a cooked Shipping build. DrawDebug is compiled out there and a debug widget
 *     is usually stripped; a Canvas overlay is not. For a plugin whose entire claim is a number, a number
 *     that disappears in the build that ships is a number nobody can check.
 *
 *   - Anything that has to be clicked belongs in UMG. An AHUD hit box is tested against
 *     UGameViewportClient::GetMousePosition, which reports nothing at all on a machine with no mouse
 *     attached - a capture rig, a build agent, a headless test - so the click never lands. Widgets are
 *     not affected. The numbers live here, where they cost nothing and always draw; controls live in a
 *     widget, where they always receive the click.
 *
 * A project that already has a HUD class does not have to reparent it: turn on bAutoDrawStatsOnAnyHUD in
 * Project Settings and the same box is drawn through AHUD::OnHUDPostRender instead. The two paths know
 * about each other and cannot stack.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Sense Budget HUD"))
class SENSEBUDGET_API ASenseBudgetHUD : public AHUD
{
	GENERATED_BODY()

public:
	ASenseBudgetHUD();

	//~ AHUD interface
	virtual void DrawHUD() override;

	/** Top-left corner of the counter box, in pixels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget|HUD")
	FVector2D StatsBoxOrigin = FVector2D(28.0f, 90.0f);

	/** Width of the counter box, in pixels. Wide enough for the "waiting longest" names. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SenseBudget|HUD", meta = (ClampMin = "260.0"))
	float StatsBoxWidth = 470.0f;

	/** Flip the counter box on and off. This is what a SHOW button in a widget calls. */
	UFUNCTION(BlueprintCallable, Category = "SenseBudget|HUD")
	void ToggleStats();

	/** Whether the counter box is drawing. Reads the subsystem, which is the one source of truth. */
	UFUNCTION(BlueprintPure, Category = "SenseBudget|HUD")
	bool AreStatsVisible() const;
};
