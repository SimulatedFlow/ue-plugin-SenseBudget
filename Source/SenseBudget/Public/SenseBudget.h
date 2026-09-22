// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Runtime module for SenseBudget.
 *
 * Loads at PreDefault so the world subsystem, the component class and the Sense.* console commands all
 * exist before the first game world is created. A guard placed in a map that is loaded on startup has to
 * be able to register on its very first BeginPlay - a perceiver that registers one frame late is a
 * perceiver that ran an unbudgeted perception update, which is exactly the frame this plugin exists to
 * flatten.
 */
class FSenseBudgetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
