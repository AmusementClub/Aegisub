#pragma once

struct AppRuntimeInitOptions;

void InitializeRuntimePathsAndOptions(AppRuntimeInitOptions const& options);
void InitializeRuntimeLoggingAndPerfTrace();
void CleanupRuntimeProcessState();
