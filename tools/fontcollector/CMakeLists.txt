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
set(AEGISUB_FONTCOLLECTOR_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/fontcollector-lib" CACHE PATH "Font collector import/static library output directory")

function(aegisub_fontcollector_set_output_dirs target_name)
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${AEGISUB_FONTCOLLECTOR_ARCHIVE_DIR}"
    )
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config} "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_${config} "${AEGISUB_FONTCOLLECTOR_RUNTIME_DIR}"
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
    ${AEGISUB_ROOT}/src/ass_time_projection.cpp
    ${AEGISUB_ROOT}/src/font_collector_core.cpp
    ${AEGISUB_ROOT}/src/font_collector_unicode.cpp
    ${AEGISUB_ROOT}/src/font_file_lister.cpp
    ${AEGISUB_ROOT}/src/string_codec.cpp
    ${AEGISUB_ROOT}/src/text_file_reader.cpp
    ${AEGISUB_ROOT}/src/text_file_writer.cpp
)

add_library(aegisub_fontcollector SHARED ${AEGISUB_FONTCOLLECTOR_SOURCES})
aegisub_fontcollector_set_output_dirs(aegisub_fontcollector)
set_target_properties(aegisub_fontcollector PROPERTIES
    OUTPUT_NAME aegisub_fontcollector
    POSITION_INDEPENDENT_CODE ON
)
if(MSVC AND AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME)
    set_property(TARGET aegisub_fontcollector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_compile_features(aegisub_fontcollector PRIVATE cxx_std_20)
target_compile_definitions(aegisub_fontcollector
    PRIVATE
        AEGISUB_FONTCOLLECTOR_BUILD
        CMAKE_BUILD
)
if(WIN32)
    target_compile_definitions(aegisub_fontcollector PRIVATE NOMINMAX UNICODE _UNICODE)
endif()
target_include_directories(aegisub_fontcollector
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${AEGISUB_ROOT}/src"
        ${Iconv_INCLUDE_DIR}
)
target_link_libraries(aegisub_fontcollector
    PRIVATE
        aegisub_fontcollector_libaegisub
        Boost::headers
        ${AEGISUB_FONTCOLLECTOR_ICONV_LIBRARIES}
        Boost::regex
)

if(WIN32)
    target_sources(aegisub_fontcollector PRIVATE ${AEGISUB_ROOT}/src/font_file_lister_gdi.cpp)
    target_link_libraries(aegisub_fontcollector PRIVATE Advapi32 Gdi32 Shell32 Usp10)
elseif(APPLE)
    target_sources(aegisub_fontcollector PRIVATE ${AEGISUB_ROOT}/src/font_file_lister_coretext.mm)
    target_link_libraries(aegisub_fontcollector PRIVATE "-framework AppKit" "-framework CoreText")
else()
    find_package(Fontconfig REQUIRED)
    target_sources(aegisub_fontcollector PRIVATE ${AEGISUB_ROOT}/src/font_file_lister_fontconfig.cpp)
    target_link_libraries(aegisub_fontcollector PRIVATE Fontconfig::Fontconfig)
endif()

add_executable(fontcollector_cli fontcollector_cli.cpp)
aegisub_fontcollector_set_output_dirs(fontcollector_cli)
set_target_properties(fontcollector_cli PROPERTIES OUTPUT_NAME fontcollector)
if(MSVC AND AEGISUB_FONTCOLLECTOR_STATIC_RUNTIME)
    set_property(TARGET fontcollector_cli PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_compile_features(fontcollector_cli PRIVATE cxx_std_20)
target_link_libraries(fontcollector_cli PRIVATE aegisub_fontcollector CLI11::CLI11)
if(WIN32)
    target_link_libraries(fontcollector_cli PRIVATE Shell32)
endif()
target_include_directories(fontcollector_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_custom_command(TARGET fontcollector_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:aegisub_fontcollector>
        $<TARGET_FILE_DIR:fontcollector_cli>
)
