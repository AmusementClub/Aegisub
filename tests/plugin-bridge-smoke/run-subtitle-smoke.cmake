if(NOT DEFINED AEGISUB_EXE OR NOT EXISTS "${AEGISUB_EXE}")
    message(FATAL_ERROR "AEGISUB_EXE does not name a built Aegisub executable")
endif()
if(NOT DEFINED SCRIPT_FILE OR NOT EXISTS "${SCRIPT_FILE}")
    message(FATAL_ERROR "SCRIPT_FILE does not name the deployed C# manifest")
endif()
if(NOT DEFINED INPUT_FILE OR NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "INPUT_FILE does not name the subtitle smoke fixture")
endif()
if(NOT DEFINED NOOP_INPUT_FILE OR NOT EXISTS "${NOOP_INPUT_FILE}")
    message(FATAL_ERROR "NOOP_INPUT_FILE does not name the no-op subtitle smoke fixture")
endif()
if(NOT DEFINED SMOKE_DIR)
    message(FATAL_ERROR "SMOKE_DIR is required")
endif()

file(MAKE_DIRECTORY "${SMOKE_DIR}")

function(run_subtitle_case case_name input_file output_file expected_commit_count second_trimmed)
    set(trace_dir "${SMOKE_DIR}/${case_name}")
    file(REMOVE_RECURSE "${trace_dir}")
    file(MAKE_DIRECTORY "${trace_dir}")
    file(REMOVE
        "${output_file}"
        "${trace_dir}/process.stdout.log"
        "${trace_dir}/process.stderr.log")

    set(command
        "${AEGISUB_EXE}"
        --cli session automation
        --script "${SCRIPT_FILE}"
        --macro aegisub.plugin-bridge.demo.trim-selected-line-endings
        --subtitle "${input_file}"
        --output-subtitle "${output_file}"
        --trace-dir "${trace_dir}")
    if(ARGC GREATER 5)
        list(APPEND command --selection "${ARGV5}" --active-row "${ARGV6}")
    endif()

    if(case_name STREQUAL "mutate")
        set(context_dump "${trace_dir}/managed-debug-context.json")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "AEGISUB_CSHARP_CONTEXT_DUMP=${context_dump}"
                ${command}
            RESULT_VARIABLE process_result
            OUTPUT_FILE "${trace_dir}/process.stdout.log"
            ERROR_FILE "${trace_dir}/process.stderr.log"
        )
    else()
        execute_process(
            COMMAND ${command}
            RESULT_VARIABLE process_result
            OUTPUT_FILE "${trace_dir}/process.stdout.log"
            ERROR_FILE "${trace_dir}/process.stderr.log"
        )
    endif()
    if(NOT process_result EQUAL 0)
        file(READ "${trace_dir}/process.stdout.log" process_stdout)
        file(READ "${trace_dir}/process.stderr.log" process_stderr)
        message(FATAL_ERROR
            "C# subtitle smoke case ${case_name} failed with ${process_result}\n"
            "stdout:\n${process_stdout}\n"
            "stderr:\n${process_stderr}")
    endif()

    if(case_name STREQUAL "mutate")
        if(NOT EXISTS "${context_dump}")
            message(FATAL_ERROR "C# subtitle smoke did not export its managed debug context")
        endif()
        file(READ "${context_dump}" debug_context)
        string(FIND "${debug_context}" "\"selectedEventIds\"" selection_snapshot)
        if(selection_snapshot EQUAL -1)
            message(FATAL_ERROR "Exported C# debug context did not contain the subtitle selection")
        endif()
        string(FIND "${debug_context}" "Unselected line" unselected_event_snapshot)
        if(NOT unselected_event_snapshot EQUAL -1)
            message(FATAL_ERROR
                "Selection-scoped C# debug context contained an unselected subtitle event")
        endif()
        string(FIND "${debug_context}" "Selected payload" selected_extradata_snapshot)
        if(selected_extradata_snapshot EQUAL -1)
            message(FATAL_ERROR
                "Selection-scoped C# debug context omitted referenced Extradata")
        endif()
        string(FIND "${debug_context}" "Unused payload" unused_extradata_snapshot)
        if(NOT unused_extradata_snapshot EQUAL -1)
            message(FATAL_ERROR
                "Selection-scoped C# debug context contained unreferenced Extradata")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT_FILE=${output_file}"
            "-DSUMMARY_FILE=${trace_dir}/summary.txt"
            "-DEXPECTED_COMMIT_COUNT=${expected_commit_count}"
            "-DEXPECT_SECOND_TRIMMED=${second_trimmed}"
            -P "${CMAKE_CURRENT_LIST_DIR}/verify-trim-selected.cmake"
        RESULT_VARIABLE verify_result
    )
    if(NOT verify_result EQUAL 0)
        message(FATAL_ERROR "C# subtitle smoke case ${case_name} output verification failed")
    endif()
    message(STATUS "C# subtitle smoke case ${case_name} passed")
endfunction()

set(mutate_output "${SMOKE_DIR}/trim-selected-output.ass")
set(noop_output "${SMOKE_DIR}/trim-selected-noop-output.ass")
set(batch_output "${SMOKE_DIR}/trim-selected-batch-output.ass")

run_subtitle_case(mutate "${INPUT_FILE}" "${mutate_output}" 1 FALSE)
run_subtitle_case(noop "${NOOP_INPUT_FILE}" "${noop_output}" 0 FALSE)
run_subtitle_case(batch "${INPUT_FILE}" "${batch_output}" 1 TRUE "7,8" "7")
