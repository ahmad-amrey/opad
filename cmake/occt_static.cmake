# Static Open CASCADE for the single-file build (OPAD_STATIC=ON, preset `windows-static`).
#
# Distribution packages ship OCCT as shared libraries only, so the static preset builds the toolkits OPAD uses
# from the OCCT source tarball, once, into build/<preset>/occt (cmake/occt_source.cmake: the same recipe as the
# bundled shared OCCT of Linux; delete build/<preset>/occt/install to rebuild). FreeType (fonts of the view cube and
# dimensions) and RapidJSON (glTF) come from the OS packages; a static toolkit carries no library of its own, so every
# executable links FreeType and its dependencies itself (see the end of this file).
#
# Included from the top-level CMakeLists.txt before find_package(OpenCASCADE); sets OpenCASCADE_DIR.

include(${CMAKE_CURRENT_LIST_DIR}/occt_source.cmake)

# The OS packages the static toolkits still need at build time (headers) and at link time (FreeType's archive).
if(NOT OPAD_STATIC_PREFIX)
  list(GET CMAKE_PREFIX_PATH -1 OPAD_STATIC_PREFIX)  # the last entry: the OS package root (the Qt one comes first)
endif()
if(NOT EXISTS "${OPAD_STATIC_PREFIX}/include/freetype2/ft2build.h")
  message(FATAL_ERROR "OPAD_STATIC: FreeType headers not found under ${OPAD_STATIC_PREFIX}/include/freetype2")
endif()
set(_occt_rapidjson "")
if(EXISTS "${OPAD_STATIC_PREFIX}/include/rapidjson/rapidjson.h")
  set(_occt_rapidjson -DUSE_RAPIDJSON=ON "-D3RDPARTY_RAPIDJSON_INCLUDE_DIR=${OPAD_STATIC_PREFIX}/include")
else()
  message(WARNING "OPAD_STATIC: RapidJSON headers not found under ${OPAD_STATIC_PREFIX}/include; glTF export is left out")
endif()

# GCC 16 (MSYS2 UCRT64) at -O3 builds an OCCT that faults in Extrema_ExtCC::Perform under every solid classification
# (BRepClass3d_SClassifier: booleans, distances, classifiers; seen in CI, GCC 15 and 13 are fine): -O2 and no strict
# aliasing there.
set(_occt_opt "")
set(_occt_flags "${OPAD_STATIC_CXX_FLAGS}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  set(_occt_opt "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG")
  string(APPEND _occt_flags " -fno-strict-aliasing")
endif()

opad_occt_from_source(OPAD_STATIC Static
  ${_occt_opt}
  "-DCMAKE_C_FLAGS=${_occt_flags}" "-DCMAKE_CXX_FLAGS=${_occt_flags}"  # per-function sections: the exe's --gc-sections drops what it does not call
  -DUSE_XLIB=OFF
  "-D3RDPARTY_FREETYPE_INCLUDE_DIR_ft2build=${OPAD_STATIC_PREFIX}/include/freetype2"
  "-D3RDPARTY_FREETYPE_INCLUDE_DIR_freetype2=${OPAD_STATIC_PREFIX}/include/freetype2"
  ${_occt_rapidjson})

# FreeType and everything its archive pulls in (HarfBuzz, libpng, Brotli, bzip2, zlib, glib ...): pkg-config knows.
# They go at the very end of every link line (CMAKE_CXX_STANDARD_LIBRARIES), as one group: OCCT and Qt both pull
# FreeType in, Qt's copy lands after the transitive closure, and FreeType and HarfBuzz reference each other.
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPAD_FT REQUIRED freetype2)
list(JOIN OPAD_FT_STATIC_LDFLAGS " " _ft_flags)
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -Wl,--start-group ${_ft_flags} -Wl,--end-group")
