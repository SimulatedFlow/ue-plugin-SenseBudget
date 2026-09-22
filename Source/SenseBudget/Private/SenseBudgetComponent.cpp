// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SenseBudgetComponent.h"

#include "GameFramework/Actor.h"
#include "SenseBudgetLog.h"
#include "SenseBudgetStatics.h"
#include "SenseBudgetSubsystem.h"

#if __has_include("Perception/AIPerceptionComponent.h")
#include "Perception/AIPerceptionComponent.h"
#define SENSEBUDGET_WITH_AIPERCEPTION 1
#else
#define SENSEBUDGET_WITH_AIPERCEPTION 0
#endif

USenseBudgetComponent::USenseBudgetComponent()
{
	// Nothing here ticks. The whole point of the plugin is that the scheduling happens once, for
	// everybody, in one place - a per-actor tick would put a hundred tick functions back on the frame
	// this plugin was bought to shorten.
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void USenseBudgetComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoRegister)
	{
		RegisterWithBudget();
	}
}

void USenseBudgetComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromBudget();

	Super::EndPlay(EndPlayReason);
}

UAIPerceptionComponent* USenseBudgetComponent::GetManagedPerception() const
{
	if (PerceptionOverride)
	{
		return PerceptionOverride;
	}

#if SENSEBUDGET_WITH_AIPERCEPTION
	if (const AActor* MyOwner = GetOwner())
	{
		return MyOwner->FindComponentByClass<UAIPerceptionComponent>();
	}
#endif

	return nullptr;
}

bool USenseBudgetComponent::RegisterWithBudget()
{
	if (bManaged)
	{
		return true;
	}

	UAIPerceptionComponent* Perception = GetManagedPerception();
	if (!Perception)
	{
		// Worth a warning rather than a silent no-op: a SenseBudget component on an actor with no
		// perception component is a mistake somebody made in the editor, and the only place they will
		// ever see it is the log.
		UE_LOG(LogSenseBudget, Warning,
			TEXT("SenseBudget: '%s' has a SenseBudgetComponent but no AIPerceptionComponent. Nothing to pace."),
			*GetNameSafe(GetOwner()));
		return false;
	}

	USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this);
	if (!Subsystem)
	{
		return false;
	}

	const int32 Id = Subsystem->Register(Perception, Profile, this);
	bManaged = Id != INDEX_NONE;

	if (bManaged)
	{
		LastReportedTier = Subsystem->GetTier(Perception);
	}

	return bManaged;
}

void USenseBudgetComponent::UnregisterFromBudget()
{
	if (!bManaged)
	{
		return;
	}

	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this))
	{
		// The subsystem puts the senses back the way it found them. A perceiver that leaves the scheduler
		// mid-window would otherwise be left with its sight switched off by the plugin that promised
		// never to blind anybody.
		Subsystem->Unregister(GetManagedPerception());
	}

	bManaged = false;
}

bool USenseBudgetComponent::IsManagedBySenseBudget() const
{
	return bManaged;
}

ESenseTier USenseBudgetComponent::GetTier() const
{
	if (const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this))
	{
		return Subsystem->GetTier(GetManagedPerception());
	}
	return ESenseTier::Parked;
}

float USenseBudgetComponent::GetSecondsSinceLastUpdate() const
{
	if (const USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this))
	{
		return Subsystem->GetSecondsSinceUpdate(GetManagedPerception());
	}
	return 0.0f;
}

FLinearColor USenseBudgetComponent::GetTierDisplayColor() const
{
	return USenseBudgetStatics::GetTierColor(GetTier());
}

bool USenseBudgetComponent::WakeUp(const ESenseWakeReason Reason, const float Distance)
{
	if (USenseBudgetSubsystem* Subsystem = USenseBudgetSubsystem::Get(this))
	{
		return Subsystem->Nudge(GetManagedPerception(), Reason, Distance);
	}
	return false;
}

void USenseBudgetComponent::NotifyTierChanged(const ESenseTier NewTier, const ESenseTier OldTier)
{
	if (NewTier == LastReportedTier)
	{
		return;
	}

	LastReportedTier = NewTier;
	OnTierChanged.Broadcast(NewTier, OldTier);
}
