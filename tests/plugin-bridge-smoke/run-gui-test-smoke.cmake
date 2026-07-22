foreach(required AEGISUB_EXE SCENARIO_FILE SMOKE_DIR WORKING_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${AEGISUB_EXE}")
    message(FATAL_ERROR "AEGISUB_EXE does not name a built Aegisub executable")
endif()
if(NOT EXISTS "${SCENARIO_FILE}")
    message(FATAL_ERROR "SCENARIO_FILE does not name a GUI scenario")
endif()

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}")

function(run_gui_scenario case_name scenario_file)
    set(case_dir "${SMOKE_DIR}/${case_name}")
    file(MAKE_DIRECTORY "${case_dir}")
    set(command
        "${AEGISUB_EXE}"
        --gui-test run
        --scenario "${scenario_file}"
        --artifacts "${case_dir}"
        --profile-dir "${case_dir}/profile")
    if(ARGC GREATER 2)
        list(APPEND command ${ARGN})
    endif()

    # The process timeout is intentionally independent of the scenario's
    # per-step budget, so a hung wx event loop cannot leave this smoke running
    # indefinitely.
    execute_process(
        COMMAND ${command}
        WORKING_DIRECTORY "${WORKING_DIR}"
        TIMEOUT 90
        RESULT_VARIABLE result
        OUTPUT_FILE "${case_dir}/process.stdout.log"
        ERROR_FILE "${case_dir}/process.stderr.log")
    if(NOT "${result}" STREQUAL "0")
        file(READ "${case_dir}/process.stdout.log" stdout)
        file(READ "${case_dir}/process.stderr.log" stderr)
        message(FATAL_ERROR
            "GUI-test scenario '${case_name}' failed with ${result}\n"
            "stdout:\n${stdout}\n"
            "stderr:\n${stderr}")
    endif()
    if(NOT EXISTS "${case_dir}/ready.json" OR NOT EXISTS "${case_dir}/result.json")
        message(FATAL_ERROR
            "GUI-test scenario '${case_name}' did not produce ready.json and result.json")
    endif()

    file(READ "${case_dir}/result.json" result_json)
    string(JSON passed ERROR_VARIABLE json_error GET "${result_json}" passed)
    if(json_error OR NOT passed)
        message(FATAL_ERROR
            "GUI-test scenario '${case_name}' result was not marked passed: ${result_json}")
    endif()
    string(JSON timed_out ERROR_VARIABLE timeout_error GET "${result_json}" timed_out)
    if(NOT timeout_error AND timed_out)
        message(FATAL_ERROR
            "GUI-test scenario '${case_name}' reported a timeout: ${result_json}")
    endif()
    string(JSON result_host ERROR_VARIABLE host_error GET "${result_json}" host)
    if(host_error OR NOT result_host STREQUAL "gui-test")
        message(FATAL_ERROR
            "GUI-test scenario '${case_name}' has an invalid host: ${result_json}")
    endif()
endfunction()

run_gui_scenario(command "${SCENARIO_FILE}")

# Keep a real AutomationStepExecutor check beside the command scenario. The
# output path is supplied at runtime so the checked-in scenario remains free
# of machine-specific paths.
if(DEFINED AUTOMATION_SCENARIO_FILE)
    set(automation_scenario "${AUTOMATION_SCENARIO_FILE}")
else()
    set(automation_scenario
        "${CMAKE_CURRENT_LIST_DIR}/scenarios/gui-run-automation.json")
endif()
if(NOT EXISTS "${automation_scenario}")
    message(FATAL_ERROR "GUI automation scenario does not exist: ${automation_scenario}")
endif()
set(automation_dir "${SMOKE_DIR}/automation")
set(automation_output "${automation_dir}/output.ass")
run_gui_scenario(
    automation
    "${automation_scenario}"
    --input "output=${automation_output}")

if(NOT EXISTS "${automation_output}")
    message(FATAL_ERROR "GUI automation smoke did not save its subtitle output")
endif()
file(READ "${automation_output}" automation_output_text)
string(FIND "${automation_output_text}" "GUI automation smoke mutation" mutation_offset)
if(mutation_offset EQUAL -1)
    message(FATAL_ERROR
        "GUI automation smoke output did not contain the expected mutation")
endif()

file(READ "${automation_dir}/result.json" automation_result)
string(JSON step_action ERROR_VARIABLE step_error GET "${automation_result}" steps 0 action)
if(step_error OR NOT step_action STREQUAL "run_automation")
    message(FATAL_ERROR
        "GUI automation smoke did not execute a run_automation step: ${automation_result}")
endif()
string(JSON output_saved ERROR_VARIABLE output_error GET "${automation_result}" steps 0 output_saved)
if(output_error OR NOT output_saved)
    message(FATAL_ERROR
        "GUI automation smoke did not report output_saved=true: ${automation_result}")
endif()
