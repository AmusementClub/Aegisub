find_program(AEGISUB_DOTNET_EXECUTABLE NAMES dotnet REQUIRED)
get_filename_component(_aegisub_dotnet_executable_realpath "${AEGISUB_DOTNET_EXECUTABLE}" REALPATH)
get_filename_component(_aegisub_dotnet_root "${_aegisub_dotnet_executable_realpath}" DIRECTORY)

execute_process(
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" --list-sdks
    OUTPUT_VARIABLE _aegisub_dotnet_sdks
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _aegisub_dotnet_sdks_result
)
if(NOT _aegisub_dotnet_sdks_result EQUAL 0 OR NOT _aegisub_dotnet_sdks MATCHES "(^|\n)10\\.")
    message(FATAL_ERROR "WITH_PLUGIN_BRIDGE currently requires a .NET 10 SDK")
endif()

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_aegisub_dotnet_rid win-x86)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_aegisub_dotnet_rid win-arm64)
    else()
        set(_aegisub_dotnet_rid win-x64)
    endif()
    set(_aegisub_nethost_filename nethost.dll)
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_aegisub_dotnet_rid osx-arm64)
    else()
        set(_aegisub_dotnet_rid osx-x64)
    endif()
    set(_aegisub_nethost_filename libnethost.dylib)
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_aegisub_dotnet_rid linux-arm64)
    else()
        set(_aegisub_dotnet_rid linux-x64)
    endif()
    set(_aegisub_nethost_filename libnethost.so)
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "WITH_PLUGIN_BRIDGE requires a 64-bit Windows target because the built-in "
        "DependencyControl NativeAOT plugin has no win-x86 runtime")
endif()

set(AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR "" CACHE PATH
    "Directory containing nethost/hostfxr headers and the nethost runtime library")

if(NOT AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR)
    file(GLOB _aegisub_host_pack_versions LIST_DIRECTORIES true
        "${_aegisub_dotnet_root}/packs/Microsoft.NETCore.App.Host.${_aegisub_dotnet_rid}/*")
    set(_aegisub_best_host_pack_version 0.0.0)
    foreach(_aegisub_host_pack_dir IN LISTS _aegisub_host_pack_versions)
        get_filename_component(_aegisub_host_pack_version "${_aegisub_host_pack_dir}" NAME)
        set(_aegisub_host_pack_native
            "${_aegisub_host_pack_dir}/runtimes/${_aegisub_dotnet_rid}/native")
        if(_aegisub_host_pack_version VERSION_GREATER_EQUAL 10.0
            AND _aegisub_host_pack_version VERSION_GREATER _aegisub_best_host_pack_version
            AND EXISTS "${_aegisub_host_pack_native}/nethost.h"
            AND EXISTS "${_aegisub_host_pack_native}/hostfxr.h"
            AND EXISTS "${_aegisub_host_pack_native}/coreclr_delegates.h"
            AND EXISTS "${_aegisub_host_pack_native}/${_aegisub_nethost_filename}")
            set(_aegisub_best_host_pack_version "${_aegisub_host_pack_version}")
            set(AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR "${_aegisub_host_pack_native}")
        endif()
    endforeach()
endif()

if(NOT AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR)
    message(FATAL_ERROR
        "Could not find the .NET 10+ ${_aegisub_dotnet_rid} native hosting pack. "
        "Set AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR explicitly.")
endif()

foreach(_aegisub_host_header nethost.h hostfxr.h coreclr_delegates.h)
    if(NOT EXISTS "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_host_header}")
        message(FATAL_ERROR "Missing ${_aegisub_host_header} in ${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
    endif()
endforeach()
if(NOT EXISTS "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_nethost_filename}")
    message(FATAL_ERROR
        "Missing ${_aegisub_nethost_filename} in ${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
endif()

set(_aegisub_coreclr_adapter_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/Aegisub.CoreClr.Adapter.csproj")
set(_aegisub_coreclr_adapter_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.CoreClr.Adapter/$<CONFIG>")
set(_aegisub_managed_contracts_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/Aegisub.Managed.Contracts.csproj")
set(_aegisub_managed_sample_project
    "${PROJECT_SOURCE_DIR}/managed/samples/Aegisub.Managed.SampleExtension/Aegisub.Managed.SampleExtension.csproj")
set(_aegisub_managed_sample_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.Managed.SampleExtension/$<CONFIG>")
set(_aegisub_managed_devhost_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.DevHost/Aegisub.Managed.DevHost.csproj")
set(_aegisub_managed_devhost_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.Managed.DevHost/$<CONFIG>")
set(_aegisub_dependency_control_core_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/Aegisub.DependencyControl.Core.csproj")
set(_aegisub_dependency_control_plugin_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/Aegisub.DependencyControl.Plugin.csproj")
set(_aegisub_dependency_control_plugin_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.Plugin/$<CONFIG>")
set(_aegisub_dependency_control_nativeaot_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.NativeAot/Aegisub.DependencyControl.NativeAot.csproj")
set(_aegisub_dependency_control_nativeaot_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.NativeAot/$<CONFIG>")
if(WIN32)
    set(_aegisub_dependency_control_nativeaot_filename
        "Aegisub.DependencyControl.NativeAot.dll")
elseif(APPLE)
    set(_aegisub_dependency_control_nativeaot_filename
        "Aegisub.DependencyControl.NativeAot.dylib")
else()
    set(_aegisub_dependency_control_nativeaot_filename
        "Aegisub.DependencyControl.NativeAot.so")
endif()
set(_aegisub_dependency_control_smoke_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Smoke/Aegisub.DependencyControl.Smoke.csproj")
set(_aegisub_dependency_control_smoke_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.Smoke/$<CONFIG>")
set(_aegisub_dependency_control_install_smoke_project
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.InstallSmokeHost/Aegisub.DependencyControl.InstallSmokeHost.csproj")
set(_aegisub_dependency_control_install_smoke_output
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.InstallSmokeHost/$<CONFIG>")

set(_aegisub_managed_stamp_dir
    "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/${CMAKE_CFG_INTDIR}")
set(_aegisub_coreclr_adapter_stamp
    "${_aegisub_managed_stamp_dir}/adapter.stamp")
set(_aegisub_coreclr_adapter_dll
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.CoreClr.Adapter/${CMAKE_CFG_INTDIR}/Aegisub.CoreClr.Adapter.dll")
add_custom_command(OUTPUT
        "${_aegisub_coreclr_adapter_stamp}"
        "${_aegisub_coreclr_adapter_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_coreclr_adapter_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" build "${_aegisub_coreclr_adapter_project}"
        --nologo
        --framework net10.0
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_coreclr_adapter_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_coreclr_adapter_project}"
        "${_aegisub_managed_contracts_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/AutomationContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/DeclarativeUiContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/PluginContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/BridgeJsonContext.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/BridgeEntryPoints.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/PluginManager.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/PluginLoadContext.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/runtimeconfig.template.json"
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/adapter.stamp"
    COMMENT "Building the Aegisub Plugin Bridge managed Adapter"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-coreclr-adapter DEPENDS
    "${_aegisub_coreclr_adapter_stamp}"
    "${_aegisub_coreclr_adapter_dll}")

set(_aegisub_managed_contracts_stamp
    "${_aegisub_managed_stamp_dir}/contracts-package.stamp")
set(_aegisub_managed_contracts_package
    "${CMAKE_CURRENT_BINARY_DIR}/managed/packages/${CMAKE_CFG_INTDIR}/Aegisub.Managed.Contracts.0.1.0.nupkg")
add_custom_command(OUTPUT
        "${_aegisub_managed_contracts_stamp}"
        "${_aegisub_managed_contracts_package}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/packages/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" pack "${_aegisub_managed_contracts_project}"
        --nologo
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/managed/packages/$<CONFIG>"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_managed_contracts_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/AutomationContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/DeclarativeUiContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/PluginContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/PACKAGE_README.md"
        aegisub-coreclr-adapter
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/contracts-package.stamp"
    COMMENT "Packing the experimental Aegisub managed Contracts NuGet package"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-managed-contracts-package DEPENDS
    "${_aegisub_managed_contracts_stamp}"
    "${_aegisub_managed_contracts_package}")
add_dependencies(aegisub-managed-contracts-package aegisub-coreclr-adapter)

set(_aegisub_managed_sample_stamp
    "${_aegisub_managed_stamp_dir}/sample-extension.stamp")
set(_aegisub_managed_sample_dll
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.Managed.SampleExtension/${CMAKE_CFG_INTDIR}/Aegisub.Managed.SampleExtension.dll")
add_custom_command(OUTPUT
        "${_aegisub_managed_sample_stamp}"
        "${_aegisub_managed_sample_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_managed_sample_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" build "${_aegisub_managed_sample_project}"
        --nologo
        --framework net10.0
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_managed_sample_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_managed_sample_project}"
        "${PROJECT_SOURCE_DIR}/managed/samples/Aegisub.Managed.SampleExtension/SampleExtensionModule.cs"
        "${_aegisub_managed_contracts_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/AutomationContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/DeclarativeUiContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/PluginContracts.cs"
        aegisub-coreclr-adapter
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/sample-extension.stamp"
    COMMENT "Building the external Plugin Bridge sample extension"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-managed-sample-extension DEPENDS
    "${_aegisub_managed_sample_stamp}"
    "${_aegisub_managed_sample_dll}")
add_dependencies(aegisub-managed-sample-extension aegisub-coreclr-adapter)

set(_aegisub_managed_devhost_stamp
    "${_aegisub_managed_stamp_dir}/devhost.stamp")
set(_aegisub_managed_devhost_dll
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.Managed.DevHost/${CMAKE_CFG_INTDIR}/Aegisub.Managed.DevHost.dll")
add_custom_command(OUTPUT
        "${_aegisub_managed_devhost_stamp}"
        "${_aegisub_managed_devhost_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_managed_devhost_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" build "${_aegisub_managed_devhost_project}"
        --nologo
        --framework net10.0
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_managed_devhost_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_managed_devhost_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.DevHost/DevHostExtensionLoadContext.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.DevHost/Program.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.DevHost/fixtures/trim-selected-context.json"
        "${_aegisub_managed_sample_project}"
        "${PROJECT_SOURCE_DIR}/managed/samples/Aegisub.Managed.SampleExtension/SampleExtensionModule.cs"
        "${_aegisub_managed_contracts_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/AutomationContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/DeclarativeUiContracts.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/PluginContracts.cs"
        "${_aegisub_managed_sample_dll}"
        aegisub-managed-sample-extension
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/devhost.stamp"
    COMMENT "Building the standalone C# Macro development host"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-managed-devhost DEPENDS
    "${_aegisub_managed_devhost_stamp}"
    "${_aegisub_managed_devhost_dll}")
add_dependencies(aegisub-managed-devhost aegisub-managed-sample-extension)

set(_aegisub_dependency_control_plugin_stamp
    "${_aegisub_managed_stamp_dir}/dependency-control-plugin.stamp")
set(_aegisub_dependency_control_plugin_dll
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.Plugin/${CMAKE_CFG_INTDIR}/Aegisub.DependencyControl.Plugin.dll")
add_custom_command(OUTPUT
        "${_aegisub_dependency_control_plugin_stamp}"
        "${_aegisub_dependency_control_plugin_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_plugin_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" build
        "${_aegisub_dependency_control_plugin_project}"
        --nologo
        --framework net10.0
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_dependency_control_plugin_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_dependency_control_plugin_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlPlugin.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlHttpTransport.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlFeedCatalog.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlFeedStore.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlInstaller.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlUninstaller.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlConfigurationStore.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlInstalledStateStore.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlInstallJournal.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlLogStore.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlNetworkSettings.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlToolView.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/Aegisub.DependencyControl.Plugin.aegisub-plugin.json"
        "${_aegisub_dependency_control_core_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/DependencyControlFeed.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/DependencyControlFeedParser.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/DependencyControlTargetPath.cs"
        "${_aegisub_managed_contracts_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/PluginContracts.cs"
        aegisub-coreclr-adapter
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/dependency-control-plugin.stamp"
    COMMENT "Building the built-in DependencyControl managed plugin"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-dependency-control-plugin DEPENDS
    "${_aegisub_dependency_control_plugin_stamp}"
    "${_aegisub_dependency_control_plugin_dll}")
add_dependencies(aegisub-dependency-control-plugin aegisub-coreclr-adapter)

set(_aegisub_dependency_control_nativeaot_stamp
    "${_aegisub_managed_stamp_dir}/dependency-control-nativeaot.stamp")
set(_aegisub_dependency_control_nativeaot_library
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.NativeAot/${CMAKE_CFG_INTDIR}/${_aegisub_dependency_control_nativeaot_filename}")
add_custom_command(OUTPUT
        "${_aegisub_dependency_control_nativeaot_stamp}"
        "${_aegisub_dependency_control_nativeaot_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_nativeaot_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" publish
        "${_aegisub_dependency_control_nativeaot_project}"
        --nologo
        --runtime "${_aegisub_dotnet_rid}"
        --self-contained true
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_dependency_control_nativeaot_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_dependency_control_nativeaot_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.NativeAot/NativeAotEntryPoint.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/Aegisub.CoreClr.Adapter.csproj"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/BridgeEntryPoints.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/BridgeJsonContext.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.CoreClr.Adapter/PluginManager.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/Aegisub.DependencyControl.Plugin.nativeaot.aegisub-plugin.json"
        "${_aegisub_dependency_control_plugin_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/dependency-control-nativeaot.stamp"
    COMMENT "Publishing the built-in DependencyControl NativeAOT plugin"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-dependency-control-nativeaot DEPENDS
    "${_aegisub_dependency_control_nativeaot_stamp}"
    "${_aegisub_dependency_control_nativeaot_library}")
add_dependencies(aegisub-dependency-control-nativeaot aegisub-dependency-control-plugin)

set(_aegisub_dependency_control_smoke_stamp
    "${_aegisub_managed_stamp_dir}/dependency-control-feed-smoke.stamp")
set(_aegisub_dependency_control_smoke_dll
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.Smoke/${CMAKE_CFG_INTDIR}/Aegisub.DependencyControl.Smoke.dll")
add_custom_command(OUTPUT
        "${_aegisub_dependency_control_smoke_stamp}"
        "${_aegisub_dependency_control_smoke_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_smoke_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" build
        "${_aegisub_dependency_control_smoke_project}"
        --nologo
        --framework net10.0
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_dependency_control_smoke_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_dependency_control_core_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/DependencyControlFeed.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/DependencyControlFeedParser.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Core/DependencyControlTargetPath.cs"
        "${_aegisub_dependency_control_smoke_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Smoke/Program.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Smoke/fixtures/feed-0.2.json"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Smoke/fixtures/feed-0.3.json"
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/dependency-control-feed-smoke.stamp"
    COMMENT "Building the DependencyControl feed compatibility smoke"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-dependency-control-feed-smoke DEPENDS
    "${_aegisub_dependency_control_smoke_stamp}"
    "${_aegisub_dependency_control_smoke_dll}")

add_custom_target(run-aegisub-dependency-control-feed-smoke
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}"
        "${_aegisub_dependency_control_smoke_output}/Aegisub.DependencyControl.Smoke.dll"
    DEPENDS aegisub-dependency-control-feed-smoke
    COMMENT "Running the offline DependencyControl feed compatibility smoke"
    USES_TERMINAL
    VERBATIM
)

set(_aegisub_dependency_control_install_smoke_stamp
    "${_aegisub_managed_stamp_dir}/dependency-control-install-smoke.stamp")
set(_aegisub_dependency_control_install_smoke_dll
    "${CMAKE_CURRENT_BINARY_DIR}/managed/Aegisub.DependencyControl.InstallSmokeHost/${CMAKE_CFG_INTDIR}/Aegisub.DependencyControl.InstallSmokeHost.dll")
add_custom_command(OUTPUT
        "${_aegisub_dependency_control_install_smoke_stamp}"
        "${_aegisub_dependency_control_install_smoke_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_install_smoke_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}" build
        "${_aegisub_dependency_control_install_smoke_project}"
        --nologo
        --framework net10.0
        --configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        --output "${_aegisub_dependency_control_install_smoke_output}"
        -p:ContinuousIntegrationBuild=true
    DEPENDS
        "${_aegisub_dependency_control_install_smoke_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.InstallSmokeHost/Program.cs"
        "${_aegisub_dependency_control_plugin_project}"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/DependencyControlToolView.cs"
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.Managed.Contracts/DeclarativeUiContracts.cs"
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_CURRENT_BINARY_DIR}/managed/stamps/$<CONFIG>/dependency-control-install-smoke.stamp"
    COMMENT "Building the DependencyControl loopback install smoke host"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(aegisub-dependency-control-install-smoke DEPENDS
    "${_aegisub_dependency_control_install_smoke_stamp}"
    "${_aegisub_dependency_control_install_smoke_dll}")

add_custom_target(run-aegisub-managed-devhost-smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DDOTNET_EXECUTABLE=${AEGISUB_DOTNET_EXECUTABLE}"
        "-DDEVHOST_DLL=${_aegisub_managed_devhost_output}/Aegisub.Managed.DevHost.dll"
        "-DSMOKE_DIR=${CMAKE_CURRENT_BINARY_DIR}/plugin-bridge-devhost-smoke/$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/run-devhost-smoke.cmake"
    DEPENDS aegisub-managed-devhost
    COMMENT "Running the standalone C# Macro DevHost smoke"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run-aegisub-csharp-template-smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DDOTNET_EXECUTABLE=${AEGISUB_DOTNET_EXECUTABLE}"
        "-DTEMPLATE_DIR=${PROJECT_SOURCE_DIR}/managed/templates/aegisub-csharp-extension"
        "-DCONTRACTS_PROJECT=${_aegisub_managed_contracts_project}"
        "-DCONTRACTS_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/managed/packages/$<CONFIG>/Aegisub.Managed.Contracts.0.1.0.nupkg"
        "-DDEVHOST_PROJECT=${_aegisub_managed_devhost_project}"
        "-DDEVHOST_DLL=${_aegisub_managed_devhost_output}/Aegisub.Managed.DevHost.dll"
        "-DAEGISUB_EXE=$<TARGET_FILE:Aegisub>"
        "-DSMOKE_DIR=${CMAKE_CURRENT_BINARY_DIR}/csharp-extension-template-smoke/$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/run-template-smoke.cmake"
    DEPENDS Aegisub aegisub-managed-devhost aegisub-managed-contracts-package
    COMMENT "Generating and running an Aegisub C# extension from the SDK template"
    USES_TERMINAL
    VERBATIM
)

add_library(aegisub-nativeaot-missing-export-fixture SHARED
    "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/nativeaot_fixture.cpp")
target_compile_features(aegisub-nativeaot-missing-export-fixture PRIVATE cxx_std_20)
target_compile_definitions(aegisub-nativeaot-missing-export-fixture PRIVATE
    AEGISUB_NATIVEAOT_FIXTURE_MISSING_EXPORT=1)
target_include_directories(aegisub-nativeaot-missing-export-fixture PRIVATE
    "${PROJECT_SOURCE_DIR}/src")

add_library(aegisub-nativeaot-incompatible-fixture SHARED
    "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/nativeaot_fixture.cpp")
target_compile_features(aegisub-nativeaot-incompatible-fixture PRIVATE cxx_std_20)
target_include_directories(aegisub-nativeaot-incompatible-fixture PRIVATE
    "${PROJECT_SOURCE_DIR}/src")

add_executable(aegisub-plugin-bridge-smoke
    "${PROJECT_SOURCE_DIR}/src/coreclr/adapter_bridge.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/bridge_error.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/native_library.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/managed_plugin_activation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/main.cpp"
)
target_compile_features(aegisub-plugin-bridge-smoke PRIVATE cxx_std_20)
target_include_directories(aegisub-plugin-bridge-smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libaegisub/include"
    "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}"
)
target_compile_definitions(aegisub-plugin-bridge-smoke PRIVATE
    "AEGISUB_PLUGIN_BRIDGE_MANAGED_DIR=\"${_aegisub_coreclr_adapter_output}\""
    "AEGISUB_PLUGIN_BRIDGE_SAMPLE_DIR=\"${_aegisub_managed_sample_output}\""
    "AEGISUB_DEPENDENCY_CONTROL_NATIVEAOT_FILENAME=\"${_aegisub_dependency_control_nativeaot_filename}\""
    "AEGISUB_NATIVEAOT_MISSING_EXPORT_FILENAME=\"$<TARGET_FILE_NAME:aegisub-nativeaot-missing-export-fixture>\""
    "AEGISUB_NATIVEAOT_INCOMPATIBLE_FILENAME=\"$<TARGET_FILE_NAME:aegisub-nativeaot-incompatible-fixture>\""
)
target_link_libraries(aegisub-plugin-bridge-smoke PRIVATE ${CMAKE_DL_LIBS} libaegisub)
add_dependencies(aegisub-plugin-bridge-smoke
    aegisub-managed-sample-extension
    aegisub-dependency-control-nativeaot
    aegisub-nativeaot-missing-export-fixture
    aegisub-nativeaot-incompatible-fixture)

add_custom_command(TARGET aegisub-plugin-bridge-smoke POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_nethost_filename}"
        "$<TARGET_FILE_DIR:aegisub-plugin-bridge-smoke>/${_aegisub_nethost_filename}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_aegisub_dependency_control_nativeaot_library}"
        "$<TARGET_FILE_DIR:aegisub-plugin-bridge-smoke>/${_aegisub_dependency_control_nativeaot_filename}"
    COMMENT "Deploying the dynamically loaded nethost library"
    VERBATIM
)

add_custom_target(run-aegisub-plugin-bridge-smoke
    COMMAND "$<TARGET_FILE:aegisub-plugin-bridge-smoke>"
    DEPENDS aegisub-plugin-bridge-smoke
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run-aegisub-plugin-sdk-smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DDOTNET_EXECUTABLE=${AEGISUB_DOTNET_EXECUTABLE}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/plugin-sdk-smoke/$<CONFIG>"
        "-DRID=${_aegisub_dotnet_rid}"
        "-DNATIVE_LIBRARY_NAME=Aegisub.Plugin.Sdk.Smoke.NativeAot${CMAKE_SHARED_LIBRARY_SUFFIX}"
        -P "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/run-plugin-sdk-smoke.cmake"
    COMMENT "Packing and validating the Aegisub Plugin SDK and NativeAOT generator"
    USES_TERMINAL
    VERBATIM
)

target_sources(Aegisub PRIVATE
    "${PROJECT_SOURCE_DIR}/src/coreclr/adapter_bridge.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/bridge_error.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/declarative_ui_host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/declarative_ui_model.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dependency_control_host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dependency_control_lua.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dependency_control_transaction.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dotnet_automation_engine.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dotnet_query_state.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dotnet_subtitle_bridge.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/native_library.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/managed_plugin_activation.cpp"
)
target_include_directories(Aegisub PRIVATE "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
target_compile_definitions(Aegisub PRIVATE WITH_PLUGIN_BRIDGE=1)
target_link_libraries(Aegisub PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(Aegisub
    aegisub-managed-sample-extension
    aegisub-dependency-control-plugin
    aegisub-dependency-control-nativeaot)

set(_aegisub_plugin_bridge_component_dir "$<TARGET_FILE_DIR:Aegisub>/plugin_bridge")
set(_aegisub_dependency_control_component_dir
    "${_aegisub_plugin_bridge_component_dir}/dependency-control")
set(_aegisub_plugin_bridge_autoload_dir "$<TARGET_FILE_DIR:Aegisub>/automation/autoload")
set(_aegisub_plugin_bridge_sample_dir "${_aegisub_plugin_bridge_autoload_dir}/CSharpBridgeDemo")
# Early template-smoke revisions deployed their generated extension directly
# into the application build tree. The manifest used the retired v1 schema and
# otherwise survives incremental builds, causing a startup autoload error.
set(_aegisub_legacy_template_smoke_id "aegisub.csharp-template.smoke")
add_custom_command(TARGET Aegisub POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_plugin_bridge_component_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_dependency_control_component_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_component_dir}/runtimes/${_aegisub_dotnet_rid}/native"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_plugin_bridge_autoload_dir}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${_aegisub_plugin_bridge_autoload_dir}/${_aegisub_legacy_template_smoke_id}.aegisub-plugin.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "${_aegisub_plugin_bridge_autoload_dir}/${_aegisub_legacy_template_smoke_id}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aegisub_plugin_bridge_sample_dir}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${_aegisub_plugin_bridge_sample_dir}/Aegisub.Managed.Contracts.dll"
        "${_aegisub_plugin_bridge_sample_dir}/Aegisub.Managed.Contracts.pdb"
        "${_aegisub_plugin_bridge_sample_dir}/Aegisub.Managed.Contracts.deps.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_nethost_filename}"
        "${_aegisub_plugin_bridge_component_dir}/${_aegisub_nethost_filename}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.dll"
        "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.pdb"
        "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.deps.json"
        "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.runtimeconfig.json"
        "${_aegisub_coreclr_adapter_output}/Aegisub.Managed.Contracts.dll"
        "${_aegisub_coreclr_adapter_output}/Aegisub.Managed.Contracts.pdb"
        "${_aegisub_plugin_bridge_component_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_aegisub_managed_sample_output}/Aegisub.Managed.SampleExtension.dll"
        "${_aegisub_managed_sample_output}/Aegisub.Managed.SampleExtension.pdb"
        "${_aegisub_managed_sample_output}/Aegisub.Managed.SampleExtension.deps.json"
        "${_aegisub_plugin_bridge_sample_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Plugin.dll"
        "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Plugin.pdb"
        "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Plugin.deps.json"
        "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Core.dll"
        "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Core.pdb"
        "${_aegisub_dependency_control_component_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_aegisub_dependency_control_nativeaot_library}"
        "${_aegisub_dependency_control_component_dir}/runtimes/${_aegisub_dotnet_rid}/native/${_aegisub_dependency_control_nativeaot_filename}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/Aegisub.DependencyControl.Plugin.nativeaot.aegisub-plugin.json"
        "${_aegisub_dependency_control_component_dir}/Aegisub.DependencyControl.Plugin.aegisub-plugin.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/managed/samples/CSharpBridgeDemo.aegisub-plugin.json"
        "${_aegisub_plugin_bridge_autoload_dir}/CSharpBridgeDemo.aegisub-plugin.json"
    COMMENT "Deploying the optional Plugin Bridge components and demo manifest"
    VERBATIM
)

install(FILES
    "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_nethost_filename}"
    "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.dll"
    "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.deps.json"
    "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.runtimeconfig.json"
    "${_aegisub_coreclr_adapter_output}/Aegisub.Managed.Contracts.dll"
    DESTINATION bin/plugin_bridge
    COMPONENT PluginBridge
)
install(FILES
    "${_aegisub_coreclr_adapter_output}/Aegisub.CoreClr.Adapter.pdb"
    "${_aegisub_coreclr_adapter_output}/Aegisub.Managed.Contracts.pdb"
    DESTINATION bin/plugin_bridge
    COMPONENT PluginBridge
    OPTIONAL
)
install(FILES
    "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Plugin.dll"
    "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Plugin.deps.json"
    "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Core.dll"
    DESTINATION bin/plugin_bridge/dependency-control
    COMPONENT PluginBridge
)
install(FILES
    "${PROJECT_SOURCE_DIR}/managed/Aegisub.DependencyControl.Plugin/Aegisub.DependencyControl.Plugin.nativeaot.aegisub-plugin.json"
    DESTINATION bin/plugin_bridge/dependency-control
    RENAME Aegisub.DependencyControl.Plugin.aegisub-plugin.json
    COMPONENT PluginBridge
)
install(FILES
    "${_aegisub_dependency_control_nativeaot_library}"
    DESTINATION bin/plugin_bridge/dependency-control/runtimes/${_aegisub_dotnet_rid}/native
    COMPONENT PluginBridge
)
install(FILES
    "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Plugin.pdb"
    "${_aegisub_dependency_control_plugin_output}/Aegisub.DependencyControl.Core.pdb"
    DESTINATION bin/plugin_bridge/dependency-control
    COMPONENT PluginBridge
    OPTIONAL
)

add_custom_target(run-aegisub-plugin-automation-smoke
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/plugin-bridge-automation-smoke/$<CONFIG>"
    COMMAND "$<TARGET_FILE:Aegisub>"
        --cli session automation
        --script "$<TARGET_FILE_DIR:Aegisub>/automation/autoload/CSharpBridgeDemo.aegisub-plugin.json"
        --macro "aegisub.plugin-bridge.demo.runtime-info"
        --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/plugin-bridge-automation-smoke/$<CONFIG>"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Aegisub>"
    DEPENDS Aegisub
    COMMENT "Running the Plugin Bridge Automation Macro end-to-end smoke"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run-aegisub-dependency-control-lua-smoke
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/dependency-control-lua-smoke/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E env
        "AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/dependency-control-lua-smoke/$<CONFIG>/state"
        "$<TARGET_FILE:Aegisub>"
        --cli session automation
        --script "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/fixtures/dependency-control-lua-compat.lua"
        --macro "DependencyControl Lua compatibility smoke"
        --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/dependency-control-lua-smoke/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E env
        "AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/dependency-control-lua-smoke/$<CONFIG>/state"
        "$<TARGET_FILE:Aegisub>"
        --cli session automation
        --script "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/fixtures/dependency-control-transport-limits.lua"
        --macro "DependencyControl transport limits smoke"
        --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/dependency-control-transport-limits-smoke/$<CONFIG>"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Aegisub>"
    DEPENDS Aegisub
    COMMENT "Running the DependencyControl Lua facade end-to-end smoke"
    USES_TERMINAL
    VERBATIM
)

set(_aegisub_dependency_control_nativeaot_isolation_dir
    "${CMAKE_CURRENT_BINARY_DIR}/dependency-control-nativeaot-isolation/$<CONFIG>")
add_custom_target(run-aegisub-dependency-control-nativeaot-smoke
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "${_aegisub_dependency_control_nativeaot_isolation_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_nativeaot_isolation_dir}/state"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_aegisub_dependency_control_nativeaot_isolation_dir}/trace"
    COMMAND "${CMAKE_COMMAND}" -E env
        "AEGISUB_DOTNET_ROOT=${_aegisub_dependency_control_nativeaot_isolation_dir}/missing-dotnet-root"
        "AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT=${_aegisub_dependency_control_nativeaot_isolation_dir}/state"
        "$<TARGET_FILE:Aegisub>"
        --cli session automation
        --script "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/fixtures/dependency-control-lua-compat.lua"
        --macro "DependencyControl Lua compatibility smoke"
        --trace-dir "${_aegisub_dependency_control_nativeaot_isolation_dir}/trace"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Aegisub>"
    DEPENDS Aegisub
    COMMENT "Running the DependencyControl NativeAOT-only smoke without CoreCLR"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run-aegisub-dependency-control-install-smoke
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/dependency-control-install-smoke/$<CONFIG>"
    COMMAND "${AEGISUB_DOTNET_EXECUTABLE}"
        "${_aegisub_dependency_control_install_smoke_output}/Aegisub.DependencyControl.InstallSmokeHost.dll"
        "$<TARGET_FILE:Aegisub>"
        "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/fixtures/dependency-control-install.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/dependency-control-install-smoke/$<CONFIG>"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Aegisub>"
    DEPENDS Aegisub aegisub-dependency-control-install-smoke
    COMMENT "Running the DependencyControl loopback install transaction smoke"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run-aegisub-plugin-query-state-smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DAEGISUB_EXE=$<TARGET_FILE:Aegisub>"
        "-DSCRIPT_FILE=$<TARGET_FILE_DIR:Aegisub>/automation/autoload/CSharpBridgeDemo.aegisub-plugin.json"
        "-DSMOKE_DIR=${CMAKE_CURRENT_BINARY_DIR}/plugin-bridge-query-state-smoke/$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/run-query-state-smoke.cmake"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Aegisub>"
    DEPENDS Aegisub
    COMMENT "Running the native C# Macro QueryState/lazy CLR smoke"
    USES_TERMINAL
    VERBATIM
)

set(_aegisub_csharp_subtitle_smoke_dir
    "${CMAKE_CURRENT_BINARY_DIR}/plugin-bridge-subtitle-smoke/$<CONFIG>")
add_custom_target(run-aegisub-plugin-subtitle-smoke
    COMMAND "${CMAKE_COMMAND}"
        "-DAEGISUB_EXE=$<TARGET_FILE:Aegisub>"
        "-DSCRIPT_FILE=$<TARGET_FILE_DIR:Aegisub>/automation/autoload/CSharpBridgeDemo.aegisub-plugin.json"
        "-DINPUT_FILE=${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/trim-selected-input.ass"
        "-DNOOP_INPUT_FILE=${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/trim-selected-noop-input.ass"
        "-DSMOKE_DIR=${_aegisub_csharp_subtitle_smoke_dir}"
        -P "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/run-subtitle-smoke.cmake"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Aegisub>"
    DEPENDS Aegisub
    COMMENT "Running the C# subtitle snapshot and batch mutation smoke"
    USES_TERMINAL
    VERBATIM
)

message(STATUS
    "Plugin Bridge: dotnet=${AEGISUB_DOTNET_EXECUTABLE}, "
    "RID=${_aegisub_dotnet_rid}, host pack=${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
