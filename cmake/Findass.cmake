find_package(PkgConfig QUIET)
pkg_check_modules(PC_ass QUIET libass)

find_path(ass_INCLUDE_DIRS
  NAMES ass/ass.h ass/ass_types.h
  PATH_SUFFIXES libass
  HINTS ${PC_ass_INCLUDE_DIRS}
)

find_library(ass_LIBRARIES
  NAMES ass
  HINTS ${PC_ass_LIBRARY_DIRS}
)

# Optional runtime DLL/shared library for app-local staging (Windows portable, dev output).
if(WIN32)
  find_file(ass_RUNTIME_LIBRARY
    NAMES ass.dll
    HINTS
      ${PC_ass_LIBRARY_DIRS}
      ${PC_ass_LIBDIR}
    PATH_SUFFIXES bin
  )
  if(NOT ass_RUNTIME_LIBRARY AND ass_LIBRARIES)
    get_filename_component(_ass_lib_dir "${ass_LIBRARIES}" DIRECTORY)
    get_filename_component(_ass_root "${_ass_lib_dir}" DIRECTORY)
    find_file(ass_RUNTIME_LIBRARY
      NAMES ass.dll
      HINTS
        "${_ass_lib_dir}"
        "${_ass_root}/bin"
        "${_ass_root}/lib"
      NO_DEFAULT_PATH
    )
    unset(_ass_lib_dir)
    unset(_ass_root)
  endif()
else()
  # Prefer SONAME for packaging hints when available.
  find_file(ass_RUNTIME_LIBRARY
    NAMES libass.so.9 libass.9.dylib libass.so libass.dylib
    HINTS
      ${PC_ass_LIBRARY_DIRS}
      ${PC_ass_LIBDIR}
  )
endif()

set(ass_VERSION ${PC_ass_VERSION})

include(FindPackageHandleStandardArgs)
# Headers are required for compiling against the public API types.
# Link libraries are optional for the main application (runtime loading).
find_package_handle_standard_args(ass
  FOUND_VAR ass_FOUND
  REQUIRED_VARS
    ass_INCLUDE_DIRS
  VERSION_VAR ass_VERSION
)

if(ass_FOUND AND NOT TARGET ass::headers)
  add_library(ass::headers INTERFACE IMPORTED)
  set_target_properties(ass::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ass_INCLUDE_DIRS}"
  )
endif()

if(ass_FOUND AND ass_LIBRARIES AND NOT TARGET ass::ass)
  add_library(ass::ass UNKNOWN IMPORTED)
  set_target_properties(ass::ass PROPERTIES
    IMPORTED_LOCATION "${ass_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${ass_INCLUDE_DIRS}"
  )
endif()

mark_as_advanced(ass_INCLUDE_DIRS ass_LIBRARIES ass_RUNTIME_LIBRARY)
