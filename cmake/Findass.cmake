find_package(PkgConfig QUIET)
pkg_check_modules(PC_ass QUIET libass)

# The application resolves libass symbols at runtime. The build only consumes
# the public headers plus an optional shared library used for app-local staging.
set(_ass_include_hint "${ass_INCLUDE_DIR}")
if(NOT _ass_include_hint AND ass_INCLUDE_DIRS)
  list(GET ass_INCLUDE_DIRS 0 _ass_include_hint)
endif()
unset(ass_INCLUDE_DIRS CACHE)
if(_ass_include_hint AND NOT IS_ABSOLUTE "${_ass_include_hint}")
  get_filename_component(_ass_include_hint
    "${_ass_include_hint}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()
if(_ass_include_hint
   AND EXISTS "${_ass_include_hint}/ass.h"
   AND NOT EXISTS "${_ass_include_hint}/ass/ass.h")
  get_filename_component(_ass_include_hint "${_ass_include_hint}" DIRECTORY)
endif()
if(_ass_include_hint)
  if(EXISTS "${_ass_include_hint}/ass/ass.h"
     AND EXISTS "${_ass_include_hint}/ass/ass_types.h")
    set(ass_INCLUDE_DIR "${_ass_include_hint}"
      CACHE PATH "Path to the include root containing ass/ass.h" FORCE)
  else()
    unset(ass_INCLUDE_DIR CACHE)
    unset(ass_INCLUDE_DIR)
  endif()
endif()

find_path(ass_INCLUDE_DIR
  NAMES ass/ass.h ass/ass_types.h
  HINTS
    "${_ass_include_hint}"
    ${PC_ass_INCLUDE_DIRS}
)
if(ass_INCLUDE_DIR
   AND (NOT EXISTS "${ass_INCLUDE_DIR}/ass/ass.h"
        OR NOT EXISTS "${ass_INCLUDE_DIR}/ass/ass_types.h"))
  unset(ass_INCLUDE_DIR CACHE)
  unset(ass_INCLUDE_DIR)
endif()
set(ass_INCLUDE_DIRS "${ass_INCLUDE_DIR}")

# An explicit relative runtime path is interpreted from the source root, just
# like the other external dependency paths accepted by this project.
if(ass_RUNTIME_LIBRARY AND NOT IS_ABSOLUTE "${ass_RUNTIME_LIBRARY}")
  get_filename_component(ass_RUNTIME_LIBRARY
    "${ass_RUNTIME_LIBRARY}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
  set(ass_RUNTIME_LIBRARY "${ass_RUNTIME_LIBRARY}"
    CACHE FILEPATH "Path to the libass runtime shared library" FORCE)
endif()
if(ass_RUNTIME_LIBRARY
   AND (NOT EXISTS "${ass_RUNTIME_LIBRARY}"
        OR IS_DIRECTORY "${ass_RUNTIME_LIBRARY}"))
  unset(ass_RUNTIME_LIBRARY CACHE)
  unset(ass_RUNTIME_LIBRARY)
endif()

if(WIN32)
  find_file(ass_RUNTIME_LIBRARY
    NAMES ass.dll
    HINTS
      "${CMAKE_SOURCE_DIR}/runtimes"
      ${PC_ass_LIBRARY_DIRS}
      ${PC_ass_LIBDIR}
  )
else()
  find_file(ass_RUNTIME_LIBRARY
    NAMES libass.so.9 libass.9.dylib libass.so libass.dylib
    HINTS
      ${PC_ass_LIBRARY_DIRS}
      ${PC_ass_LIBDIR}
  )
endif()

set(ass_VERSION ${PC_ass_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ass
  FOUND_VAR ass_FOUND
  REQUIRED_VARS
    ass_INCLUDE_DIR
  VERSION_VAR ass_VERSION
)

if(ass_FOUND AND NOT TARGET ass::headers)
  add_library(ass::headers INTERFACE IMPORTED)
  set_target_properties(ass::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ass_INCLUDE_DIR}"
  )
endif()

# Clear stale values left by older build trees. There is intentionally no
# ass::ass imported target: production targets must never link libass.
unset(ass_LIBRARIES CACHE)
unset(ass_LIBRARIES)

mark_as_advanced(ass_INCLUDE_DIR ass_RUNTIME_LIBRARY)
unset(_ass_include_hint)
