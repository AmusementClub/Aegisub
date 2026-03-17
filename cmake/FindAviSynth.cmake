find_package(PkgConfig QUIET)
pkg_check_modules(PC_AviSynth QUIET avisynth)
if(DEFINED AviSynth_INCLUDE_DIR AND NOT IS_ABSOLUTE "${AviSynth_INCLUDE_DIR}")
  get_filename_component(AviSynth_INCLUDE_DIR "${AviSynth_INCLUDE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()
if(AviSynth_INCLUDE_DIR AND NOT EXISTS "${AviSynth_INCLUDE_DIR}/avisynth.h")
  unset(AviSynth_INCLUDE_DIR CACHE)
  unset(AviSynth_INCLUDE_DIR)
endif()
find_path(AviSynth_INCLUDE_DIR
  NAMES avisynth.h
  PATHS "C:/Program Files/AviSynth+/FilterSDK/include" "C:/Program Files (x86)/AviSynth+/FilterSDK/include"
  PATH_SUFFIXES avisynth
  HINTS ${AviSynth_INCLUDE_DIR}
  HINTS ${PC_AviSynth_INCLUDE_DIRS}
)
set(AviSynth_VERSION ${PC_AviSynth_VERSION})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AviSynth
  FOUND_VAR AviSynth_FOUND
  REQUIRED_VARS
    AviSynth_INCLUDE_DIR
  VERSION_VAR AviSynth_VERSION
)
