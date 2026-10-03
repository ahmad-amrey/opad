# Run after LibreDWG's configure step (cmake/libredwg.cmake) on the config.h it generated.
#
# Its CMakeLists reads the library version out of configure.ac with perl. Git for Windows checks text files out with
# CRLF (core.autocrlf), so that string ends in a carriage return, which GCC takes for a line end inside a string
# literal. It also asks `git describe` from the build folder, which names OPAD's commit rather than LibreDWG's release.

if(NOT EXISTS "${CONFIG}")
  message(FATAL_ERROR "libredwg_config.cmake: ${CONFIG} not found")
endif()
file(READ "${CONFIG}" _text)
set(_before "${_text}")
string(REPLACE "\r" "" _text "${_text}")
find_package(Git QUIET)
if(GIT_EXECUTABLE)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE}" describe --tags --always
                  OUTPUT_VARIABLE _version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
  if(_rc EQUAL 0 AND _version)
    string(REGEX REPLACE "#define PACKAGE_VERSION \"[^\"]*\"" "#define PACKAGE_VERSION \"${_version}\"" _text "${_text}")
    string(REGEX REPLACE "#define PACKAGE_STRING \"[^\"]*\"" "#define PACKAGE_STRING \"LibreDWG ${_version}\"" _text "${_text}")
  endif()
endif()
if(NOT _text STREQUAL _before)
  file(WRITE "${CONFIG}" "${_text}")
endif()
