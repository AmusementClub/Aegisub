find_package(PkgConfig QUIET)
pkg_check_modules(PC_uchardet QUIET uchardet)
find_path(uchardet_INCLUDE_DIRS
  NAMES uchardet/uchardet.h
  HINTS ${PC_uchardet_INCLUDE_DIRS}
)
find_library(uchardet_LIBRARIES
  NAMES uchardet
  PATH_SUFFIXES build/src
  HINTS ${PC_uchardet_LIBRARY_DIRS}
)
set(uchardet_VERSION ${PC_uchardet_VERSION})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(uchardet
  FOUND_VAR uchardet_FOUND
  REQUIRED_VARS
    uchardet_LIBRARIES
    uchardet_INCLUDE_DIRS
  VERSION_VAR uchardet_VERSION
)

if(uchardet_FOUND AND NOT TARGET uchardet::libuchardet)
  add_library(uchardet::libuchardet UNKNOWN IMPORTED)
  set_target_properties(uchardet::libuchardet PROPERTIES
    IMPORTED_LOCATION "${uchardet_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${uchardet_INCLUDE_DIRS}"
  )
endif()
