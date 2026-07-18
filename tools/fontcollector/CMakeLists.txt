cmake_minimum_required(VERSION 3.16)

set(AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(POLICY CMP0091)
        cmake_policy(SET CMP0091 NEW)
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" FORCE)
    set(AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME ON)
    project(AegisubFontCollector VERSION 1.0 LANGUAGES CXX)
    set(AEGISUB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Path to the Aegisub source tree")
    include(CTest)
else()
    set(AEGISUB_ROOT "${PROJECT_SOURCE_DIR}")
    if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
        set(AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME ON)
    endif()
endif()
get_filename_component(AEGISUB_ROOT "${AEGISUB_ROOT}" ABSOLUTE)

find_package(CLI11 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS regex)
find_package(Iconv REQUIRED)

set(AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES Iconv::Iconv)
if(WIN32 AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_aegisub_fontcollector_vcpkg_dir "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
    set(_aegisub_fontcollector_iconv_release_libs)
    set(_aegisub_fontcollector_iconv_debug_libs)
    foreach(lib_name iconv charset)
        if(EXISTS "${_aegisub_fontcollector_vcpkg_dir}/lib/${lib_name}.lib")
            list(APPEND _aegisub_fontcollector_iconv_release_libs "${_aegisub_fontcollector_vcpkg_dir}/lib/${lib_name}.lib")
        endif()
        if(EXISTS "${_aegisub_fontcollector_vcpkg_dir}/debug/lib/${lib_name}.lib")
            list(APPEND _aegisub_fontcollector_iconv_debug_libs "${_aegisub_fontcollector_vcpkg_dir}/debug/lib/${lib_name}.lib")
        endif()
    endforeach()
    if(_aegisub_fontcollector_iconv_release_libs)
        set(AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES)
        foreach(iconv_lib IN LISTS _aegisub_fontcollector_iconv_debug_libs)
            list(APPEND AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES "$<$<CONFIG:Debug>:${iconv_lib}>")
        endforeach()
        foreach(iconv_lib IN LISTS _aegisub_fontcollector_iconv_release_libs)
            list(APPEND AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES "$<$<NOT:$<CONFIG:Debug>>:${iconv_lib}>")
        endforeach()
    endif()
endif()

set(AEGISUB_FONTCOLLECTOR_RUNTIME_DIR "${CMAKE_BINARY_DIR}/fontcollector" CACHE PATH "Font collector runtime output directory")
set(AEGISUB_FONTCOLLECTOR_STANDALONE_RUNTIME_DIR "${CMAKE_BINARY_DIR}/fontcollector-standalone" CACHE PATH "Standalone font collector CLI output directory")
set(AEGISUB_FONTCOLLECTOR_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/fontcollector-lib" CACHE PATH "Font collector import/static library output directory")

function(aegisub_fontcollector_set_output_dirs target_name)
    set(runtime_dir "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}")
    if(ARGC GREATER 1)
        set(runtime_dir "${ARGV1}")
    endif()
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${runtime_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${runtime_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${AEGISUB_FONTCOLLECTOR_ARCHIVE_DIR}"
    )
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config} "${runtime_dir}"
            LIBRARY_OUTPUT_DIRECTORY_${config} "${runtime_dir}"
            ARCHIVE_OUTPUT_DIRECTORY_${config} "${AEGISUB_FONTCOLLECTOR_ARCHIVE_DIR}/${config}"
        )
    endforeach()
endfunction()

set(AEGISUB_FONTCOLLECTOR_LAGI_SOURCES
    ${AEGISUB_ROOT}/libaegisub/ass/time.cpp
    ${AEGISUB_ROOT}/libaegisub/ass/uuencode.cpp
    ${AEGISUB_ROOT}/libaegisub/common/charset.cpp
    ${AEGISUB_ROOT}/libaegisub/common/charset_6937.cpp
    ${AEGISUB_ROOT}/libaegisub/common/charset_conv.cpp
    ${AEGISUB_ROOT}/libaegisub/common/color.cpp
    ${AEGISUB_ROOT}/libaegisub/common/file_mapping.cpp
    ${AEGISUB_ROOT}/libaegisub/common/format.cpp
    ${AEGISUB_ROOT}/libaegisub/common/fs.cpp
    ${AEGISUB_ROOT}/libaegisub/common/io.cpp
    ${AEGISUB_ROOT}/libaegisub/common/line_iterator.cpp
    ${AEGISUB_ROOT}/libaegisub/common/parser.cpp
    ${AEGISUB_ROOT}/libaegisub/common/vfr.cpp
    ${AEGISUB_ROOT}/libaegisub/common/native_library.cpp
    fontcollector_libaegisub_log.cpp
)

if(WIN32)
    list(APPEND AEGISUB_FONTCOLLECTOR_LAGI_SOURCES
        ${AEGISUB_ROOT}/libaegisub/windows/access.cpp
        ${AEGISUB_ROOT}/libaegisub/windows/charset_conv_win.cpp
        ${AEGISUB_ROOT}/libaegisub/windows/fs.cpp
        ${AEGISUB_ROOT}/libaegisub/windows/util_win.cpp
    )
else()
    list(APPEND AEGISUB_FONTCOLLECTOR_LAGI_SOURCES
        ${AEGISUB_ROOT}/libaegisub/unix/access.cpp
        ${AEGISUB_ROOT}/libaegisub/unix/fs.cpp
        ${AEGISUB_ROOT}/libaegisub/unix/util.cpp
    )
endif()

add_library(aegisub_fontcollector_libaegisub STATIC ${AEGISUB_FONTCOLLECTOR_LAGI_SOURCES})
aegisub_fontcollector_set_output_dirs(aegisub_fontcollector_libaegisub)
set_target_properties(aegisub_fontcollector_libaegisub PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
if(MSVC AND AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME)
    set_property(TARGET aegisub_fontcollector_libaegisub PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_compile_features(aegisub_fontcollector_libaegisub PUBLIC cxx_std_20)
target_compile_definitions(aegisub_fontcollector_libaegisub
    PRIVATE
        CMAKE_BUILD
)
if(WIN32)
    target_compile_definitions(aegisub_fontcollector_libaegisub PRIVATE NOMINMAX UNICODE _UNICODE)
endif()
target_include_directories(aegisub_fontcollector_libaegisub
    PUBLIC
        "${AEGISUB_ROOT}/libaegisub/include"
    PRIVATE
        "${AEGISUB_ROOT}/libaegisub"
        ${Iconv_INCLUDE_DIR}
)
target_link_libraries(aegisub_fontcollector_libaegisub
    PUBLIC
        Boost::headers
    PRIVATE
        ${AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES}
)

set(AEGISUB_FONTCOLLECTOR_SOURCES
    fontcollector_api.cpp
    ${AEGISUB_ROOT}/src/ass_attachment.cpp
    ${AEGISUB_ROOT}/src/ass_dialogue.cpp
    ${AEGISUB_ROOT}/src/ass_entry.cpp
    ${AEGISUB_ROOT}/src/ass_file.cpp
    ${AEGISUB_ROOT}/src/ass_io_core.cpp
    ${AEGISUB_ROOT}/src/ass_override.cpp
    ${AEGISUB_ROOT}/src/ass_parser.cpp
    ${AEGISUB_ROOT}/src/ass_style.cpp
    ${AEGISUB_ROOT}/src/ass_style_resolution.cpp
    ${AEGISUB_ROOT}/src/ass_time_projection.cpp
    ${AEGISUB_ROOT}/src/font_collector_core.cpp
    ${AEGISUB_ROOT}/src/font_collector_unicode.cpp
    ${AEGISUB_ROOT}/src/font_family_catalog.cpp
    ${AEGISUB_ROOT}/src/font_name_normalization.cpp
    ${AEGISUB_ROOT}/src/font_file_lister.cpp
    ${AEGISUB_ROOT}/src/font_file_lister_libass.cpp
    ${AEGISUB_ROOT}/src/font_matching_common.cpp
    ${AEGISUB_ROOT}/src/font_matching_libass.cpp
    ${AEGISUB_ROOT}/src/string_codec.cpp
    ${AEGISUB_ROOT}/src/text_file_reader.cpp
    ${AEGISUB_ROOT}/src/text_file_writer.cpp
)

function(aegisub_fontcollector_configure_library target_name linkage)
    aegisub_fontcollector_set_output_dirs(${target_name})
    set_target_properties(${target_name} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        AEGISUB_FONTCOLLECTOR_HAS_FONTCONFIG FALSE
    )
    if(MSVC AND AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME)
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name}
        PRIVATE
            CMAKE_BUILD
    )
    if(linkage STREQUAL "SHARED")
        target_compile_definitions(${target_name} PRIVATE AEGISUB_FONTCOLLECTOR_BUILD)
    else()
        target_compile_definitions(${target_name} PUBLIC AEGISUB_FONTCOLLECTOR_STATIC)
    endif()
    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE NOMINMAX UNICODE _UNICODE)
    endif()
    target_include_directories(${target_name}
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE
            "${AEGISUB_ROOT}/src"
            ${Iconv_INCLUDE_DIR}
    )
    target_link_libraries(${target_name}
        PRIVATE
            aegisub_fontcollector_libaegisub
            Boost::headers
            ${AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES}
    )
    if(linkage STREQUAL "STATIC")
        target_link_libraries(${target_name} PUBLIC Boost::regex)
    else()
        target_link_libraries(${target_name} PRIVATE Boost::regex)
    endif()

    if(MSVC AND AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME)
        # Boost::regex from vcpkg static triplet may default to debug CRT,
        # which conflicts with the target's Release /MT. Suppress the stale default lib.
        target_link_options(${target_name} PRIVATE
            "$<$<NOT:$<CONFIG:Debug>>:/NODEFAULTLIB:LIBCMTD>"
        )
    endif()

    if(WIN32)
        # Platform: GDI/DirectWrite. Libass: DirectWrite. No Fontconfig.
        target_sources(${target_name} PRIVATE
            ${AEGISUB_ROOT}/src/font_family_catalog_win.cpp
            ${AEGISUB_ROOT}/src/font_file_lister_gdi.cpp
            ${AEGISUB_ROOT}/src/font_file_lister_dwrite.cpp)
        if(linkage STREQUAL "STATIC")
            target_link_libraries(${target_name} PUBLIC Advapi32 Gdi32 Shell32 Usp10 dwrite Version)
        else()
            target_link_libraries(${target_name} PRIVATE Advapi32 Gdi32 Shell32 Usp10 dwrite Version)
        endif()
    elseif(APPLE)
        # Platform: CoreText. Libass: optional Fontconfig (not REQUIRED).
        target_sources(${target_name} PRIVATE
            ${AEGISUB_ROOT}/src/font_file_lister_coretext.mm)
        find_package(Fontconfig)
        if(Fontconfig_FOUND)
            target_compile_definitions(${target_name} PRIVATE AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG)
            set_property(TARGET ${target_name} PROPERTY AEGISUB_FONTCOLLECTOR_HAS_FONTCONFIG TRUE)
            target_sources(${target_name} PRIVATE
                ${AEGISUB_ROOT}/src/font_file_lister_fontconfig.cpp)
            if(linkage STREQUAL "STATIC")
                target_link_libraries(${target_name} PUBLIC "-framework AppKit" "-framework CoreText" Fontconfig::Fontconfig)
            else()
                target_link_libraries(${target_name} PRIVATE "-framework AppKit" "-framework CoreText" Fontconfig::Fontconfig)
            endif()
        else()
            message(STATUS
                "Fontconfig not found: fontcollector libass matcher disabled on Apple; "
                "enable the vcpkg manifest feature 'fontconfig' to install it")
            if(linkage STREQUAL "STATIC")
                target_link_libraries(${target_name} PUBLIC "-framework AppKit" "-framework CoreText")
            else()
                target_link_libraries(${target_name} PRIVATE "-framework AppKit" "-framework CoreText")
            endif()
        endif()
    else()
        # Linux: Fontconfig serves platform and libass matchers.
        find_package(Fontconfig REQUIRED)
        set_property(TARGET ${target_name} PROPERTY AEGISUB_FONTCOLLECTOR_HAS_FONTCONFIG TRUE)
        target_sources(${target_name} PRIVATE ${AEGISUB_ROOT}/src/font_file_lister_fontconfig.cpp)
        if(linkage STREQUAL "STATIC")
            target_link_libraries(${target_name} PUBLIC Fontconfig::Fontconfig)
        else()
            target_link_libraries(${target_name} PRIVATE Fontconfig::Fontconfig)
        endif()
    endif()
endfunction()

add_library(aegisub_fontcollector SHARED ${AEGISUB_FONTCOLLECTOR_SOURCES})
set_target_properties(aegisub_fontcollector PROPERTIES OUTPUT_NAME aegisub_fontcollector)
aegisub_fontcollector_configure_library(aegisub_fontcollector SHARED)

add_library(aegisub_fontcollector_static STATIC ${AEGISUB_FONTCOLLECTOR_SOURCES})
set_target_properties(aegisub_fontcollector_static PROPERTIES OUTPUT_NAME aegisub_fontcollector_static)
aegisub_fontcollector_configure_library(aegisub_fontcollector_static STATIC)

function(aegisub_fontcollector_add_cli target_name core_target runtime_dir)
    add_executable(${target_name} fontcollector_cli.cpp)
    aegisub_fontcollector_set_output_dirs(${target_name} "${runtime_dir}")
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME fontcollector)
    if(MSVC AND AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME)
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    get_target_property(fontconfig_enabled ${core_target} AEGISUB_FONTCOLLECTOR_HAS_FONTCONFIG)
    if(fontconfig_enabled)
        target_compile_definitions(${target_name} PRIVATE AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG)
    endif()
    target_sources(${target_name} PRIVATE
        ${AEGISUB_ROOT}/libaegisub/common/cajun/elements.cpp
        ${AEGISUB_ROOT}/libaegisub/common/cajun/writer.cpp
    )
    target_link_libraries(${target_name} PRIVATE ${core_target} CLI11::CLI11)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE Shell32)
    endif()
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${AEGISUB_ROOT}/libaegisub/include"
    )
endfunction()

# DLL wrapper: links the exported C API core DLL and writes both files to
# AEGISUB_FONTCOLLECTOR_RUNTIME_DIR.
aegisub_fontcollector_add_cli(fontcollector_cli aegisub_fontcollector "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}")

# Standalone CLI: links the static core into the executable and writes it to a
# separate directory so its output does not collide with the DLL wrapper.
aegisub_fontcollector_add_cli(fontcollector_cli_standalone aegisub_fontcollector_static "${AEGISUB_FONTCOLLECTOR_STANDALONE_RUNTIME_DIR}")

add_custom_command(TARGET fontcollector_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:aegisub_fontcollector>
        $<TARGET_FILE_DIR:fontcollector_cli>
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING AND WIN32)
    add_executable(fontcollector_api_smoke tests/fontcollector_api_smoke.cpp)
    aegisub_fontcollector_set_output_dirs(fontcollector_api_smoke "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}")
    target_compile_features(fontcollector_api_smoke PRIVATE cxx_std_20)
    target_link_libraries(fontcollector_api_smoke PRIVATE aegisub_fontcollector)
    target_include_directories(fontcollector_api_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

    add_test(NAME fontcollector_api_smoke COMMAND fontcollector_api_smoke)
    add_test(
        NAME fontcollector_cli_smoke
        COMMAND ${CMAKE_COMMAND}
            "-DFONTCOLLECTOR_EXE=$<TARGET_FILE:fontcollector_cli>"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/fontcollector-cli-smoke"
            -P "${CMAKE_CURRENT_LIST_DIR}/tests/fontcollector_cli_smoke.cmake"
    )
    add_test(
        NAME fontcollector_cli_standalone_smoke
        COMMAND ${CMAKE_COMMAND}
            "-DFONTCOLLECTOR_EXE=$<TARGET_FILE:fontcollector_cli_standalone>"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/fontcollector-cli-standalone-smoke"
            -P "${CMAKE_CURRENT_LIST_DIR}/tests/fontcollector_cli_smoke.cmake"
    )
endif()
