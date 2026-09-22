// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "SenseBudget.h"
#include "SenseBudgetLog.h"

DEFINE_LOG_CATEGORY(LogSenseBudget);

#define LOCTEXT_NAMESPACE "FSenseBudgetModule"

void FSenseBudgetModule::StartupModule()
{
	UE_LOG(LogSenseBudget, Log, TEXT("SenseBudget started."));
}

void FSenseBudgetModule::ShutdownModule()
{
	UE_LOG(LogSenseBudget, Log, TEXT("SenseBudget shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSenseBudgetModule, SenseBudget)
