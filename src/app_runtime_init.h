#pragma once

struct AppRuntimeInitOptions;

void InitializeRuntimePathsAndOptions();
void InitializeRuntimeLoggingAndPerfTrace();
void InitializeOptionalRuntimeFacilities(AppRuntimeInitOptions const& options);
void CleanupRuntimeProcessState();
