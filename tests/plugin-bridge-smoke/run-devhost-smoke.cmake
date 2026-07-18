if(NOT DEFINED DOTNET_EXECUTABLE OR NOT EXISTS "${DOTNET_EXECUTABLE}")
    message(FATAL_ERROR "DOTNET_EXECUTABLE does not name dotnet")
endif()
if(NOT DEFINED DEVHOST_DLL OR NOT EXISTS "${DEVHOST_DLL}")
    message(FATAL_ERROR "DEVHOST_DLL does not name the built managed DevHost")
endif()
if(NOT DEFINED SMOKE_DIR)
    message(FATAL_ERROR "SMOKE_DIR is required")
endif()

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}")
set(result_file "${SMOKE_DIR}/macro-result.json")
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" "${DEVHOST_DLL}" --result "${result_file}"
    RESULT_VARIABLE process_result
    OUTPUT_VARIABLE process_stdout
    ERROR_VARIABLE process_stderr
)
if(NOT process_result EQUAL 0)
    message(FATAL_ERROR
        "Managed DevHost failed with ${process_result}\n"
        "stdout:\n${process_stdout}\n"
        "stderr:\n${process_stderr}")
endif()
if(NOT EXISTS "${result_file}")
    message(FATAL_ERROR "Managed DevHost did not write its Macro result")
endif()
file(READ "${result_file}" result)
string(FIND "${result}" "\"expectedDocumentToken\": 1" token_match)
string(FIND "${result}" "\"eventId\": 101" selected_patch)
string(FIND "${result}" "\"eventId\": 102" unselected_patch)
if(token_match EQUAL -1 OR selected_patch EQUAL -1 OR NOT unselected_patch EQUAL -1)
    message(FATAL_ERROR "Managed DevHost result did not contain the expected selected-only mutation")
endif()

set(failure_context "${SMOKE_DIR}/failure-context.json")
file(WRITE "${failure_context}" "{\"invocationToken\":2,\"subtitles\":null}")
execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" "${DEVHOST_DLL}"
        --context "${failure_context}"
    RESULT_VARIABLE failure_result
    OUTPUT_VARIABLE failure_stdout
    ERROR_VARIABLE failure_stderr
)
if(NOT failure_result EQUAL 2)
    message(FATAL_ERROR
        "Managed DevHost failure case returned ${failure_result}, expected 2\n"
        "stdout:\n${failure_stdout}\n"
        "stderr:\n${failure_stderr}")
endif()
string(FIND "${failure_stderr}" "\"code\": \"aegisub.subtitle.required\"" failure_code)
string(FIND "${failure_stderr}" "\"category\": \"extension\"" failure_category)
string(FIND "${failure_stderr}" "\"retryable\": false" failure_retryable)
if(failure_code EQUAL -1 OR failure_category EQUAL -1 OR failure_retryable EQUAL -1)
    message(FATAL_ERROR "Managed DevHost failure did not emit the expected structured error envelope")
endif()

message(STATUS "Standalone C# Macro DevHost replayed a subtitle snapshot and structured failure successfully")
