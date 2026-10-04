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
# The drawing fonts are compiled into the programs (paint/CMakeLists.txt); their licences travel with the package.
file(MAKE_DIRECTORY "${OPAD_STAGE}/licenses")
foreach(_font_licence LiberationSans-LICENSE.txt NotoSansArabic-OFL.txt)
  configure_file("${OPAD_SOURCE_DIR}/third_party/fonts/${_font_licence}" "${OPAD_STAGE}/licenses/${_font_licence}" COPYONLY)
endforeach()
file(WRITE "${OPAD_STAGE}/licenses/Fonts-NOTICE.txt"
  "Technical drawings are lettered in Liberation Sans 2.1.5 (Red Hat; Arimo by Google) and Noto Sans Arabic\n"
  "(The Noto Project Authors), compiled into opad.exe and opad-cli.exe unmodified. Both are licensed under the\n"
  "SIL Open Font License 1.1 (LiberationSans-LICENSE.txt, NotoSansArabic-OFL.txt). Title-block templates and drawing\n"
  "symbols are OPAD's own, drawn from general drafting practice.\n")

# DWG: LibreDWG's dwg2dxf / dxf2dwg (GPLv3) are separate programs OPAD runs; they travel with their licence and where
# their source is.
if(OPAD_DWG_PROGRAMS)
  string(REPLACE "|" ";" OPAD_DWG_PROGRAMS "${OPAD_DWG_PROGRAMS}")
  file(COPY ${OPAD_DWG_PROGRAMS} DESTINATION "${OPAD_STAGE}")
  set(_ldwg "${OPAD_SOURCE_DIR}/third_party/libredwg")
  file(MAKE_DIRECTORY "${OPAD_STAGE}/licenses")
  configure_file("${_ldwg}/COPYING" "${OPAD_STAGE}/licenses/LibreDWG-COPYING.txt" COPYONLY)
  execute_process(COMMAND git -C "${_ldwg}" describe --tags --always OUTPUT_VARIABLE _ldwg_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  file(WRITE "${OPAD_STAGE}/licenses/LibreDWG-NOTICE.txt"
    "dwg2dxf.exe and dxf2dwg.exe are LibreDWG ${_ldwg_version}, unmodified, built by OPAD's build from\n"
    "https://github.com/LibreDWG/libredwg (tag ${_ldwg_version}; third_party/libredwg in OPAD's source tree).\n"
    "LibreDWG is free software under the GNU General Public License version 3 (LibreDWG-COPYING.txt).\n"
    "OPAD runs them as separate programs to read and write DWG drawings; it does not link to LibreDWG.\n")
endif()

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
