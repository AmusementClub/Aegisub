function(aegisub_add_luasocket_target)
    if(TARGET luasocket)
        return()
    endif()

    if(NOT DEFINED AEGISUB_LUASOCKET_ROOT OR AEGISUB_LUASOCKET_ROOT STREQUAL "")
        set(AEGISUB_LUASOCKET_ROOT "${PROJECT_SOURCE_DIR}/vendor/luasocket" PARENT_SCOPE)
        set(AEGISUB_LUASOCKET_ROOT "${PROJECT_SOURCE_DIR}/vendor/luasocket")
    endif()

    if(NOT EXISTS "${AEGISUB_LUASOCKET_ROOT}/src/luasocket.c")
        message(FATAL_ERROR "LuaSocket source tree not found at ${AEGISUB_LUASOCKET_ROOT}. Initialize the submodule or set AEGISUB_LUASOCKET_ROOT.")
    endif()

    set(luasocket_include_dirs
        "${AEGISUB_LUASOCKET_ROOT}/src"
        "${PROJECT_SOURCE_DIR}/vendor/luajit/src"
    )

    set(luasocket_compile_definitions
        LUASOCKET_API=
        LUASOCKET_NODEBUG
    )

    set(socket_core_sources
        "${AEGISUB_LUASOCKET_ROOT}/src/luasocket.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/inet.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/tcp.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/udp.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/io.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/buffer.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/timeout.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/options.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/auxiliar.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/compat.c"
        "${AEGISUB_LUASOCKET_ROOT}/src/except.c"
    )

    if(UNIX AND NOT APPLE)
        list(APPEND socket_core_sources
            "${AEGISUB_LUASOCKET_ROOT}/src/unix.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/unixstream.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/unixdgram.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/select.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/usocket.c"
        )
        set(socket_libraries m)
    elseif(APPLE)
        list(APPEND socket_core_sources
            "${AEGISUB_LUASOCKET_ROOT}/src/unix.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/unixstream.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/unixdgram.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/select.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/usocket.c"
        )
        set(socket_libraries m)
    elseif(WIN32)
        list(APPEND socket_core_sources
            "${AEGISUB_LUASOCKET_ROOT}/src/select.c"
            "${AEGISUB_LUASOCKET_ROOT}/src/wsocket.c"
        )
        set(socket_libraries ws2_32 iphlpapi)
    endif()

    add_library(socket_core OBJECT ${socket_core_sources})
    target_include_directories(socket_core PRIVATE ${luasocket_include_dirs})
    target_compile_definitions(socket_core PRIVATE ${luasocket_compile_definitions})

    add_library(mime_core OBJECT
        "${AEGISUB_LUASOCKET_ROOT}/src/mime.c"
    )
    target_include_directories(mime_core PRIVATE ${luasocket_include_dirs})
    target_compile_definitions(mime_core PRIVATE ${luasocket_compile_definitions})

    add_library(luasocket STATIC
        $<TARGET_OBJECTS:socket_core>
        $<TARGET_OBJECTS:mime_core>
    )
    target_include_directories(luasocket PUBLIC ${luasocket_include_dirs})
    target_link_libraries(luasocket PUBLIC ${socket_libraries})
endfunction()