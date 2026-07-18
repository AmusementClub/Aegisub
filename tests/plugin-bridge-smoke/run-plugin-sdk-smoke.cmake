cmake_minimum_required(VERSION 3.24)

foreach(required DOTNET_EXECUTABLE SOURCE_DIR OUTPUT_DIR RID NATIVE_LIBRARY_NAME)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "run-plugin-sdk-smoke requires ${required}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(package_dir "${OUTPUT_DIR}/packages")
set(native_output "${OUTPUT_DIR}/native")
set(host_output "${OUTPUT_DIR}/host")
set(nuget_packages "${OUTPUT_DIR}/nuget-packages")
file(REMOVE_RECURSE "${nuget_packages}")
file(MAKE_DIRECTORY "${package_dir}" "${native_output}" "${host_output}" "${nuget_packages}")

set(build_tasks_project
    "${SOURCE_DIR}/managed/Aegisub.Plugin.BuildTasks/Aegisub.Plugin.BuildTasks.csproj")
set(generator_project
    "${SOURCE_DIR}/managed/Aegisub.Plugin.Generators/Aegisub.Plugin.Generators.csproj")
set(sdk_project
    "${SOURCE_DIR}/managed/Aegisub.Plugin.Sdk/Aegisub.Plugin.Sdk.csproj")
set(contracts_project
    "${SOURCE_DIR}/managed/Aegisub.Managed.Contracts/Aegisub.Managed.Contracts.csproj")
set(smoke_project
    "${SOURCE_DIR}/managed/samples/Aegisub.Plugin.Sdk.Smoke/Aegisub.Plugin.Sdk.Smoke.csproj")
set(native_project
    "${SOURCE_DIR}/managed/samples/Aegisub.Plugin.Sdk.Smoke.NativeAot/Aegisub.Plugin.Sdk.Smoke.NativeAot.csproj")
set(host_project
    "${SOURCE_DIR}/managed/Aegisub.Plugin.Sdk.SmokeHost/Aegisub.Plugin.Sdk.SmokeHost.csproj")
set(package_consumer_project
    "${SOURCE_DIR}/tests/plugin-bridge-smoke/plugin-sdk-package-consumer/Aegisub.Plugin.Sdk.PackageSmoke.csproj")
set(invalid_consumer_project
    "${SOURCE_DIR}/tests/plugin-bridge-smoke/plugin-sdk-invalid-consumer/Aegisub.Plugin.Sdk.InvalidSmoke.csproj")
set(build_tasks_assembly
    "${SOURCE_DIR}/managed/Aegisub.Plugin.BuildTasks/bin/Release/net10.0/Aegisub.Plugin.BuildTasks.dll")

function(run_dotnet description)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "NUGET_PACKAGES=${nuget_packages}"
            "${DOTNET_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        ENCODING UTF-8)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${description} failed with ${result}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    if(NOT "${output}" STREQUAL "")
        string(STRIP "${output}" output)
        message(STATUS "${output}")
    endif()
    if(NOT "${error}" STREQUAL "")
        string(STRIP "${error}" error)
        message(STATUS "${error}")
    endif()
endfunction()

function(run_dotnet_expect_failure description expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "NUGET_PACKAGES=${nuget_packages}"
            "${DOTNET_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        ENCODING UTF-8)
    if(result EQUAL 0)
        message(FATAL_ERROR "${description} unexpectedly succeeded")
    endif()
    set(combined "${output}\n${error}")
    string(FIND "${combined}" "${expected}" expected_index)
    if(expected_index EQUAL -1)
        message(FATAL_ERROR
            "${description} did not report ${expected}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

run_dotnet("Build server shutdown" build-server shutdown)
run_dotnet("BuildTasks package"
    pack "${build_tasks_project}" --nologo --configuration Release
    --output "${package_dir}" -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("Contracts package"
    pack "${contracts_project}" --nologo --configuration Release
    --output "${package_dir}" -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("Generator package"
    pack "${generator_project}" --nologo --configuration Release
    --output "${package_dir}" -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("SDK package"
    pack "${sdk_project}" --nologo --configuration Release
    --output "${package_dir}" -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("Packaged SDK restore"
    restore "${package_consumer_project}" --source "${package_dir}" -nr:false)
run_dotnet("Packaged SDK consumer build"
    build "${package_consumer_project}" --nologo --configuration Release
    --no-restore -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("Invalid SDK consumer restore"
    restore "${invalid_consumer_project}" --source "${package_dir}" -nr:false)
run_dotnet_expect_failure("Invalid SDK diagnostic" "AEGISUBSDK001"
    build "${invalid_consumer_project}" --nologo --configuration Release
    --no-restore -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("CoreCLR SDK smoke build"
    build "${smoke_project}" --nologo --configuration Release
    -p:AegisubBuildTasksAssembly=${build_tasks_assembly}
    -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("CoreCLR plugin package"
    pack "${smoke_project}" --nologo --configuration Release --no-build
    --output "${package_dir}"
    -p:AegisubBuildTasksAssembly=${build_tasks_assembly}
    -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("NativeAOT SDK smoke publish"
    publish "${native_project}" --nologo --configuration Release
    --runtime "${RID}" --self-contained true --output "${native_output}"
    -p:AegisubBuildTasksAssembly=${build_tasks_assembly}
    -p:ContinuousIntegrationBuild=true -nr:false)
run_dotnet("SDK smoke host build"
    build "${host_project}" --nologo --configuration Release
    --output "${host_output}" -p:ContinuousIntegrationBuild=true -nr:false)

set(core_manifest
    "${SOURCE_DIR}/managed/samples/Aegisub.Plugin.Sdk.Smoke/obj/manifest/aegisub.plugin-sdk.smoke.aegisub-plugin.json")
set(core_package "${package_dir}/Aegisub.Plugin.Sdk.Smoke.0.1.0.nupkg")
set(native_library "${native_output}/${NATIVE_LIBRARY_NAME}")
set(package_consumer_manifest
    "${SOURCE_DIR}/tests/plugin-bridge-smoke/plugin-sdk-package-consumer/obj/Release/net10.0/aegisub.plugin-sdk.package-smoke.aegisub-plugin.json")
foreach(required_file "${core_manifest}" "${core_package}" "${native_library}"
        "${host_output}/Aegisub.Plugin.Sdk.SmokeHost.dll"
        "${package_consumer_manifest}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Plugin SDK smoke output is missing: ${required_file}")
    endif()
endforeach()

run_dotnet("Plugin SDK integration smoke"
    "${host_output}/Aegisub.Plugin.Sdk.SmokeHost.dll"
    "${core_manifest}" "${core_package}" "${native_library}")
