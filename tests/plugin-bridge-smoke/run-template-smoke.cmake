foreach(required
    DOTNET_EXECUTABLE
    TEMPLATE_DIR
    CONTRACTS_PROJECT
    CONTRACTS_PACKAGE
    DEVHOST_PROJECT
    DEVHOST_DLL
    AEGISUB_EXE
    SMOKE_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${DOTNET_EXECUTABLE}")
    message(FATAL_ERROR "DOTNET_EXECUTABLE does not name dotnet")
endif()
if(NOT EXISTS "${TEMPLATE_DIR}/.template.config/template.json")
    message(FATAL_ERROR "TEMPLATE_DIR does not contain the C# extension template")
endif()
if(NOT EXISTS "${CONTRACTS_PROJECT}")
    message(FATAL_ERROR "CONTRACTS_PROJECT does not exist")
endif()
if(NOT EXISTS "${CONTRACTS_PACKAGE}")
    message(FATAL_ERROR "CONTRACTS_PACKAGE does not name the packed experimental NuGet package")
endif()
if(NOT EXISTS "${DEVHOST_PROJECT}" OR NOT EXISTS "${DEVHOST_DLL}")
    message(FATAL_ERROR "The managed DevHost project/output does not exist")
endif()
if(NOT EXISTS "${AEGISUB_EXE}")
    message(FATAL_ERROR "AEGISUB_EXE does not name a built Aegisub executable")
endif()

set(extension_name "GeneratedExtension")
set(extension_id "aegisub.csharp-template.smoke")
set(macro_id "${extension_id}.hello")
set(template_hive "${SMOKE_DIR}/template-hive")
set(project_dir "${SMOKE_DIR}/project")
set(project_file "${project_dir}/${extension_name}.csproj")
set(release_dir "${project_dir}/bin/Release/net10.0")
set(debug_dir "${project_dir}/bin/Debug/net10.0")
set(dev_root "${SMOKE_DIR}/development-host")
set(autoload_dir "${dev_root}/automation/autoload")
set(package_dir "${autoload_dir}/${extension_id}")
set(deployed_manifest "${autoload_dir}/${extension_id}.aegisub-plugin.json")

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}" "${dev_root}")

execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" new install "${TEMPLATE_DIR}"
        --debug:custom-hive "${template_hive}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "C# extension template installation failed with ${install_result}\n"
        "stdout:\n${install_stdout}\n"
        "stderr:\n${install_stderr}")
endif()

get_filename_component(aegisub_development_root "${AEGISUB_EXE}" DIRECTORY)
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" new aegisub-csharp-extension
        --debug:custom-hive "${template_hive}"
        --name "${extension_name}"
        --output "${project_dir}"
        --extensionId "${extension_id}"
        --authorName "Aegisub Template Smoke"
        --extensionDescription "Generated extension template smoke."
        --contractsProject "${CONTRACTS_PROJECT}"
        --devHostProject "${DEVHOST_PROJECT}"
        --aegisubDevelopmentRoot "${aegisub_development_root}"
    RESULT_VARIABLE create_result
    OUTPUT_VARIABLE create_stdout
    ERROR_VARIABLE create_stderr
)
if(NOT create_result EQUAL 0 OR NOT EXISTS "${project_file}")
    message(FATAL_ERROR
        "C# extension template creation failed with ${create_result}\n"
        "stdout:\n${create_stdout}\n"
        "stderr:\n${create_stderr}")
endif()

# Add a local managed dependency plus two same-named RID assets. This verifies
# that the class-library template copies package dependencies and preserves
# runtime subdirectories instead of flattening x86/x64 files over each other.
set(dependency_dir "${SMOKE_DIR}/dependency")
set(dependency_feed "${SMOKE_DIR}/dependency-feed")
set(dependency_project "${dependency_dir}/Aegisub.TemplateSmoke.Dependency.csproj")
file(MAKE_DIRECTORY
    "${dependency_dir}/native/win-x64"
    "${dependency_dir}/native/win-x86"
    "${dependency_feed}")
file(WRITE "${dependency_project}" [=[
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <PackageId>Aegisub.TemplateSmoke.Dependency</PackageId>
    <Version>1.0.0</Version>
  </PropertyGroup>
  <ItemGroup>
    <None Include="native/win-x64/template-smoke-native.txt"
          Pack="true" PackagePath="runtimes/win-x64/native/template-smoke-native.txt" />
    <None Include="native/win-x86/template-smoke-native.txt"
          Pack="true" PackagePath="runtimes/win-x86/native/template-smoke-native.txt" />
  </ItemGroup>
</Project>
]=])
file(WRITE "${dependency_dir}/DependencyMarker.cs" [=[
namespace Aegisub.TemplateSmoke.Dependency;
public static class DependencyMarker
{
    public static string Value => "dependency-loaded";
}
]=])
file(WRITE "${dependency_dir}/native/win-x64/template-smoke-native.txt" "win-x64")
file(WRITE "${dependency_dir}/native/win-x86/template-smoke-native.txt" "win-x86")
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" pack "${dependency_project}"
        --configuration Release
        --output "${dependency_feed}"
        --nologo
    RESULT_VARIABLE dependency_pack_result
    OUTPUT_VARIABLE dependency_pack_stdout
    ERROR_VARIABLE dependency_pack_stderr
)
if(NOT dependency_pack_result EQUAL 0)
    message(FATAL_ERROR
        "Template dependency package failed with ${dependency_pack_result}\n"
        "stdout:\n${dependency_pack_stdout}\n"
        "stderr:\n${dependency_pack_stderr}")
endif()

file(READ "${project_file}" project_contents)
string(REPLACE "</Project>" [=[
  <ItemGroup>
    <PackageReference Include="Aegisub.TemplateSmoke.Dependency" Version="1.0.0" />
  </ItemGroup>
</Project>]=] project_contents "${project_contents}")
file(WRITE "${project_file}" "${project_contents}")
file(READ "${project_dir}/ExtensionModule.cs" extension_source)
string(REPLACE
    "internal sealed class HelloMacro : IAegisubMacro\n{"
    [=[internal sealed class HelloMacro : IAegisubMacro
{
    private static readonly string DependencyValue =
        global::Aegisub.TemplateSmoke.Dependency.DependencyMarker.Value;]=]
    extension_source "${extension_source}")
string(REPLACE
    "\"Generated C# extension executed successfully\""
    "\"Generated C# extension executed successfully: \" + DependencyValue"
    extension_source "${extension_source}")
file(WRITE "${project_dir}/ExtensionModule.cs" "${extension_source}")

execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" restore "${project_file}" --use-lock-file
        --source "${dependency_feed}"
    RESULT_VARIABLE restore_result
    OUTPUT_VARIABLE restore_stdout
    ERROR_VARIABLE restore_stderr
)
if(NOT restore_result EQUAL 0 OR NOT EXISTS "${project_dir}/packages.lock.json")
    message(FATAL_ERROR
        "Generated extension restore/lock creation failed with ${restore_result}\n"
        "stdout:\n${restore_stdout}\n"
        "stderr:\n${restore_stderr}")
endif()

execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" restore "${project_file}" --locked-mode
        --source "${dependency_feed}"
    RESULT_VARIABLE locked_restore_result
    OUTPUT_VARIABLE locked_restore_stdout
    ERROR_VARIABLE locked_restore_stderr
)
if(NOT locked_restore_result EQUAL 0)
    message(FATAL_ERROR
        "Generated extension locked restore failed with ${locked_restore_result}\n"
        "stdout:\n${locked_restore_stdout}\n"
        "stderr:\n${locked_restore_stderr}")
endif()

execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" build "${project_file}"
        --configuration Release
        --no-restore
        -p:AegisubDeployDevelopmentBuild=false
    RESULT_VARIABLE release_build_result
    OUTPUT_VARIABLE release_build_stdout
    ERROR_VARIABLE release_build_stderr
)
if(NOT release_build_result EQUAL 0 OR
   NOT EXISTS "${release_dir}/${extension_name}.dll" OR
   NOT EXISTS "${release_dir}/${extension_name}.pdb" OR
   NOT EXISTS "${release_dir}/${extension_name}.deps.json")
    message(FATAL_ERROR
        "Generated extension Release build failed with ${release_build_result}\n"
        "stdout:\n${release_build_stdout}\n"
        "stderr:\n${release_build_stderr}")
endif()
if(EXISTS "${release_dir}/Aegisub.Managed.Contracts.dll")
    message(FATAL_ERROR "Generated extension incorrectly copied its own Contracts runtime assembly")
endif()
if(NOT EXISTS "${release_dir}/Aegisub.TemplateSmoke.Dependency.dll" OR
   NOT EXISTS "${release_dir}/runtimes/win-x64/native/template-smoke-native.txt" OR
   NOT EXISTS "${release_dir}/runtimes/win-x86/native/template-smoke-native.txt")
    message(FATAL_ERROR "Generated extension output did not preserve its managed/RID dependencies")
endif()

set(devhost_result "${SMOKE_DIR}/devhost-result.json")
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" "${DEVHOST_DLL}"
        --assembly "${release_dir}/${extension_name}.dll"
        --type "${extension_name}.ExtensionModule"
        --macro "${macro_id}"
        --context "${project_dir}/fixtures/context.json"
        --result "${devhost_result}"
    RESULT_VARIABLE devhost_result_code
    OUTPUT_VARIABLE devhost_stdout
    ERROR_VARIABLE devhost_stderr
)
if(NOT devhost_result_code EQUAL 0 OR NOT EXISTS "${devhost_result}")
    message(FATAL_ERROR
        "Generated extension DevHost run failed with ${devhost_result_code}\n"
        "stdout:\n${devhost_stdout}\n"
        "stderr:\n${devhost_stderr}")
endif()
file(READ "${devhost_result}" devhost_json)
string(FIND "${devhost_json}" "Generated C# extension executed successfully: dependency-loaded" devhost_status)
if(devhost_status EQUAL -1)
    message(FATAL_ERROR "Generated extension DevHost result did not contain its expected status")
endif()

# A Debug build should deploy only when an Aegisub executable exists in the
# configured development root. A zero-byte marker is enough for this MSBuild
# behavior test; the real executable is used for the integration run below.
file(WRITE "${dev_root}/Aegisub.exe" "")
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" build "${project_file}"
        --configuration Debug
        --no-restore
        "-p:AegisubDevelopmentRoot=${dev_root}"
    RESULT_VARIABLE debug_build_result
    OUTPUT_VARIABLE debug_build_stdout
    ERROR_VARIABLE debug_build_stderr
)
if(NOT debug_build_result EQUAL 0 OR
   NOT EXISTS "${debug_dir}/${extension_name}.dll" OR
   NOT EXISTS "${package_dir}/${extension_name}.dll" OR
   NOT EXISTS "${package_dir}/${extension_name}.pdb" OR
   NOT EXISTS "${deployed_manifest}")
    message(FATAL_ERROR
        "Generated extension Debug deployment failed with ${debug_build_result}\n"
        "stdout:\n${debug_build_stdout}\n"
        "stderr:\n${debug_build_stderr}")
endif()
if(EXISTS "${package_dir}/Aegisub.Managed.Contracts.dll")
    message(FATAL_ERROR "Debug deployment incorrectly packaged its own Contracts assembly")
endif()
if(NOT EXISTS "${package_dir}/Aegisub.TemplateSmoke.Dependency.dll" OR
   NOT EXISTS "${package_dir}/runtimes/win-x64/native/template-smoke-native.txt" OR
   NOT EXISTS "${package_dir}/runtimes/win-x86/native/template-smoke-native.txt" OR
   EXISTS "${package_dir}/template-smoke-native.txt" OR
   NOT EXISTS "${package_dir}/aegisub-automation.json")
    message(FATAL_ERROR "Debug deployment omitted automation assets or flattened managed/RID dependencies")
endif()
file(READ "${package_dir}/runtimes/win-x64/native/template-smoke-native.txt" x64_native_marker)
file(READ "${package_dir}/runtimes/win-x86/native/template-smoke-native.txt" x86_native_marker)
if(NOT x64_native_marker STREQUAL "win-x64" OR NOT x86_native_marker STREQUAL "win-x86")
    message(FATAL_ERROR "Debug deployment mixed architecture-specific runtime assets")
endif()

execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" run
        --project "${project_file}"
        --launch-profile "Managed fixture"
        --no-build
    RESULT_VARIABLE profile_result
    OUTPUT_VARIABLE profile_stdout
    ERROR_VARIABLE profile_stderr
)
if(NOT profile_result EQUAL 0)
    message(FATAL_ERROR
        "Generated Managed fixture launch profile failed with ${profile_result}\n"
        "stdout:\n${profile_stdout}\n"
        "stderr:\n${profile_stderr}")
endif()
string(FIND "${profile_stdout}" "Generated C# extension executed successfully" profile_status)
if(profile_status EQUAL -1)
    message(FATAL_ERROR "Generated Managed fixture launch profile returned an unexpected result")
endif()

set(trace_dir "${SMOKE_DIR}/aegisub-trace")
file(MAKE_DIRECTORY "${trace_dir}")
get_filename_component(aegisub_working_directory "${AEGISUB_EXE}" DIRECTORY)
execute_process(
    COMMAND "${AEGISUB_EXE}"
        --headless run
        --scenario "${package_dir}/aegisub-automation.json"
        --artifacts "${trace_dir}"
    WORKING_DIRECTORY "${aegisub_working_directory}"
    RESULT_VARIABLE aegisub_result
    OUTPUT_VARIABLE aegisub_stdout
    ERROR_VARIABLE aegisub_stderr
)
if(NOT aegisub_result EQUAL 0)
    message(FATAL_ERROR
        "Generated extension Aegisub integration failed with ${aegisub_result}\n"
        "stdout:\n${aegisub_stdout}\n"
        "stderr:\n${aegisub_stderr}")
endif()
foreach(expected
    "script_loaded=true"
    "feature_found=true"
    "result=PASS"
    "Generated C# extension executed successfully: dependency-loaded")
    string(FIND "${aegisub_stdout}" "${expected}" expected_position)
    if(expected_position EQUAL -1)
        message(FATAL_ERROR
            "Generated extension Aegisub output is missing '${expected}'\n"
            "stdout:\n${aegisub_stdout}\n"
            "stderr:\n${aegisub_stderr}")
    endif()
endforeach()

file(READ "${project_dir}/Properties/launchSettings.json" launch_settings)
foreach(profile "Managed fixture" "Aegisub headless integration" "Aegisub GUI integration")
    string(FIND "${launch_settings}" "\"${profile}\"" profile_position)
    if(profile_position EQUAL -1)
        message(FATAL_ERROR "Generated launchSettings.json is missing '${profile}'")
    endif()
endforeach()

get_filename_component(contracts_feed "${CONTRACTS_PACKAGE}" DIRECTORY)
set(missing_contracts_project "${SMOKE_DIR}/missing/Aegisub.Managed.Contracts.csproj")
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" restore "${project_file}"
        --force-evaluate
        --source "${contracts_feed}"
        --source "${dependency_feed}"
        --ignore-failed-sources
        "-p:AegisubContractsProject=${missing_contracts_project}"
    RESULT_VARIABLE package_restore_result
    OUTPUT_VARIABLE package_restore_stdout
    ERROR_VARIABLE package_restore_stderr
)
if(NOT package_restore_result EQUAL 0)
    message(FATAL_ERROR
        "Generated extension Contracts package fallback restore failed with ${package_restore_result}\n"
        "stdout:\n${package_restore_stdout}\n"
        "stderr:\n${package_restore_stderr}")
endif()
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" build "${project_file}"
        --configuration Release
        --no-restore
        -p:AegisubDeployDevelopmentBuild=false
        "-p:AegisubContractsProject=${missing_contracts_project}"
    RESULT_VARIABLE package_build_result
    OUTPUT_VARIABLE package_build_stdout
    ERROR_VARIABLE package_build_stderr
)
if(NOT package_build_result EQUAL 0)
    message(FATAL_ERROR
        "Generated extension Contracts package fallback build failed with ${package_build_result}\n"
        "stdout:\n${package_build_stdout}\n"
        "stderr:\n${package_build_stderr}")
endif()
if(EXISTS "${release_dir}/Aegisub.Managed.Contracts.dll")
    message(FATAL_ERROR "Contracts NuGet fallback incorrectly copied its runtime assembly")
endif()
file(READ "${project_dir}/packages.lock.json" package_lock)
string(FIND "${package_lock}" "Aegisub.Managed.Contracts" package_lock_id)
string(FIND "${package_lock}" "0.1.0" package_lock_version)
if(package_lock_id EQUAL -1 OR package_lock_version EQUAL -1)
    message(FATAL_ERROR "Contracts NuGet fallback did not resolve exact version 0.1.0")
endif()

message(STATUS
    "Generated C# extension template passed locked restore, package fallback, launch profile, Debug deploy, and Aegisub integration")
