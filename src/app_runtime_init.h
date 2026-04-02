#pragma once

struct AppRuntimeInitOptions;

void InitializeRuntimePathsAndOptions();
void InitializeRuntimeLoggingAndPerfTrace();
void CleanupRuntimeProcessState();
