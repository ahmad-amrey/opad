# Which Open CASCADE a shared (non-static) build uses: the system's, or OCCT 7.9.2 built from source into the build
# tree (cmake/occt_source.cmake) so that every platform runs the same kernel. OPAD needs OCCT 7.8 or newer.
#
#   OPAD_OCCT_BUILD=AUTO  (default) Linux: build it when the system OCCT is missing or older than 7.8 (Ubuntu 24.04 has
#                         7.6). Windows (MSYS2) and macOS (Homebrew) take the system OCCT as they always did.
#   OPAD_OCCT_BUILD=ON    build it whatever the system has (not on Windows: use MSYS2's OCCT or the windows-static preset).
#   OPAD_OCCT_BUILD=OFF   the system OCCT, which must be 7.8 or newer: configuring stops otherwise.
#
# The bundled OCCT is shared libraries in build/<preset>/occt/install/lib. Programs built here find them through their
# build RPATH, the libraries find each other through $ORIGIN, and linuxdeploy (opad-single) follows the same paths.
#
# Included from the top-level CMakeLists.txt before find_package(OpenCASCADE). Sets OPAD_OCCT_BUNDLED and, when ON,
# OpenCASCADE_DIR, OPAD_OCCT_INSTALL_DIR and OPAD_OCCT_SOURCE_DIR.

set(OPAD_OCCT_BUILD AUTO CACHE STRING "Build OCCT 7.9.2 from source into the build tree: AUTO (Linux, when the system OCCT is missing or older than 7.8), ON or OFF")
set_property(CACHE OPAD_OCCT_BUILD PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${OPAD_OCCT_BUILD}" _occt_mode)
if(NOT _occt_mode MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "OPAD_OCCT_BUILD must be AUTO, ON or OFF (is '${OPAD_OCCT_BUILD}')")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/occt_source.cmake)  # OPAD_OCCT_VERSION and the recipe
set(OPAD_OCCT_BUNDLED OFF)
set(_occt_bundled_dir "${CMAKE_BINARY_DIR}/occt/install/lib/cmake/opencascade")

# The version of the system OCCT find_package(OpenCASCADE) would load, read from its version file without loading its
# targets (they could not be unloaded again for the bundled ones). Not find_package(OpenCASCADE 7.8): OCCT's version
# file asks for an exact match, so 7.9 would not count as 7.8 or newer.
function(_opad_system_occt out_version out_dir)
  set(_dirs "")
  if(OpenCASCADE_DIR AND EXISTS "${OpenCASCADE_DIR}/OpenCASCADEConfigVersion.cmake")
    set(_dirs "${OpenCASCADE_DIR}")
  elseif(DEFINED ENV{OpenCASCADE_DIR} AND EXISTS "$ENV{OpenCASCADE_DIR}/OpenCASCADEConfigVersion.cmake")
    set(_dirs "$ENV{OpenCASCADE_DIR}")
  else()
    set(_prefixes ${CMAKE_PREFIX_PATH} $ENV{CMAKE_PREFIX_PATH} ${CMAKE_SYSTEM_PREFIX_PATH})
    find_path(_occt_version_dir OpenCASCADEConfigVersion.cmake
      PATHS ${_prefixes} NO_DEFAULT_PATH NO_CACHE
      PATH_SUFFIXES lib/cmake/opencascade lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake/opencascade lib64/cmake/opencascade
                    share/cmake/opencascade cmake/opencascade cmake)
    if(_occt_version_dir)
      set(_dirs "${_occt_version_dir}")
    endif()
  endif()
  set(${out_version} "" PARENT_SCOPE)
  set(${out_dir} "" PARENT_SCOPE)
  if(_dirs)
    file(STRINGS "${_dirs}/OpenCASCADEConfigVersion.cmake" _line REGEX "set\\(PACKAGE_VERSION \"[0-9.]+\"\\)")
    if(_line MATCHES "\"([0-9.]+)\"")
      set(${out_version} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      set(${out_dir} "${_dirs}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

if(OpenCASCADE_DIR STREQUAL _occt_bundled_dir)
  set(OPAD_OCCT_BUNDLED ON)  # chosen at an earlier configure: keep it (OPAD_OCCT_BUILD=OFF switches back)
  if(_occt_mode STREQUAL "OFF")
    unset(OpenCASCADE_DIR CACHE)
    set(OPAD_OCCT_BUNDLED OFF)
  endif()
endif()
if(NOT OPAD_OCCT_BUNDLED)
  if(_occt_mode STREQUAL "ON")
    set(OPAD_OCCT_BUNDLED ON)
  elseif(_occt_mode STREQUAL "OFF" OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    _opad_system_occt(_occt_version _occt_dir)
    if(_occt_version AND _occt_version VERSION_GREATER_EQUAL 7.8)
      message(STATUS "OPAD: system OCCT ${_occt_version} (${_occt_dir})")
      set(OpenCASCADE_DIR "${_occt_dir}" CACHE PATH "" FORCE)  # the one checked, not another one further on the path
    elseif(_occt_mode STREQUAL "OFF")
      if(_occt_version)
        set(_why "the system OCCT is ${_occt_version} (${_occt_dir})")
      else()
        set(_why "no system OCCT was found")
      endif()
      message(FATAL_ERROR "OPAD needs Open CASCADE 7.8 or newer and ${_why}. Install OCCT 7.8+ and point "
        "OpenCASCADE_DIR at its lib/cmake/opencascade folder, or configure with -DOPAD_OCCT_BUILD=AUTO (or ON) to build "
        "OCCT ${OPAD_OCCT_VERSION} from source into the build tree once.")
    else()
      if(_occt_version)
        message(STATUS "OPAD: system OCCT ${_occt_version} (${_occt_dir}) is older than 7.8: building OCCT ${OPAD_OCCT_VERSION} from source")
      else()
        message(STATUS "OPAD: no system OCCT found: building OCCT ${OPAD_OCCT_VERSION} from source")
      endif()
      set(OPAD_OCCT_BUNDLED ON)
    endif()
  endif()
endif()

if(OPAD_OCCT_BUNDLED)
  if(WIN32)
    message(FATAL_ERROR "OPAD_OCCT_BUILD: a shared OCCT built from source is not supported on Windows; use MSYS2's "
      "OCCT (pacman -S mingw-w64-x86_64-occt) or the windows-static preset")
  endif()
  # FreeType (view cube and dimension text) and RapidJSON (glTF) from the OS packages, as for the static build.
  find_path(OPAD_OCCT_FREETYPE_INCLUDE ft2build.h PATH_SUFFIXES freetype2)
  if(NOT OPAD_OCCT_FREETYPE_INCLUDE)
    message(FATAL_ERROR "OPAD_OCCT: FreeType headers not found (Debian/Ubuntu: apt install libfreetype-dev)")
  endif()
  find_path(OPAD_OCCT_RAPIDJSON_INCLUDE rapidjson/rapidjson.h)
  set(_occt_rapidjson "")
  if(OPAD_OCCT_RAPIDJSON_INCLUDE)
    set(_occt_rapidjson -DUSE_RAPIDJSON=ON "-D3RDPARTY_RAPIDJSON_INCLUDE_DIR=${OPAD_OCCT_RAPIDJSON_INCLUDE}")
  else()
    message(WARNING "OPAD_OCCT: RapidJSON headers not found (Debian/Ubuntu: apt install rapidjson-dev); glTF is left out")
  endif()
  if(APPLE)
    set(_occt_platform -DUSE_XLIB=OFF "-DINSTALL_NAME_DIR=${OPAD_OCCT_INSTALL_DIR}/lib")
  else()
    set(_occt_platform -DUSE_XLIB=ON "-DCMAKE_INSTALL_RPATH=$ORIGIN")  # X11 windows for the view; toolkits find each other
  endif()
  opad_occt_from_source(OPAD_OCCT Shared
    ${_occt_platform}
    "-D3RDPARTY_FREETYPE_INCLUDE_DIR_ft2build=${OPAD_OCCT_FREETYPE_INCLUDE}"
    "-D3RDPARTY_FREETYPE_INCLUDE_DIR_freetype2=${OPAD_OCCT_FREETYPE_INCLUDE}"
    ${_occt_rapidjson})
  list(APPEND CMAKE_BUILD_RPATH "${OPAD_OCCT_INSTALL_DIR}/lib")  # every program and test runs from the build tree
endif()
