if(NOT DEFINED AEGISUB_EXE OR NOT EXISTS "${AEGISUB_EXE}")
    message(FATAL_ERROR "AEGISUB_EXE does not name a built Aegisub executable")
endif()
if(NOT DEFINED SCRIPT_FILE OR NOT EXISTS "${SCRIPT_FILE}")
    message(FATAL_ERROR "SCRIPT_FILE does not name the deployed C# manifest")
endif()
if(NOT DEFINED SMOKE_DIR)
    message(FATAL_ERROR "SMOKE_DIR is required")
endif()

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}")
set(invalid_runtime_root "${SMOKE_DIR}/missing-dotnet-root")

function(assert_contains value expected description)
    string(FIND "${value}" "${expected}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "QueryState smoke did not report ${description}")
    endif()
endfunction()

set(guarded_trace_dir "${SMOKE_DIR}/guarded")
file(MAKE_DIRECTORY "${guarded_trace_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "AEGISUB_DOTNET_ROOT=${invalid_runtime_root}"
        "${AEGISUB_EXE}"
        --cli session automation
        --script "${SCRIPT_FILE}"
        --macro aegisub.plugin-bridge.demo.trim-selected-line-endings
        --trace-dir "${guarded_trace_dir}"
    RESULT_VARIABLE guarded_result
    OUTPUT_VARIABLE guarded_stdout
    ERROR_VARIABLE guarded_stderr
)
if(NOT guarded_result EQUAL 41)
    message(FATAL_ERROR
        "Native QueryState guard returned ${guarded_result}, expected 41\n"
        "stdout:\n${guarded_stdout}\n"
        "stderr:\n${guarded_stderr}")
endif()
if(NOT EXISTS "${guarded_trace_dir}/summary.txt")
    message(FATAL_ERROR "Native QueryState guard did not write a summary")
endif()
file(READ "${guarded_trace_dir}/summary.txt" guarded_summary)
assert_contains("${guarded_summary}" "automation.script_loaded=true" "a loaded manifest")
assert_contains("${guarded_summary}" "automation.feature_found=true" "the guarded Macro")
assert_contains("${guarded_summary}" "automation.validate.ran=true" "native validation")
assert_contains("${guarded_summary}" "automation.validate.passed=false" "a rejected host state")

set(guarded_output "${guarded_stdout}\n${guarded_stderr}")
string(FIND "${guarded_output}" "Adapter initialized" adapter_initialized)
if(NOT adapter_initialized EQUAL -1)
    message(FATAL_ERROR "Native QueryState validation unexpectedly initialized the managed Adapter")
endif()

# Prove that the invalid Runtime override would fail if the Macro crossed the
# managed boundary. This makes the guarded case a CLR-laziness assertion rather
# than merely an expected validation result.
set(unconditional_trace_dir "${SMOKE_DIR}/unconditional")
file(MAKE_DIRECTORY "${unconditional_trace_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "AEGISUB_DOTNET_ROOT=${invalid_runtime_root}"
        "${AEGISUB_EXE}"
        --cli session automation
        --script "${SCRIPT_FILE}"
        --macro aegisub.plugin-bridge.demo.runtime-info
        --trace-dir "${unconditional_trace_dir}"
    RESULT_VARIABLE unconditional_result
    OUTPUT_VARIABLE unconditional_stdout
    ERROR_VARIABLE unconditional_stderr
)
if(NOT unconditional_result EQUAL 43)
    message(FATAL_ERROR
        "Unconditional Runtime probe returned ${unconditional_result}, expected 43\n"
        "stdout:\n${unconditional_stdout}\n"
        "stderr:\n${unconditional_stderr}")
endif()

# An automatically discovered but unusable app-local Runtime is optional. The
# host must validate it before CoreCLR activation and then fall back to the
# system registration. Explicit AEGISUB_DOTNET_ROOT remains strict above.
get_filename_component(aegisub_executable_dir "${AEGISUB_EXE}" DIRECTORY)
set(app_local_runtime_root "${aegisub_executable_dir}/.dotnet")
if(NOT EXISTS "${app_local_runtime_root}")
    file(MAKE_DIRECTORY "${app_local_runtime_root}")
    set(fallback_trace_dir "${SMOKE_DIR}/app-local-fallback")
    file(MAKE_DIRECTORY "${fallback_trace_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "AEGISUB_DOTNET_ROOT="
            "${AEGISUB_EXE}"
            --cli session automation
            --script "${SCRIPT_FILE}"
            --macro aegisub.plugin-bridge.demo.runtime-info
            --trace-dir "${fallback_trace_dir}"
        RESULT_VARIABLE fallback_result
        OUTPUT_VARIABLE fallback_stdout
        ERROR_VARIABLE fallback_stderr
    )
    file(REMOVE_RECURSE "${app_local_runtime_root}")
    if(NOT fallback_result EQUAL 0)
        message(FATAL_ERROR
            "Damaged app-local Runtime did not fall back to the system Runtime (${fallback_result})\n"
            "stdout:\n${fallback_stdout}\n"
            "stderr:\n${fallback_stderr}")
    endif()
    if(NOT EXISTS "${fallback_trace_dir}/summary.txt")
        message(FATAL_ERROR "App-local Runtime fallback did not write a summary")
    endif()
    file(READ "${fallback_trace_dir}/summary.txt" fallback_summary)
    assert_contains("${fallback_summary}" "automation.result=PASS"
        "a successful system Runtime fallback")
else()
    message(STATUS
        "Skipping empty .dotnet fallback probe because ${app_local_runtime_root} already exists")
endif()

get_filename_component(sample_manifest_dir "${SCRIPT_FILE}" DIRECTORY)
set(manifest_validation_dir "${SMOKE_DIR}/manifest-validation")
file(MAKE_DIRECTORY "${manifest_validation_dir}")
file(COPY "${sample_manifest_dir}/CSharpBridgeDemo"
    DESTINATION "${manifest_validation_dir}")
file(READ "${SCRIPT_FILE}" valid_manifest)

function(assert_manifest_rejected manifest expected description)
    execute_process(
        COMMAND "${AEGISUB_EXE}"
            --cli session automation
            --script "${manifest}"
            --macro aegisub.plugin-bridge.demo.runtime-info
            --trace-dir "${manifest}.trace"
        RESULT_VARIABLE invalid_result
        OUTPUT_VARIABLE invalid_stdout
        ERROR_VARIABLE invalid_stderr
    )
    if(NOT invalid_result EQUAL 40)
        message(FATAL_ERROR
            "${description} returned ${invalid_result}, expected 40\n"
            "stdout:\n${invalid_stdout}\n"
            "stderr:\n${invalid_stderr}")
    endif()
    set(invalid_output "${invalid_stdout}\n${invalid_stderr}")
    string(FIND "${invalid_output}" "${expected}" invalid_match)
    if(invalid_match EQUAL -1)
        message(FATAL_ERROR "${description} did not report '${expected}'")
    endif()
endfunction()

string(REPLACE
    "aegisub.plugin-bridge.demo.trim-selected-line-endings"
    "aegisub.plugin-bridge.demo.runtime-info"
    duplicate_manifest "${valid_manifest}")
set(duplicate_manifest_file "${manifest_validation_dir}/duplicate.aegisub-plugin.json")
file(WRITE "${duplicate_manifest_file}" "${duplicate_manifest}")
assert_manifest_rejected(
    "${duplicate_manifest_file}" "duplicate Macro ID" "Duplicate manifest Macro ID")

string(REPLACE
    "\"manifestVersion\": 2"
    "\"manifestVersion\": 4294967297"
    overflow_manifest "${valid_manifest}")
set(overflow_manifest_file "${manifest_validation_dir}/overflow.aegisub-plugin.json")
file(WRITE "${overflow_manifest_file}" "${overflow_manifest}")
assert_manifest_rejected(
    "${overflow_manifest_file}" "outside the native range" "Overflowing manifest version")

string(REPLACE
    "\"manifestVersion\": 2"
    "\"manifestVersion\": 1"
    legacy_manifest "${valid_manifest}")
set(legacy_manifest_file "${manifest_validation_dir}/legacy-v1.aegisub-plugin.json")
file(WRITE "${legacy_manifest_file}" "${legacy_manifest}")
assert_manifest_rejected(
    "${legacy_manifest_file}" "Unsupported C# extension manifestVersion"
    "Legacy manifest version")

string(REPLACE
    "\"contributions\": ["
    "\"contributions\": [\n    { \"kind\": \"settings\", \"id\": \"aegisub.plugin-bridge.demo.automation\" },"
    duplicate_contribution_manifest "${valid_manifest}")
set(duplicate_contribution_file
    "${manifest_validation_dir}/duplicate-contribution.aegisub-plugin.json")
file(WRITE "${duplicate_contribution_file}" "${duplicate_contribution_manifest}")
assert_manifest_rejected(
    "${duplicate_contribution_file}" "duplicate contribution ID"
    "Duplicate manifest contribution ID")

string(REPLACE
    "\"kind\": \"automation\""
    "\"kind\": \"unknown\""
    unknown_contribution_manifest "${valid_manifest}")
set(unknown_contribution_file
    "${manifest_validation_dir}/unknown-contribution.aegisub-plugin.json")
file(WRITE "${unknown_contribution_file}" "${unknown_contribution_manifest}")
assert_manifest_rejected(
    "${unknown_contribution_file}" "Unsupported C# extension contribution kind 'unknown'"
    "Unknown manifest contribution kind")

string(REPLACE
    "\"id\": \"aegisub.plugin-bridge.demo.automation\""
    "\"id\": \"\""
    empty_contribution_id_manifest "${valid_manifest}")
set(empty_contribution_id_file
    "${manifest_validation_dir}/empty-contribution-id.aegisub-plugin.json")
file(WRITE "${empty_contribution_id_file}" "${empty_contribution_id_manifest}")
assert_manifest_rejected(
    "${empty_contribution_id_file}" "requires a non-empty 'id'"
    "Empty manifest contribution ID")

string(REPLACE
    "\"description\": \"Validates lazy hostfxr activation through an Automation Macro.\""
    "\"description\": 42"
    wrong_type_manifest "${valid_manifest}")
set(wrong_type_manifest_file "${manifest_validation_dir}/wrong-type.aegisub-plugin.json")
file(WRITE "${wrong_type_manifest_file}" "${wrong_type_manifest}")
assert_manifest_rejected(
    "${wrong_type_manifest_file}" "field 'description' must be a string"
    "Wrongly typed optional manifest field")

string(REPLACE
    "CSharpBridgeDemo/Aegisub.Managed.SampleExtension.dll"
    "\\\\outside\\\\Aegisub.Managed.SampleExtension.dll"
    rooted_manifest "${valid_manifest}")
set(rooted_manifest_file "${manifest_validation_dir}/rooted.aegisub-plugin.json")
file(WRITE "${rooted_manifest_file}" "${rooted_manifest}")
assert_manifest_rejected(
    "${rooted_manifest_file}" "must be relative" "Windows root-relative entryAssembly")

message(STATUS "C# declarative QueryState stayed native and preserved lazy CLR activation")
