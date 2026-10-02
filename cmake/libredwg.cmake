# DWG: LibreDWG's dwg2dxf and dxf2dwg, built from the third_party/libredwg submodule and placed beside opad.exe.
#
# LibreDWG is GPLv3. OPAD links none of it: core/src/drawing_io.cpp (convert_dwg) runs these as separate programs, and
# the portable package ships them with LibreDWG's licence and the source reference (cmake/portable_stage.cmake).
# Built once by its own CMake build (~1.5 min) into build/<preset>/libredwg; static, so the programs need no DLLs.
# Off with -DOPAD_DWG=OFF, and skipped when the submodule is not checked out (git submodule update --init).

option(OPAD_DWG "Build LibreDWG's dwg2dxf/dxf2dwg beside OPAD (third_party/libredwg)" ON)
set(_ldwg_src "${PROJECT_SOURCE_DIR}/third_party/libredwg")
if(NOT OPAD_DWG)
  return()
endif()
if(NOT EXISTS "${_ldwg_src}/CMakeLists.txt")
  message(STATUS "OPAD: third_party/libredwg is not checked out (git submodule update --init third_party/libredwg); "
                 "reading DWG then needs dwg2dxf on PATH or the ODA File Converter")
  return()
endif()

include(ExternalProject)
set(_ldwg_bin "${CMAKE_BINARY_DIR}/libredwg")
set(_ldwg_programs dwg2dxf dxf2dwg)
set(_ldwg_built "")
set(_ldwg_staged "")
foreach(_p IN LISTS _ldwg_programs)
  list(APPEND _ldwg_built "${_ldwg_bin}/${_p}${CMAKE_EXECUTABLE_SUFFIX}")
  list(APPEND _ldwg_staged "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_p}${CMAKE_EXECUTABLE_SUFFIX}")
endforeach()

set(_ldwg_args
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DBUILD_SHARED_LIBS=OFF
  -DLIBREDWG_DISABLE_JSON=ON  # its JSON reader needs the nested jsmn submodule; OPAD only converts DXF
  -DDISABLE_WERROR=ON
  -DENABLE_LTO=OFF
  -DBUILD_TESTING=OFF
  -DCMAKE_C_FLAGS=-w)          # its warnings are not ours to fix
if(MINGW)
  list(APPEND _ldwg_args "-DCMAKE_EXE_LINKER_FLAGS=-static -s")
endif()

ExternalProject_Add(libredwg
  SOURCE_DIR "${_ldwg_src}"
  BINARY_DIR "${_ldwg_bin}"
  CMAKE_ARGS ${_ldwg_args}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target ${_ldwg_programs}
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${_ldwg_built}
  LOG_CONFIGURE ON
  LOG_BUILD ON
  LOG_OUTPUT_ON_FAILURE ON)
# A CRLF checkout (Git for Windows' core.autocrlf) puts a carriage return into its generated config.h; see the script.
ExternalProject_Add_Step(libredwg config_h
  COMMAND ${CMAKE_COMMAND} "-DCONFIG=${_ldwg_bin}/src/config.h" "-DSOURCE=${_ldwg_src}" -P "${PROJECT_SOURCE_DIR}/cmake/libredwg_config.cmake"
  DEPENDEES configure
  DEPENDERS build)
# Reconfigure (and so rebuild) when the submodule moves to another release.
ExternalProject_Add_StepDependencies(libredwg configure
  "${_ldwg_src}/CMakeLists.txt" "${_ldwg_src}/configure.ac" "${_ldwg_src}/src/cmakeconfig.h.in")

add_custom_command(OUTPUT ${_ldwg_staged}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ldwg_built} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  DEPENDS ${_ldwg_built} libredwg
  COMMENT "LibreDWG's DWG converters beside OPAD"
  VERBATIM)
add_custom_target(opad-dwg ALL DEPENDS ${_ldwg_staged})
set(OPAD_DWG_PROGRAMS ${_ldwg_staged})
