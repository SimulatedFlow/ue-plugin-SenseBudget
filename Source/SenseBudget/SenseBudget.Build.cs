// Copyright 2026 Silvan Teufel. All Rights Reserved.

using UnrealBuildTool;

public class SenseBudget : ModuleRules
{
	public SenseBudget(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// One runtime module and nothing else.
		//
		// Deliberately NOT here:
		//   UMG      - the counter box is drawn on UCanvas from AHUD so it survives a cooked Shipping
		//              build. A plugin whose entire claim is a number cannot afford that number to be
		//              stripped in the build that ships. The demo map's buttons are UMG assets in
		//              Content that call the Blueprint library, exactly as a project would.
		//   UnrealEd - everything here ships. There is no editor module, so nothing can go missing
		//              between what a designer places in the editor and what the packaged game runs.
		//   Niagara / Chaos - nothing here is a particle and nothing here touches physics.
		//
		// AIModule is the one dependency with teeth, and it is held at arm's length on purpose. The only
		// engine perception API this plugin touches is the public, BlueprintCallable surface of
		// UAIPerceptionComponent - SetSenseEnabled, IsSenseEnabled, RequestStimuliListenerUpdate and
		// GetSensesConfigIterator - plus UAISenseConfig::GetSenseImplementation. Nothing from
		// AIModule/Private is included, no sense subclass is subclassed, and no internal container is
		// reached into. The includes themselves are additionally guarded with __has_include in
		// SenseBudgetSubsystem.cpp: if a future engine version moves those headers, the scheduler keeps
		// compiling and keeps ranking, and only the issuing of perception updates falls away with a
		// compile-time notice, instead of the whole plugin failing to build.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule",
			"GameplayTasks",
			"DeveloperSettings",
		});

		// RenderCore gives us GWhiteTexture, the one-pixel texture the counter box is tiled from.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
		});
	}
}
