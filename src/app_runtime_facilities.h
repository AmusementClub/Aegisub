#pragma once

struct AppRuntimeInitOptions;

void InitializeRuntimeOptionalFacilities(AppRuntimeInitOptions const& options);
void ShutdownRuntimeApplicationServices();
void CleanupRuntimeOptionalFacilities();
