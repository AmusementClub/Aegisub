foreach(required AEGISUB_EXE SCENARIO_FILE SMOKE_DIR WORKING_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}")
execute_process(
    COMMAND "${AEGISUB_EXE}"
        --gui-test run
        --scenario "${SCENARIO_FILE}"
        --artifacts "${SMOKE_DIR}"
        --profile-dir "${SMOKE_DIR}/profile"
    WORKING_DIRECTORY "${WORKING_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "GUI-test scenario failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT EXISTS "${SMOKE_DIR}/ready.json" OR NOT EXISTS "${SMOKE_DIR}/result.json")
    message(FATAL_ERROR "GUI-test scenario did not produce ready.json and result.json")
endif()
file(READ "${SMOKE_DIR}/result.json" result_json)
string(JSON passed ERROR_VARIABLE json_error GET "${result_json}" passed)
if(json_error OR NOT passed)
    message(FATAL_ERROR "GUI-test result was not marked passed: ${result_json}")
endif()
string(JSON result_host ERROR_VARIABLE host_error GET "${result_json}" host)
if(host_error OR NOT result_host STREQUAL "gui-test")
    message(FATAL_ERROR "GUI-test result has an invalid host: ${result_json}")
endif()
