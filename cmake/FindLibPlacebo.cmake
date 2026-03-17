find_package(PkgConfig QUIET)
pkg_check_modules(PC_LibPlacebo QUIET libplacebo)

if(DEFINED LibPlacebo_INCLUDE_DIR AND NOT IS_ABSOLUTE "${LibPlacebo_INCLUDE_DIR}")
  get_filename_component(LibPlacebo_INCLUDE_DIR "${LibPlacebo_INCLUDE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()
if(LibPlacebo_INCLUDE_DIR AND NOT EXISTS "${LibPlacebo_INCLUDE_DIR}/libplacebo/config.h")
  unset(LibPlacebo_INCLUDE_DIR CACHE)
  unset(LibPlacebo_INCLUDE_DIR)
endif()

find_path(LibPlacebo_INCLUDE_DIR
  NAMES libplacebo/config.h
  HINTS ${LibPlacebo_INCLUDE_DIR}
  HINTS ${PC_LibPlacebo_INCLUDE_DIRS}
)

set(LibPlacebo_VERSION ${PC_LibPlacebo_VERSION})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibPlacebo
  FOUND_VAR LibPlacebo_FOUND
  REQUIRED_VARS
    LibPlacebo_INCLUDE_DIR
  VERSION_VAR LibPlacebo_VERSION
)
