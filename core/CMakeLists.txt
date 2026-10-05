configure_file(include/opad/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/opad/version.hpp @ONLY)

# The agent guide is compiled in (TODO 10 B2): a raw string generated from core/res/agent_guide.md, rewritten only when
# the text changed, so editing the guide re-runs the configure step and rebuilds one file.
set(OPAD_AGENT_GUIDE ${CMAKE_CURRENT_SOURCE_DIR}/res/agent_guide.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OPAD_AGENT_GUIDE})
file(READ ${OPAD_AGENT_GUIDE} opad_guide_text)
string(REPLACE "\r" "" opad_guide_text "${opad_guide_text}")
string(FIND "${opad_guide_text}" ")opad_guide" opad_guide_clash)
if(NOT opad_guide_clash EQUAL -1)
  message(FATAL_ERROR "agent_guide.md must not contain the raw-string delimiter )opad_guide")
endif()
set(opad_guide_cpp "// Generated from core/res/agent_guide.md; edit that file.\n#include \"opad/agent.hpp\"\n\nnamespace opad::agent {\nconst std::string& guide() {\n  static const std::string text = R\"opad_guide(${opad_guide_text})opad_guide\";\n  return text;\n}\n}  // namespace opad::agent\n")
set(opad_guide_out ${CMAKE_CURRENT_BINARY_DIR}/agent_guide.cpp)
set(opad_guide_old "")
if(EXISTS ${opad_guide_out})
  file(READ ${opad_guide_out} opad_guide_old)
endif()
if(NOT "${opad_guide_old}" STREQUAL "${opad_guide_cpp}")
  file(WRITE ${opad_guide_out} "${opad_guide_cpp}")
endif()

# Third-party notices compiled in (TODO 11 UI-13): written at build time by cmake/notices.cmake from the link lines of
# opad and opad-cli in build.ninja (rewritten only when they change), for Help > Third-party licences and `opad-cli
# licenses`; the same text is <build>/THIRD-PARTY-NOTICES.txt, which the single-file package ships beside the exe.
# MSYS2's pacman lies in <root>/usr/bin beside its runtime (msys-2.0.dll), the toolchain in <root>/mingw64/bin: an Arch
# Linux /usr/bin/pacman beside /usr/bin/g++ is not it (Linux packages are looked up through dpkg).
get_filename_component(_opad_msys "${CMAKE_CXX_COMPILER}" DIRECTORY)
if(MINGW AND EXISTS "${_opad_msys}/../../usr/bin/msys-2.0.dll")
  find_program(OPAD_PACMAN pacman HINTS "${_opad_msys}/../../usr/bin" NO_DEFAULT_PATH)
endif()
set(opad_notices_cpp ${CMAKE_CURRENT_BINARY_DIR}/third_party_notices.cpp)
set(opad_notices_stamp ${CMAKE_CURRENT_BINARY_DIR}/third_party_notices.stamp)
set(_opad_notices_files "")
get_target_property(_opad_json_dirs nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir IN LISTS _opad_json_dirs)
  if(EXISTS "${_dir}/nlohmann/json.hpp")
    list(APPEND _opad_notices_files "${_dir}/nlohmann/json.hpp")  # header-only: compiled into OPAD's own code
  endif()
endforeach()
set(_opad_notices_occt "")
set(_opad_notices_targets programs)
set(_opad_notices_scope "This list covers the libraries opad and opad-cli are linked against in this build (the portable package lists every DLL it ships instead).")
if(OPAD_STATIC)
  if(EXISTS "${OPAD_STATIC_PREFIX}/include/rapidjson/rapidjson.h")  # header-only, compiled into the static OCCT
    list(APPEND _opad_notices_files "${OPAD_STATIC_PREFIX}/include/rapidjson/rapidjson.h")
  endif()
  set(_opad_notices_occt "${_occt_install}|${OPAD_OCCT_VERSION}|${OPAD_OCCT_URL}|${_occt_src}")
  set(_opad_notices_scope "The single-file programs contain the libraries below, linked in statically.")
elseif(NOT OPAD_BUILD_APP AND NOT OPAD_BUILD_CLI)  # the Python wheel (python/pyproject.toml)
  set(_opad_notices_scope "This list covers the libraries the opad Python module is linked against. A wheel repaired with delvewheel, auditwheel or delocate also carries copies of the shared libraries among them (Open CASCADE and what it loads).")
  set(_opad_notices_targets python)
endif()
string(REPLACE ";" "|" _opad_notices_files "${_opad_notices_files}")
string(REPLACE ";" "|" _opad_notices_libdirs "${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}")
string(REPLACE ";" "|" _opad_notices_libs "${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES}")
set(_opad_notices_ninja "")
if(CMAKE_GENERATOR STREQUAL "Ninja")
  set(_opad_notices_ninja ${CMAKE_BINARY_DIR}/build.ninja)
endif()
add_custom_command(OUTPUT ${opad_notices_stamp}
  BYPRODUCTS ${opad_notices_cpp} ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt
  COMMAND ${CMAKE_COMMAND} "-DNOTICES_OUT=${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" "-DNOTICES_CPP=${opad_notices_cpp}"
          "-DNOTICES_NINJA=${_opad_notices_ninja}" "-DNOTICES_TARGETS=${_opad_notices_targets}"
          "-DNOTICES_LIBDIRS=${_opad_notices_libdirs}" "-DNOTICES_LIBS=${_opad_notices_libs}" "-DNOTICES_STATIC=${OPAD_STATIC}" "-DNOTICES_OWN=${CMAKE_BINARY_DIR}" "-DNOTICES_OCCT=${_opad_notices_occt}"
          "-DNOTICES_FILES=${_opad_notices_files}" "-DNOTICES_PACMAN=${OPAD_PACMAN}" "-DNOTICES_VERSION=${PROJECT_VERSION}"
          "-DNOTICES_SCOPE=${_opad_notices_scope}" "-DNOTICES_EXTRA=${PROJECT_SOURCE_DIR}/cmake/notices_extra.txt"
          -P ${PROJECT_SOURCE_DIR}/cmake/notices.cmake
  COMMAND ${CMAKE_COMMAND} -E touch ${opad_notices_stamp}
  DEPENDS ${PROJECT_SOURCE_DIR}/cmake/notices.cmake ${PROJECT_SOURCE_DIR}/cmake/notices_extra.txt ${_opad_notices_ninja}
  COMMENT "Third-party notices from the link libraries"
  VERBATIM)

# Every src/**/*.cpp is part of the core (CONFIGURE_DEPENDS re-globs at each build): a new file or folder needs no edit
# here. Anything to leave out goes into list(REMOVE_ITEM) here.
file(GLOB_RECURSE opad_core_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(SORT opad_core_sources)
add_library(opad_core STATIC ${opad_core_sources} ${opad_notices_cpp} ${opad_notices_stamp} ${CMAKE_CURRENT_BINARY_DIR}/agent_guide.cpp)
add_library(opad::core ALIAS opad_core)

target_include_directories(opad_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/plugins/include>
    ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(opad_core
  PUBLIC ${OPAD_OCCT_LIBS} nlohmann_json::nlohmann_json
  PRIVATE ${CMAKE_DL_LIBS} ZLIB::ZLIB)
target_compile_definitions(opad_core PUBLIC _USE_MATH_DEFINES)
if(TARGET TKV3d AND TARGET TKService)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_FONT=1)
  # Drawing text is shaped with HarfBuzz (Arabic and other complex scripts, src/drawing_text.cpp; glyph outlines through
  # its draw API, 7.0 or later); without it OCCT's unshaped text is drawn. The static build's archive comes from the
  # FreeType group at the end of the link line (occt_static.cmake), which holds HarfBuzz and its dependencies.
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(OPAD_HB QUIET IMPORTED_TARGET harfbuzz>=7.0)
  endif()
  if(OPAD_HB_FOUND)
    target_link_libraries(opad_core PRIVATE PkgConfig::OPAD_HB)
    target_compile_definitions(opad_core PRIVATE OPAD_HAVE_SHAPING=1)
  else()
    message(STATUS "HarfBuzz 7 not found: drawing text is drawn unshaped")
  endif()
endif()
if(OPAD_HAVE_GLTF)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_GLTF=1)
endif()
if(TARGET zstd::libzstd_static AND OPAD_STATIC)
  target_link_libraries(opad_core PRIVATE zstd::libzstd_static)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_ZSTD=1)
elseif(TARGET zstd::libzstd_shared)
  target_link_libraries(opad_core PRIVATE zstd::libzstd_shared)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_ZSTD=1)
endif()
if(NOT MSVC)
  # Keep the software rasteriser bit-identical across compilers (N4): no fused multiply-add contraction.
  set_source_files_properties(src/render.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
