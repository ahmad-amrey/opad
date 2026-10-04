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

# GPL guard (TODO 11 UI-14, decision D1): MSYS2's Open CASCADE drags GPL FFmpeg, codecs, FreeImage and OpenVR in.
include("${OPAD_SOURCE_DIR}/cmake/gpl_guard.cmake")
file(GLOB_RECURSE _staged_dlls "${OPAD_STAGE}/*.dll")
opad_gpl_guard("portable package" "${OPAD_STAGE}" ${_resolved} ${_staged_dlls})

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

# Third-party notices (TODO 11 UI-13): each staged DLL traced back to where it was copied from, then to its MSYS2
# package; THIRD-PARTY-NOTICES.txt lists them with version, licence and source, licenses/<package>/ holds their texts.
file(GLOB_RECURSE _staged_dlls RELATIVE "${OPAD_STAGE}" "${OPAD_STAGE}/*.dll")
string(REPLACE "|" ";" _notices_files "${OPAD_NOTICES_HEADERS}")
foreach(_rel IN LISTS _staged_dlls)
  foreach(_dir IN LISTS OPAD_DLL_DIRS OPAD_QT_PLUGINS)
    if(EXISTS "${_dir}/${_rel}")
      list(APPEND _notices_files "${_dir}/${_rel}")
      break()
    endif()
  endforeach()
endforeach()
string(REPLACE ";" "|" NOTICES_FILES "${_notices_files}")
set(NOTICES_OUT "${OPAD_STAGE}/THIRD-PARTY-NOTICES.txt")
set(NOTICES_LICENSES_DIR "${OPAD_STAGE}/licenses")
set(NOTICES_OWN "${OPAD_BUILD_DIR}")
set(NOTICES_PACMAN "${OPAD_PACMAN}")
set(NOTICES_VERSION "${OPAD_VERSION}")
set(NOTICES_EXTRA "${OPAD_SOURCE_DIR}/cmake/notices_extra.txt")
string(CONCAT NOTICES_SCOPE "This folder ships the DLLs listed below beside opad.exe. The LGPL libraries among them (Qt, "
  "Open CASCADE and others) are separate DLLs that you may replace with compatible builds of your own.")
include("${OPAD_SOURCE_DIR}/cmake/notices.cmake")

# DWG: LibreDWG's dwg2dxf / dxf2dwg (GPLv3) are separate programs OPAD runs. They travel with their licence and their
# Corresponding Source (GPLv3 section 6): the source tree they were built from and OPAD's scripts that built them.
if(OPAD_DWG_PROGRAMS)
  string(REPLACE "|" ";" OPAD_DWG_PROGRAMS "${OPAD_DWG_PROGRAMS}")
  file(COPY ${OPAD_DWG_PROGRAMS} DESTINATION "${OPAD_STAGE}")
  set(_ldwg "${OPAD_SOURCE_DIR}/third_party/libredwg")
  set(_ldwg_out "${OPAD_STAGE}/licenses/LibreDWG")
  file(MAKE_DIRECTORY "${_ldwg_out}/build")
  configure_file("${_ldwg}/COPYING" "${_ldwg_out}/COPYING.txt" COPYONLY)
  execute_process(COMMAND git -C "${_ldwg}" describe --tags --always OUTPUT_VARIABLE _ldwg_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND git -C "${_ldwg}" rev-parse HEAD OUTPUT_VARIABLE _ldwg_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  # LF line ends as upstream; the sample drawings (test/test-data, 54 MB) are not needed to build and stay upstream.
  execute_process(COMMAND git -c core.autocrlf=false -C "${_ldwg}" archive --format=zip "--prefix=libredwg-${_ldwg_version}/"
                          -o "${_ldwg_out}/libredwg-${_ldwg_version}-source.zip" HEAD -- . ":(exclude)test/test-data"
                  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "portable package: no LibreDWG source archive (${_rc}): ${_err}\nThe converters cannot ship without their source.")
  endif()
  file(COPY "${OPAD_SOURCE_DIR}/cmake/libredwg.cmake" "${OPAD_SOURCE_DIR}/cmake/libredwg_config.cmake" DESTINATION "${_ldwg_out}/build")
  file(WRITE "${_ldwg_out}/NOTICE.txt"
    "dwg2dxf.exe and dxf2dwg.exe are LibreDWG ${_ldwg_version} (commit ${_ldwg_commit}), free software under the GNU\n"
    "General Public License version 3 or later (COPYING.txt). OPAD runs them as separate programs to read and write DWG\n"
    "drawings; it does not link to LibreDWG, and OPAD's own licence does not apply to them.\n\n"
    "Corresponding source, in this folder:\n"
    "  libredwg-${_ldwg_version}-source.zip  the LibreDWG tree they were built from (git archive of that commit; the\n"
    "      sample drawings in test/test-data are not needed to build and are left out, they are in the upstream tag)\n"
    "  build/libredwg.cmake         how OPAD's build configures and builds them: LibreDWG's own CMake build with the\n"
    "      options listed there (_ldwg_args: static, JSON reader off, no tests), MinGW-w64 GCC, targets dwg2dxf dxf2dwg,\n"
    "      linked with -static -s\n"
    "  build/libredwg_config.cmake  the one step between configure and build: in the generated src/config.h it drops the\n"
    "      carriage returns a CRLF checkout leaves there and sets PACKAGE_VERSION / PACKAGE_STRING to `git describe` of\n"
    "      the LibreDWG checkout. No LibreDWG source file is changed.\n\n"
    "To rebuild: unpack the zip, then\n"
    "  cmake -S libredwg-${_ldwg_version} -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF\n"
    "        -DLIBREDWG_DISABLE_JSON=ON -DDISABLE_WERROR=ON -DENABLE_LTO=OFF -DBUILD_TESTING=OFF\n"
    "        \"-DCMAKE_EXE_LINKER_FLAGS=-static -s\"\n"
    "  cmake --build build --target dwg2dxf dxf2dwg\n"
    "(the zip has LF line ends, so config.h needs no fixing; libredwg_config.cmake only sets the version string)\n\n"
    "Upstream: https://github.com/LibreDWG/libredwg (tag ${_ldwg_version}), https://www.gnu.org/software/libredwg/\n")
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
