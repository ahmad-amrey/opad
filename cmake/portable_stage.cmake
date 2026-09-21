# Script side of the opad-portable target (cmake/portable.cmake passes the -D values; lists arrive '|'-separated).

foreach(_v OPAD_EXES OPAD_STAGE OPAD_WINDEPLOYQT OPAD_DLL_DIRS OPAD_SOURCE_DIR)
  if(NOT ${_v})
    message(FATAL_ERROR "portable_stage.cmake: ${_v} is not set")
  endif()
endforeach()
string(REPLACE "|" ";" OPAD_EXES "${OPAD_EXES}")
string(REPLACE "|" ";" OPAD_DLL_DIRS "${OPAD_DLL_DIRS}")

file(REMOVE_RECURSE "${OPAD_STAGE}")
file(MAKE_DIRECTORY "${OPAD_STAGE}")

set(_staged_exes "")
foreach(_exe IN LISTS OPAD_EXES)
  file(COPY "${_exe}" DESTINATION "${OPAD_STAGE}")
  get_filename_component(_name "${_exe}" NAME)
  list(APPEND _staged_exes "${OPAD_STAGE}/${_name}")
endforeach()
list(GET _staged_exes 0 _app)  # opad.exe, the only Qt one

# Qt: its DLLs and the plugins the app can load (platforms/qwindows, styles, imageformats). The touch/network plugin
# types are skipped: nothing uses them and they drag Qt6Network and its dependencies in.
execute_process(
  COMMAND "${OPAD_WINDEPLOYQT}" --release --no-translations --no-system-d3d-compiler --no-opengl-sw
          --no-compiler-runtime --skip-plugin-types generic,networkinformation,tls "${_app}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "windeployqt failed (${_rc}):\n${_out}")
endif()

# Everything else (OCCT toolkits, freetype, tbb, the compiler runtime, what the Qt plugins pull in): walk the import
# tables. Windows' own DLLs are left out; whatever is not found in the toolchain is reported, not ignored.
file(GLOB_RECURSE _plugins "${OPAD_STAGE}/*.dll")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
if(OPAD_OBJDUMP)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
  set(CMAKE_OBJDUMP "${OPAD_OBJDUMP}")
endif()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${_staged_exes}
  LIBRARIES ${_plugins}
  DIRECTORIES ${OPAD_DLL_DIRS}
  RESOLVED_DEPENDENCIES_VAR _resolved
  UNRESOLVED_DEPENDENCIES_VAR _unresolved
  CONFLICTING_DEPENDENCIES_PREFIX _conflict
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "^hvsifiletrust" "^pdmutilities" "^wpaxholder"
  POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")
foreach(_name IN LISTS _conflict_FILENAMES)  # same DLL in two search dirs: take the first (the toolchain's)
  list(GET _conflict_${_name} 0 _first)
  list(APPEND _resolved "${_first}")
endforeach()
set(_copied 0)
foreach(_dll IN LISTS _resolved)
  string(FIND "${_dll}" "${OPAD_STAGE}/" _inside)
  if(_inside EQUAL 0)
    continue()
  endif()
  file(COPY "${_dll}" DESTINATION "${OPAD_STAGE}")
  math(EXPR _copied "${_copied} + 1")
endforeach()
if(_unresolved)
  list(JOIN _unresolved ", " _missing)
  message(FATAL_ERROR "portable package: DLLs not found in ${OPAD_DLL_DIRS}: ${_missing}")
endif()

file(WRITE "${OPAD_STAGE}/opad.portable"
  "Portable install: OPAD keeps its settings and cache in the data folder beside this file.\n"
  "Delete this file to use the registry and %LOCALAPPDATA% instead.\n")
file(COPY "${OPAD_SOURCE_DIR}/LICENSE" DESTINATION "${OPAD_STAGE}")

get_filename_component(_parent "${OPAD_STAGE}" DIRECTORY)
get_filename_component(_folder "${OPAD_STAGE}" NAME)
file(REMOVE "${OPAD_STAGE}.zip")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar cf "${OPAD_STAGE}.zip" --format=zip "${_folder}"
  WORKING_DIRECTORY "${_parent}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "zipping ${OPAD_STAGE} failed (${_rc})")
endif()
message(STATUS "Portable package: ${OPAD_STAGE} (+ .zip), ${_copied} DLLs besides Qt's")
