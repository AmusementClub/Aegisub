if(NOT DEFINED AEGISUB_EXE)
	message(FATAL_ERROR "AEGISUB_EXE is required")
endif()

function(run_help EXPECTED_TEXT)
	execute_process(
		COMMAND "${AEGISUB_EXE}" ${ARGN}
		RESULT_VARIABLE result
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error
		TIMEOUT 5
	)
	if(NOT result STREQUAL "0")
		message(FATAL_ERROR
			"Aegisub ${ARGN} returned ${result}, expected 0\nstdout:\n${output}\nstderr:\n${error}")
	endif()

	set(combined "${output}${error}")
	string(FIND "${combined}" "${EXPECTED_TEXT}" expected_at)
	if(expected_at LESS 0)
		message(FATAL_ERROR
			"Aegisub ${ARGN} output did not contain ${EXPECTED_TEXT}\noutput:\n${combined}")
	endif()

	string(FIND "${combined}" "${AEGISUB_EXE}" leaked_path_at)
	if(NOT leaked_path_at LESS 0)
		message(FATAL_ERROR "Aegisub ${ARGN} help exposed its executable path")
	endif()
endfunction()

run_help("headless" --help)
run_help("probe" headless --help)
run_help("playback" headless probe --help)
run_help("--probe-video" headless probe playback --help)
run_help("--scenario" gui-test run --help)
run_help("--matcher" fontcollector check --help)
