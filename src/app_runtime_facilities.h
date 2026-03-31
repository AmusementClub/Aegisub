#pragma once

struct AppRuntimeInitOptions;

void InitializeRuntimeOptionalFacilities(AppRuntimeInitOptions const& options);
void CleanupRuntimeOptionalFacilities();
