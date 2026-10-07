# The simulation engines (core/src/sim): Eigen for the kinematic solver, Project Chrono for dynamics and Netgen for the
# meshes CalculiX analyses. Chrono (BSD-3) and Netgen (LGPL-2.1) are built once from their git tags into the build tree,
# like OCCT on Linux (cmake/occt_source.cmake), as shared libraries the programs find through their RPATH; Netgen is built
# against the same OCCT as OPAD. CalculiX's solver (ccx, GPL) is a separate program OPAD runs and never links: on the PATH,
# beside OPAD, or named by OPAD_CCX (see core/src/sim/fea.cpp).
#
#   OPAD_CHRONO=ON|OFF  dynamic studies (default ON)
#   OPAD_NETGEN=ON|OFF  static and modal studies (default ON)
#   OPAD_CHRONO_SOURCE_DIR / OPAD_NETGEN_SOURCE_DIR: a checkout to build from instead of cloning (offline builds)
#
# Each is cloned at a pinned commit (verified), configured with only what OPAD uses (Chrono's core module: multibody,
# contacts, motors; Netgen's mesher with its OCC geometry, no GUI and no Python), built and installed into
# build/<preset>/<name>/install once; delete that folder to build it again. Neither tag builds with MinGW as released:
# opad_patch_<name> edits the checkout before it is configured. On Windows their DLLs are copied beside the programs.
#
# Sets OPAD_HAVE_CHRONO / OPAD_HAVE_NETGEN and the imported targets opad::chrono / opad::netgen.

# Eigen 3 (header only): the system's (Ubuntu's 3.4), else 3.4.1 from its pinned tarball into build/<preset>/eigen.
# MSYS2 ships Eigen 5, which Chrono 9 does not take (its FindEigen3 reads the version where Eigen 5 no longer has it);
# OPAD and Chrono must compile against the same Eigen, so 5 is never used.
set(OPAD_EIGEN_VERSION 3.4.1)
set(OPAD_EIGEN_URL "https://gitlab.com/libeigen/eigen/-/archive/${OPAD_EIGEN_VERSION}/eigen-${OPAD_EIGEN_VERSION}.tar.gz")
set(OPAD_EIGEN_SHA256 b93c667d1b69265cdb4d9f30ec21f8facbbe8b307cf34c0b9942834c6d4fdbe2)
find_package(Eigen3 3.3...<4 QUIET NO_MODULE)
if(Eigen3_FOUND)
  get_target_property(EIGEN3_INCLUDE_DIR Eigen3::Eigen INTERFACE_INCLUDE_DIRECTORIES)
else()
  set(_eigen_root "${CMAKE_BINARY_DIR}/eigen")
  set(EIGEN3_INCLUDE_DIR "${_eigen_root}/eigen-${OPAD_EIGEN_VERSION}")
  if(NOT EXISTS "${EIGEN3_INCLUDE_DIR}/Eigen/Core")
    set(_eigen_tarball "${_eigen_root}/eigen-${OPAD_EIGEN_VERSION}.tar.gz")
    if(NOT EXISTS "${_eigen_tarball}")
      message(STATUS "OPAD: no Eigen 3 installed, downloading ${OPAD_EIGEN_URL}")
      file(DOWNLOAD "${OPAD_EIGEN_URL}" "${_eigen_tarball}" EXPECTED_HASH SHA256=${OPAD_EIGEN_SHA256} STATUS _dl)
      list(GET _dl 0 _rc)
      if(NOT _rc EQUAL 0)
        file(REMOVE "${_eigen_tarball}")
        message(FATAL_ERROR "OPAD: downloading Eigen ${OPAD_EIGEN_VERSION} failed (${_dl}); install Eigen 3 or put the tarball at ${_eigen_tarball}")
      endif()
    endif()
    file(SHA256 "${_eigen_tarball}" _sha)
    if(NOT _sha STREQUAL OPAD_EIGEN_SHA256)
      message(FATAL_ERROR "OPAD: ${_eigen_tarball} is not Eigen ${OPAD_EIGEN_VERSION} (SHA256 ${_sha})")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_eigen_tarball}" DESTINATION "${_eigen_root}"
         PATTERNS "eigen-${OPAD_EIGEN_VERSION}/Eigen" "eigen-${OPAD_EIGEN_VERSION}/unsupported"
                  "eigen-${OPAD_EIGEN_VERSION}/signature_of_eigen3_matrix_library" "eigen-${OPAD_EIGEN_VERSION}/COPYING.*")
  endif()
  add_library(Eigen3::Eigen INTERFACE IMPORTED GLOBAL)
  set_target_properties(Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}")
  message(STATUS "OPAD: Eigen ${OPAD_EIGEN_VERSION} from ${EIGEN3_INCLUDE_DIR}")
endif()

option(OPAD_CHRONO "Dynamic studies with Project Chrono (built from source into the build tree once)" ON)
option(OPAD_NETGEN "Static and modal studies with Netgen meshes (built from source into the build tree once)" ON)

set(OPAD_CHRONO_VERSION 9.0.1)
set(OPAD_CHRONO_GIT https://github.com/projectchrono/chrono.git)
set(OPAD_CHRONO_COMMIT 2617649bf687456328a122b63dc0bb64df91394a)
set(OPAD_NETGEN_VERSION 6.2.2507)
set(OPAD_NETGEN_GIT https://github.com/NGSolve/netgen.git)
set(OPAD_NETGEN_COMMIT 9642315f7a930701df33c917c537a51a4f3b46cd)

# Replaces `old` by `new` once in <src>/<file> before the engine is configured; stops when `old` is missing (a new tag).
function(opad_engine_patch src file old new)
  file(READ "${src}/${file}" _text)
  string(FIND "${_text}" "${new}" _done)
  if(_done GREATER -1)
    return()
  endif()
  string(FIND "${_text}" "${old}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "OPAD: patch target not found in ${src}/${file}: ${old}")
  endif()
  string(REPLACE "${old}" "${new}" _text "${_text}")
  file(WRITE "${src}/${file}" "${_text}")
endfunction()

# What the pinned Chrono needs to build with MinGW GCC 15 (MSYS2): <cstdint> was included through other headers before,
# and its Windows GCC branch links no socket library (ChSocket wants ws2_32, HACD's timer winmm).
function(opad_patch_chrono src)
  opad_engine_patch("${src}" src/chrono/utils/ChSocket.cpp "#include \"chrono/utils/ChSocket.h\"\n"
                    "#include \"chrono/utils/ChSocket.h\"\n#include <cstdint>  // OPAD: GCC 15\n")
  opad_engine_patch("${src}" src/chrono/CMakeLists.txt "SET (CH_SOCKET_LIB \"\")  # not needed?"
                    "SET (CH_SOCKET_LIB ws2_32 winmm)  # OPAD: MinGW")
endfunction()

# Netgen takes every Windows build for MSVC: its flags and linker options are kept to MSVC (MinGW gets big objects).
function(opad_patch_netgen src)
  opad_engine_patch("${src}" libsrc/core/CMakeLists.txt
    "  target_compile_options(ngcore PUBLIC /bigobj $<BUILD_INTERFACE:/MP;/W1;/wd4068>)\n"
    "  if(MSVC)  # OPAD: MinGW\n  target_compile_options(ngcore PUBLIC /bigobj $<BUILD_INTERFACE:/MP;/W1;/wd4068>)\n  else()\n  target_compile_options(ngcore PUBLIC -Wa,-mbig-obj)\n  endif()\n")
  opad_engine_patch("${src}" libsrc/core/CMakeLists.txt
    "  target_link_options(ngcore PUBLIC /ignore:4273 /ignore:4217 /ignore:4049)\n"
    "  if(MSVC)  # OPAD: MinGW\n  target_link_options(ngcore PUBLIC /ignore:4273 /ignore:4217 /ignore:4049)\n  endif()\n")
  opad_engine_patch("${src}" libsrc/core/utils.cpp
    "    void* func = GetProcAddress((HMODULE)lib, func_name.c_str());"
    "    void* func = reinterpret_cast<void*>(GetProcAddress((HMODULE)lib, func_name.c_str()));  // OPAD: MinGW")
  # Inline members marked dllimport (GCC refuses their definitions; inline code needs no export).
  opad_engine_patch("${src}" libsrc/core/bitarray.hpp "  NGCORE_API auto * Data() const"
                    "  /* OPAD: MinGW */ auto * Data() const")
  opad_engine_patch("${src}" libsrc/core/bitarray.hpp "    NGCORE_API TBitArray & Or (const TBitArray & ba2)"
                    "    /* OPAD: MinGW */ TBitArray & Or (const TBitArray & ba2)")
  opad_engine_patch("${src}" libsrc/gprim/spline.hpp "    DLL_HEADER virtual const GeomPoint<D> & StartPI ()"
                    "    /* OPAD: MinGW */ virtual const GeomPoint<D> & StartPI ()")
  opad_engine_patch("${src}" libsrc/gprim/spline.hpp "    DLL_HEADER virtual const GeomPoint<D> & EndPI ()"
                    "    /* OPAD: MinGW */ virtual const GeomPoint<D> & EndPI ()")
endfunction()

# opad_engine_from_source(<name> <tag> <git> <commit> <config file relative to install> [configure args...])
# Calls opad_patch_<name>(<source dir>) first when it is defined.
function(opad_engine_from_source name tag git commit config)
  string(TOUPPER "${name}" _up)
  set(_root "${CMAKE_BINARY_DIR}/${name}")
  set(_install "${_root}/install")
  set(${_up}_INSTALL_DIR "${_install}" PARENT_SCOPE)
  set(_src "${OPAD_${_up}_SOURCE_DIR}")
  if(NOT _src)
    set(_src "${_root}/source")
  endif()
  set(${_up}_SOURCE_USED "${_src}" PARENT_SCOPE)
  if(EXISTS "${_install}/${config}")
    return()
  endif()
  if(NOT OPAD_${_up}_SOURCE_DIR)
    if(NOT EXISTS "${_src}/CMakeLists.txt")
      find_package(Git REQUIRED)
      message(STATUS "OPAD_${_up}: cloning ${name} ${tag} from ${git}")
      execute_process(COMMAND "${GIT_EXECUTABLE}" clone --quiet --depth 1 --branch "${tag}" "${git}" "${_src}" RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "OPAD_${_up}: cloning ${git} failed; set OPAD_${_up}_SOURCE_DIR to a checkout of ${tag} or -DOPAD_${_up}=OFF")
      endif()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_src}" rev-parse HEAD OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _head STREQUAL commit)
      message(FATAL_ERROR "OPAD_${_up}: ${_src} is at ${_head}, not ${tag} (${commit})")
    endif()
  endif()
  if(COMMAND opad_patch_${name})
    cmake_language(CALL opad_patch_${name} "${_src}")
  endif()
  message(STATUS "OPAD_${_up}: building ${name} ${tag} in ${_root}/build (once; this takes a while)")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" -S "${_src}" -B "${_root}/build"
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=${_install}"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}" "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
      "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}" ${ARGN}
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "OPAD_${_up}: configuring ${name} failed (${_rc})")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" --target install RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR NOT EXISTS "${_install}/${config}")
    message(FATAL_ERROR "OPAD_${_up}: building ${name} failed (${_rc})")
  endif()
endfunction()

if(APPLE)
  set(_opad_engine_rpath "-DCMAKE_INSTALL_RPATH=@loader_path")
else()
  set(_opad_engine_rpath "-DCMAKE_INSTALL_RPATH=\$ORIGIN")
endif()

# What the third-party notices list for the engines (cmake/notices.cmake NOTICES_BUILT): '^'-joined below.
set(OPAD_NOTICES_BUILT "")

set(OPAD_HAVE_CHRONO OFF)
if(OPAD_CHRONO)
  opad_engine_from_source(chrono ${OPAD_CHRONO_VERSION} ${OPAD_CHRONO_GIT} ${OPAD_CHRONO_COMMIT} include/chrono/ChConfig.h
    -DBUILD_DEMOS=OFF -DBUILD_TESTING=OFF -DBUILD_BENCHMARKING=OFF -DENABLE_OPENMP=OFF -DUSE_SIMD=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_MPI=ON -DCMAKE_DISABLE_FIND_PACKAGE_CUDA=ON "-DEIGEN3_INCLUDE_DIR=${EIGEN3_INCLUDE_DIR}"
    -DMSVC_VERSION=0  # MinGW: Chrono expands an unset ${MSVC_VERSION} in an if() (an error), every use is behind MSVC
    ${_opad_engine_rpath})
  add_library(opad_chrono SHARED IMPORTED GLOBAL)
  add_library(opad::chrono ALIAS opad_chrono)
  if(WIN32)
    set_target_properties(opad_chrono PROPERTIES IMPORTED_LOCATION "${CHRONO_INSTALL_DIR}/bin/libChronoEngine.dll"
      IMPORTED_IMPLIB "${CHRONO_INSTALL_DIR}/lib/libChronoEngine.dll.a")
  elseif(APPLE)
    set_target_properties(opad_chrono PROPERTIES IMPORTED_LOCATION "${CHRONO_INSTALL_DIR}/lib/libChronoEngine.dylib")
  else()
    set_target_properties(opad_chrono PROPERTIES IMPORTED_LOCATION "${CHRONO_INSTALL_DIR}/lib/libChronoEngine.so")
  endif()
  set_target_properties(opad_chrono PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CHRONO_INSTALL_DIR}/include;${CHRONO_INSTALL_DIR}/include/chrono;${CHRONO_INSTALL_DIR}/include/chrono/collision/bullet"
    INTERFACE_COMPILE_DEFINITIONS "EIGEN_DONT_PARALLELIZE")
  target_link_libraries(opad_chrono INTERFACE Eigen3::Eigen)
  list(APPEND CMAKE_BUILD_RPATH "${CHRONO_INSTALL_DIR}/lib")
  if(WIN32)  # no RPATH there: beside the programs
    file(COPY "${CHRONO_INSTALL_DIR}/bin/libChronoEngine.dll" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  # Its licence texts kept with the install (the notices read them from there, cmake/notices.cmake): Chrono's BSD licence,
  # and Bullet's zlib notice for the collision code compiled into it.
  file(COPY "${CHRONO_SOURCE_USED}/LICENSE" DESTINATION "${CHRONO_INSTALL_DIR}/share/licenses")
  file(COPY_FILE "${PROJECT_SOURCE_DIR}/cmake/notices_bullet.txt" "${CHRONO_INSTALL_DIR}/share/licenses/Bullet-zlib.txt" ONLY_IF_DIFFERENT)
  list(APPEND OPAD_NOTICES_BUILT
    "chrono|${CHRONO_INSTALL_DIR}|${OPAD_CHRONO_VERSION}|${OPAD_CHRONO_GIT} (tag ${OPAD_CHRONO_VERSION}, commit ${OPAD_CHRONO_COMMIT})|BSD-3-Clause, with Bullet's collision detection under Zlib|https://projectchrono.org|Project Chrono multibody dynamics (dynamic studies)|${CHRONO_INSTALL_DIR}/share/licenses/LICENSE,${CHRONO_INSTALL_DIR}/share/licenses/Bullet-zlib.txt")
  set(OPAD_HAVE_CHRONO ON)
  message(STATUS "OPAD: Project Chrono ${OPAD_CHRONO_VERSION} from ${CHRONO_INSTALL_DIR} (dynamic studies)")
endif()

set(OPAD_HAVE_NETGEN OFF)
if(OPAD_NETGEN)
  # Netgen links OCCT and sets its own install RPATH ($ORIGIN): the directories of what it links are added to it, so an OCCT
  # built in this tree is found where it was built (a bundle has them side by side).
  set(_opad_netgen_rpath -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON)
  opad_engine_from_source(netgen v${OPAD_NETGEN_VERSION} ${OPAD_NETGEN_GIT} ${OPAD_NETGEN_COMMIT} include/nglib.h
    -DUSE_SUPERBUILD=OFF -DUSE_GUI=OFF -DUSE_PYTHON=OFF -DUSE_MPI=OFF -DUSE_OCC=ON
    -DUSE_NATIVE_ARCH=OFF -DUSE_JPEG=OFF -DUSE_MPEG=OFF -DUSE_CGNS=OFF -DENABLE_UNIT_TESTS=OFF -DBUILD_STUB_FILES=OFF
    -DUSE_INTERNAL_TCL=OFF -DINSTALL_PROFILES=OFF -DNG_INSTALL_DIR_LIB=lib -DNG_INSTALL_DIR_BIN=bin -DNG_INSTALL_DIR_INCLUDE=include
    -DNG_INSTALL_DIR_CMAKE=lib/cmake/netgen -DNG_INSTALL_DIR_RES=share -DNG_INSTALL_DIR_PYTHON=lib/python
    "-DOpenCascade_ROOT=${OpenCASCADE_DIR}" "-DOpenCASCADE_DIR=${OpenCASCADE_DIR}" ${_opad_netgen_rpath})
  add_library(opad_netgen INTERFACE IMPORTED GLOBAL)
  add_library(opad::netgen ALIAS opad_netgen)
  if(WIN32)
    set(_ng_libs "${NETGEN_INSTALL_DIR}/lib/libnglib.dll.a;${NETGEN_INSTALL_DIR}/lib/libngcore.dll.a")
  elseif(APPLE)
    set(_ng_libs "${NETGEN_INSTALL_DIR}/lib/libnglib.dylib;${NETGEN_INSTALL_DIR}/lib/libngcore.dylib")
  else()
    set(_ng_libs "${NETGEN_INSTALL_DIR}/lib/libnglib.so;${NETGEN_INSTALL_DIR}/lib/libngcore.so")
  endif()
  set_target_properties(opad_netgen PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NETGEN_INSTALL_DIR}/include;${NETGEN_INSTALL_DIR}/include/include"
    INTERFACE_LINK_LIBRARIES "${_ng_libs}"
    INTERFACE_COMPILE_DEFINITIONS "OCCGEOMETRY")
  list(APPEND CMAKE_BUILD_RPATH "${NETGEN_INSTALL_DIR}/lib")
  if(WIN32)
    file(COPY "${NETGEN_INSTALL_DIR}/bin/libngcore.dll" "${NETGEN_INSTALL_DIR}/bin/libnglib.dll" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  file(COPY "${NETGEN_SOURCE_USED}/LICENSE" DESTINATION "${NETGEN_INSTALL_DIR}/share/licenses")
  list(APPEND OPAD_NOTICES_BUILT
    "netgen|${NETGEN_INSTALL_DIR}|${OPAD_NETGEN_VERSION}|${OPAD_NETGEN_GIT} (tag v${OPAD_NETGEN_VERSION}, commit ${OPAD_NETGEN_COMMIT})|LGPL-2.1-only|https://ngsolve.org|Netgen mesh generator (static and modal studies)|${NETGEN_INSTALL_DIR}/share/licenses/LICENSE")
  set(OPAD_HAVE_NETGEN ON)
  message(STATUS "OPAD: Netgen ${OPAD_NETGEN_VERSION} from ${NETGEN_INSTALL_DIR} (static and modal studies, with CalculiX's ccx)")
endif()
list(JOIN OPAD_NOTICES_BUILT "^" OPAD_NOTICES_BUILT)
