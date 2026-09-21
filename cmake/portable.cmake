# Portable Windows package: `cmake --build --preset windows-portable` (target opad-portable).
#
# Stages opad.exe (and opad-cli.exe) with everything they load into build/<preset>/portable/OPAD-<version>-windows-x64
# and zips that folder. The folder runs from anywhere with no MSYS2, Qt or OCCT installed; the `opad.portable`
# marker makes the app keep its settings and cache in <folder>/data (see app/main.cpp).
# Included from app/CMakeLists.txt, after the opad target exists.

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
string(REPLACE ";" "|" _opad_portable_exes "${_opad_portable_exes}")  # a list cannot cross -D as is

add_custom_target(opad-portable
  COMMAND ${CMAKE_COMMAND}
    "-DOPAD_EXES=${_opad_portable_exes}"
    "-DOPAD_STAGE=${CMAKE_BINARY_DIR}/portable/OPAD-${PROJECT_VERSION}-windows-x64"
    "-DOPAD_WINDEPLOYQT=${OPAD_WINDEPLOYQT}"
    "-DOPAD_DLL_DIRS=${_opad_cxx_bin}|${_opad_qt_bin}|${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    "-DOPAD_OBJDUMP=${CMAKE_OBJDUMP}"
    "-DOPAD_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${PROJECT_SOURCE_DIR}/cmake/portable_stage.cmake"
  DEPENDS ${_opad_portable_deps}
  COMMENT "Staging the portable package"
  VERBATIM)
