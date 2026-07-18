if(NOT DEFINED OUTPUT_FILE OR NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "C# subtitle mutation smoke output was not created")
endif()

file(READ "${OUTPUT_FILE}" output)
string(FIND "${output}" "CSharpBridgeDemo.aegisub-plugin.json" persisted_cli_script)
if(NOT persisted_cli_script EQUAL -1)
    message(FATAL_ERROR
        "The command-line C# script was incorrectly persisted into the subtitle output")
endif()

string(FIND "${output}" "Selected line\\Nsecond part" selected_result)
if(selected_result EQUAL -1)
    message(FATAL_ERROR "Selected subtitle line was not trimmed as expected")
endif()

if(EXPECT_SECOND_TRIMMED)
    set(expected_second "Unselected line\\Nmust stay unchanged")
else()
    set(expected_second "Unselected line   \\Nmust stay unchanged")
endif()
string(FIND "${output}" "${expected_second}" unselected_result)
if(unselected_result EQUAL -1)
    message(FATAL_ERROR "Second subtitle line did not have the expected mutation state")
endif()

if(DEFINED SUMMARY_FILE AND DEFINED EXPECTED_COMMIT_COUNT)
    if(NOT EXISTS "${SUMMARY_FILE}")
        message(FATAL_ERROR "C# subtitle mutation smoke summary was not created")
    endif()
    file(READ "${SUMMARY_FILE}" summary)
    string(FIND
        "${summary}"
        "automation.mutation.commit_count=${EXPECTED_COMMIT_COUNT}"
        commit_count_result)
    if(commit_count_result EQUAL -1)
        message(FATAL_ERROR
            "C# subtitle mutation smoke did not record the expected commit count")
    endif()
endif()

message(STATUS "C# subtitle snapshot/mutation output verified")
