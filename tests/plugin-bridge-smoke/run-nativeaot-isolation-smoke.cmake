cmake_minimum_required(VERSION 3.16)

foreach(required AEGISUB_EXE SCENARIO_FILE ISOLATION_DIR WORKING_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${AEGISUB_EXE}")
    message(FATAL_ERROR "AEGISUB_EXE does not name a built Aegisub executable")
endif()
if(NOT EXISTS "${SCENARIO_FILE}")
    message(FATAL_ERROR "SCENARIO_FILE does not name a DependencyControl scenario")
endif()
file(REMOVE_RECURSE "${ISOLATION_DIR}")
set(state_dir "${ISOLATION_DIR}/state")
set(trace_dir "${ISOLATION_DIR}/trace")
set(missing_dotnet_root "${ISOLATION_DIR}/missing-dotnet-root")
file(MAKE_DIRECTORY "${state_dir}" "${trace_dir}")

set(stdout_file "${trace_dir}/process.stdout.log")
set(stderr_file "${trace_dir}/process.stderr.log")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "AEGISUB_DOTNET_ROOT=${missing_dotnet_root}"
        "AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT=${state_dir}"
        "${AEGISUB_EXE}"
        --headless run
        --scenario "${SCENARIO_FILE}"
        --artifacts "${trace_dir}"
    WORKING_DIRECTORY "${WORKING_DIR}"
    TIMEOUT 240
    RESULT_VARIABLE process_result
    OUTPUT_FILE "${stdout_file}"
    ERROR_FILE "${stderr_file}"
)
if(NOT "${process_result}" STREQUAL "0")
    set(stdout "")
    set(stderr "")
    if(EXISTS "${stdout_file}")
        file(READ "${stdout_file}" stdout)
    endif()
    if(EXISTS "${stderr_file}")
        file(READ "${stderr_file}" stderr)
    endif()
    message(FATAL_ERROR
        "DependencyControl NativeAOT isolation smoke failed with ${process_result}\n"
        "stdout:\n${stdout}\n"
        "stderr:\n${stderr}")
endif()

set(result_file "${trace_dir}/result.json")
if(NOT EXISTS "${result_file}")
    message(FATAL_ERROR "DependencyControl NativeAOT smoke did not write result.json")
endif()
file(READ "${result_file}" result_json)
string(JSON passed ERROR_VARIABLE json_error GET "${result_json}" passed)
if(json_error OR NOT passed)
    message(FATAL_ERROR
        "DependencyControl NativeAOT smoke result was not marked passed: ${result_json}")
endif()
