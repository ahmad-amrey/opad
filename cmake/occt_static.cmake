# Static Open CASCADE for the single-file build (OPAD_STATIC=ON, preset `windows-static`).
#
# Distribution packages ship OCCT as shared libraries only, so the static preset builds the toolkits OPAD uses
# from the OCCT source tarball, once, into build/<preset>/occt (download -> extract -> configure -> build -> install;
# about 15 minutes on 16 cores, then cached: delete build/<preset>/occt/install to rebuild). Everything OPAD does
# not need is left out: Draw, DETools, Tcl/Tk, TBB, FFmpeg, OpenVR, FreeImage, VTK. FreeType (fonts of the view
# cube and dimensions) and RapidJSON (glTF) come from the OS packages; a static toolkit carries no library of its
# own, so every executable links FreeType and its dependencies itself (see the end of this file).
#
# Included from the top-level CMakeLists.txt before find_package(OpenCASCADE); sets OpenCASCADE_DIR.

set(OPAD_OCCT_VERSION 7.9.2)
string(REPLACE "." "_" _occt_tag "${OPAD_OCCT_VERSION}")
set(OPAD_OCCT_URL "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V${_occt_tag}.tar.gz")
set(OPAD_OCCT_SHA256 3cd080d3fc33ba0c6c157e110afe3e015859524c4694dbb09812ec9d61595639)

set(_occt_root "${CMAKE_BINARY_DIR}/occt")
set(_occt_tarball "${_occt_root}/V${_occt_tag}.tar.gz")
set(_occt_src "${_occt_root}/OCCT-${_occt_tag}")
set(_occt_build "${_occt_root}/build")
set(_occt_install "${_occt_root}/install")
set(_occt_config "${_occt_install}/lib/cmake/opencascade/OpenCASCADEConfig.cmake")

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

if(NOT EXISTS "${_occt_config}")
  file(MAKE_DIRECTORY "${_occt_root}")
  if(NOT EXISTS "${_occt_tarball}")
    message(STATUS "OPAD_STATIC: downloading OCCT ${OPAD_OCCT_VERSION} from ${OPAD_OCCT_URL}")
    file(DOWNLOAD "${OPAD_OCCT_URL}" "${_occt_tarball}" EXPECTED_HASH SHA256=${OPAD_OCCT_SHA256}
         SHOW_PROGRESS STATUS _dl)
    list(GET _dl 0 _rc)
    if(NOT _rc EQUAL 0)
      file(REMOVE "${_occt_tarball}")
      message(FATAL_ERROR "OPAD_STATIC: download failed: ${_dl}")
    endif()
  endif()
  if(NOT EXISTS "${_occt_src}/CMakeLists.txt")
    message(STATUS "OPAD_STATIC: extracting ${_occt_tarball}")
    file(ARCHIVE_EXTRACT INPUT "${_occt_tarball}" DESTINATION "${_occt_root}")
  endif()

  message(STATUS "OPAD_STATIC: building OCCT ${OPAD_OCCT_VERSION} static toolkits in ${_occt_build} (once; this takes a while)")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" -S "${_occt_src}" -B "${_occt_build}"
      "-DCMAKE_BUILD_TYPE=Release"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCMAKE_C_FLAGS=${OPAD_STATIC_CXX_FLAGS}" "-DCMAKE_CXX_FLAGS=${OPAD_STATIC_CXX_FLAGS}"  # per-function sections: the exe's --gc-sections drops what it does not call
      "-DCMAKE_INSTALL_PREFIX=${_occt_install}" -DINSTALL_DIR_LAYOUT=Unix
      -DBUILD_LIBRARY_TYPE=Static -DBUILD_USE_PCH=OFF
      -DBUILD_MODULE_FoundationClasses=ON -DBUILD_MODULE_ModelingData=ON -DBUILD_MODULE_ModelingAlgorithms=ON
      -DBUILD_MODULE_Visualization=ON -DBUILD_MODULE_ApplicationFramework=ON -DBUILD_MODULE_DataExchange=ON
      -DBUILD_MODULE_Draw=OFF -DBUILD_MODULE_DETools=OFF -DBUILD_DOC_Overview=OFF -DBUILD_SAMPLES_QT=OFF
      -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_TBB=OFF -DUSE_FFMPEG=OFF -DUSE_OPENVR=OFF -DUSE_FREEIMAGE=OFF -DUSE_DRACO=OFF
      -DUSE_VTK=OFF -DUSE_EIGEN=OFF -DUSE_OPENGL=ON -DUSE_GLES2=OFF -DUSE_D3D=OFF -DUSE_XLIB=OFF
      -DUSE_FREETYPE=ON
      "-D3RDPARTY_FREETYPE_INCLUDE_DIR_ft2build=${OPAD_STATIC_PREFIX}/include/freetype2"
      "-D3RDPARTY_FREETYPE_INCLUDE_DIR_freetype2=${OPAD_STATIC_PREFIX}/include/freetype2"
      ${_occt_rapidjson}
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "OPAD_STATIC: configuring OCCT failed (${_rc})")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_occt_build}" --target install
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR NOT EXISTS "${_occt_config}")
    message(FATAL_ERROR "OPAD_STATIC: building OCCT failed (${_rc})")
  endif()
endif()

set(OpenCASCADE_DIR "${_occt_install}/lib/cmake/opencascade" CACHE PATH "" FORCE)
message(STATUS "OPAD_STATIC: OCCT ${OPAD_OCCT_VERSION} static toolkits from ${_occt_install}")

# FreeType and everything its archive pulls in (HarfBuzz, libpng, Brotli, bzip2, zlib, glib ...): pkg-config knows.
# They go at the very end of every link line (CMAKE_CXX_STANDARD_LIBRARIES), as one group: OCCT and Qt both pull
# FreeType in, Qt's copy lands after the transitive closure, and FreeType and HarfBuzz reference each other.
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPAD_FT REQUIRED freetype2)
list(JOIN OPAD_FT_STATIC_LDFLAGS " " _ft_flags)
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -Wl,--start-group ${_ft_flags} -Wl,--end-group")
