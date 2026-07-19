cmake_minimum_required(VERSION 3.25)

foreach(_required DOTNET_EXECUTABLE SMOKE_DLL SMOKE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()
if(NOT EXISTS "${SMOKE_DLL}")
    message(FATAL_ERROR "DependencyControl feed smoke assembly is missing: ${SMOKE_DLL}")
endif()

function(_json_escape _input _output)
    set(_value "${_input}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    set(${_output} "${_value}" PARENT_SCOPE)
endfunction()

set(_feeds
    "dependency-control|https://raw.githubusercontent.com/TypesettingTools/DependencyControl/master/DependencyControl.json"
    "line0-aegisub-scripts|https://raw.githubusercontent.com/TypesettingTools/line0-Aegisub-Scripts/master/DependencyControl.json"
    "aegisub-motion-depctrl|https://raw.githubusercontent.com/TypesettingTools/Aegisub-Motion/DepCtrl/DependencyControl.json"
    "ass-foundation|https://raw.githubusercontent.com/TypesettingTools/ASSFoundation/master/DependencyControl.json"
    "sub-inspector|https://raw.githubusercontent.com/TypesettingTools/SubInspector/master/DependencyControl.json"
)

file(REMOVE_RECURSE "${SMOKE_DIR}")
file(MAKE_DIRECTORY "${SMOKE_DIR}/feeds")
set(_report "${SMOKE_DIR}/report.ndjson")
file(WRITE "${_report}" "")
set(_failure_count 0)
set(_pass_count 0)

foreach(_entry IN LISTS _feeds)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _url)
    set(_path "${SMOKE_DIR}/feeds/${_name}.json")
    file(DOWNLOAD
        "${_url}"
        "${_path}"
        STATUS _download_status
        TLS_VERIFY ON
        TIMEOUT 30
        INACTIVITY_TIMEOUT 15
    )
    list(GET _download_status 0 _download_code)
    list(GET _download_status 1 _download_message)
    if(NOT _download_code EQUAL 0)
        math(EXPR _failure_count "${_failure_count} + 1")
        _json_escape("${_download_message}" _error)
        file(APPEND "${_report}"
            "{\"name\":\"${_name}\",\"url\":\"${_url}\",\"status\":\"downloadFailed\",\"error\":\"${_error}\"}\n")
        continue()
    endif()

    file(SIZE "${_path}" _feed_size)
    if(_feed_size GREATER 8388608)
        math(EXPR _failure_count "${_failure_count} + 1")
        file(APPEND "${_report}"
            "{\"name\":\"${_name}\",\"url\":\"${_url}\",\"status\":\"sizeLimitExceeded\",\"bytes\":${_feed_size}}\n")
        continue()
    endif()

    execute_process(
        COMMAND "${DOTNET_EXECUTABLE}" "${SMOKE_DLL}" --feed "${_path}" "${_url}"
        RESULT_VARIABLE _parse_result
        OUTPUT_VARIABLE _parse_output
        ERROR_VARIABLE _parse_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        TIMEOUT 30
    )
    if(NOT _parse_result EQUAL 0)
        math(EXPR _failure_count "${_failure_count} + 1")
        _json_escape("${_parse_error}" _error)
        file(APPEND "${_report}"
            "{\"name\":\"${_name}\",\"url\":\"${_url}\",\"status\":\"parseFailed\",\"bytes\":${_feed_size},\"error\":\"${_error}\"}\n")
        continue()
    endif()

    string(REGEX MATCH "feed_format=([^\r\n]+)" _format_match "${_parse_output}")
    set(_format "${CMAKE_MATCH_1}")
    string(REGEX MATCH "macros=([0-9]+)" _macros_match "${_parse_output}")
    set(_macros "${CMAKE_MATCH_1}")
    string(REGEX MATCH "modules=([0-9]+)" _modules_match "${_parse_output}")
    set(_modules "${CMAKE_MATCH_1}")
    string(REGEX MATCH "known_feeds=([0-9]+)" _known_match "${_parse_output}")
    set(_known_feeds "${CMAKE_MATCH_1}")
    math(EXPR _pass_count "${_pass_count} + 1")
    file(APPEND "${_report}"
        "{\"name\":\"${_name}\",\"url\":\"${_url}\",\"status\":\"passed\",\"bytes\":${_feed_size},\"format\":\"${_format}\",\"macros\":${_macros},\"modules\":${_modules},\"knownFeeds\":${_known_feeds}}\n")
endforeach()

message(STATUS
    "DependencyControl public feed corpus: ${_pass_count} passed, ${_failure_count} failed; report=${_report}")
if(_failure_count GREATER 0)
    message(FATAL_ERROR "DependencyControl public feed corpus regression failed")
endif()
