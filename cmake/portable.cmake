# Portable Windows package: `cmake --build --preset windows-portable` (target opad-portable).
#
# Stages opad.exe (and opad-cli.exe) with everything they load into build/<preset>/portable/OPAD-<version>-windows-x64
# and zips that folder. The folder runs from anywhere with no MSYS2, Qt or OCCT installed; the `opad.portable`
# marker makes the app keep its settings and cache in <folder>/data (see app/main.cpp).
# It refuses to stage the GPL FFmpeg, codec and FreeImage DLLs MSYS2's OCCT drags in (see portable_stage.cmake), so
# with MSYS2's own OCCT it fails unless OPAD_ALLOW_GPL_DLLS=ON (a package that must never leave the machine).
# Included from app/CMakeLists.txt, after the opad target exists.

option(OPAD_ALLOW_GPL_DLLS "opad-portable: stage FFmpeg, codec, FreeImage and OpenVR DLLs anyway (local use only, never distribute)" OFF)

get_target_property(_opad_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_opad_qt_bin "${_opad_qmake}" DIRECTORY)
find_program(OPAD_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${_opad_qt_bin}")
if(NOT OPAD_WINDEPLOYQT)
  message(STATUS "OPAD: windeployqt not found; the opad-portable target is not available")
  return()
endif()

get_filename_component(_opad_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_opad_portable_exes "$<TARGET_FILE:opad>")
set(_opad_portable_deps opad)
if(TARGET opad-cli)
  list(APPEND _opad_portable_exes "$<TARGET_FILE:opad-cli>")
  list(APPEND _opad_portable_deps opad-cli)
endif()
if(TARGET opad-thumbnails)  # Explorer thumbnails, registered by Settings > File types
  list(APPEND _opad_portable_exes "$<TARGET_FILE:opad-thumbnails>")
  list(APPEND _opad_portable_deps opad-thumbnails)
endif()
string(REPLACE ";" "|" _opad_portable_exes "${_opad_portable_exes}")  # a list cannot cross -D as is
# LibreDWG's converters (cmake/libredwg.cmake), shipped as separate programs with their licence.
set(_opad_portable_dwg "")
if(TARGET opad-dwg)
  string(REPLACE ";" "|" _opad_portable_dwg "${OPAD_DWG_PROGRAMS}")
  list(APPEND _opad_portable_deps opad-dwg)
endif()
# Third-party notices: where windeployqt takes the Qt plugins from (to find their package), headers compiled in.
execute_process(COMMAND "${_opad_qmake}" -query QT_INSTALL_PLUGINS OUTPUT_VARIABLE _opad_qt_plugins OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(_opad_portable_headers "")
get_target_property(_opad_json_dirs nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir IN LISTS _opad_json_dirs)
  if(EXISTS "${_dir}/nlohmann/json.hpp")
    set(_opad_portable_headers "${_dir}/nlohmann/json.hpp")
  endif()
endforeach()

add_custom_target(opad-portable
  COMMAND ${CMAKE_COMMAND}
    "-DOPAD_EXES=${_opad_portable_exes}"
    "-DOPAD_STAGE=${CMAKE_BINARY_DIR}/portable/OPAD-${PROJECT_VERSION}-windows-x64"
    "-DOPAD_WINDEPLOYQT=${OPAD_WINDEPLOYQT}"
    "-DOPAD_DLL_DIRS=${_opad_cxx_bin}|${_opad_qt_bin}|${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    "-DOPAD_OBJDUMP=${CMAKE_OBJDUMP}"
    "-DOPAD_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DOPAD_DWG_PROGRAMS=${_opad_portable_dwg}"
    "-DOPAD_ALLOW_GPL_DLLS=${OPAD_ALLOW_GPL_DLLS}"
    "-DOPAD_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DOPAD_VERSION=${PROJECT_VERSION}"
    "-DOPAD_PACMAN=${OPAD_PACMAN}"
    "-DOPAD_QT_PLUGINS=${_opad_qt_plugins}"
    "-DOPAD_NOTICES_HEADERS=${_opad_portable_headers}"
    -P "${PROJECT_SOURCE_DIR}/cmake/portable_stage.cmake"
  DEPENDS ${_opad_portable_deps}
  COMMENT "Staging the portable package"
  VERBATIM)
