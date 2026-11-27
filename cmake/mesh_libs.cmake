# Mesh editing libraries (core/src/mesh): Manifold for Booleans, plane cuts and shells, meshoptimizer for simplifying.
# Both are permissive (Manifold Apache-2.0, meshoptimizer MIT) and fetched from their pinned release tarballs into
# build/<preset>/meshlibs at the first configure, then compiled here as static libraries from their own source lists
# (Manifold's CMake sets RPATH and build-type globals of its own; only its library is wanted). No OS package is used:
# MSYS2 has Manifold 3.2.1, Ubuntu none, and a result must be the same on every system (replay compares fingerprints).
# So Manifold is built serial everywhere (MANIFOLD_PAR=-1, no TBB) with its own floating-point flags
# (-ffp-contract=off, -fexcess-precision=standard), as upstream builds it.
#
#   OPAD_MANIFOLD_SOURCE_DIR / OPAD_MESHOPT_SOURCE_DIR: an unpacked source tree to build from instead (offline builds)
#
# Sets the targets opad::manifold and opad::meshopt.

set(OPAD_MANIFOLD_VERSION 3.5.5)
set(OPAD_MANIFOLD_URL "https://github.com/elalish/manifold/archive/refs/tags/v${OPAD_MANIFOLD_VERSION}.tar.gz")
set(OPAD_MANIFOLD_SHA256 e4542acaa8852e7151928a78f908711bf30f4da5132c0f735f1ba739997e19d1)
set(OPAD_MESHOPT_VERSION 1.3)
set(OPAD_MESHOPT_URL "https://github.com/zeux/meshoptimizer/archive/refs/tags/v${OPAD_MESHOPT_VERSION}.tar.gz")
set(OPAD_MESHOPT_SHA256 ca4f47d451201593904b9e3067fa54b65db94bc34ab5173864950862b748a145)

# Downloads and unpacks <name>-<version>.tar.gz once (checked against its SHA-256); sets <out> to the source folder.
function(opad_fetch_tarball name version url sha out)
  set(root "${CMAKE_BINARY_DIR}/meshlibs")
  set(dir "${root}/${name}-${version}")
  if(NOT EXISTS "${dir}/.opad-unpacked")
    set(tarball "${root}/${name}-${version}.tar.gz")
    if(NOT EXISTS "${tarball}")
      message(STATUS "OPAD: downloading ${url}")
      file(DOWNLOAD "${url}" "${tarball}" EXPECTED_HASH SHA256=${sha} STATUS dl)
      list(GET dl 0 rc)
      if(NOT rc EQUAL 0)
        file(REMOVE "${tarball}")
        message(FATAL_ERROR "OPAD: downloading ${name} ${version} failed (${dl}); put the tarball at ${tarball}")
      endif()
    endif()
    file(SHA256 "${tarball}" got)
    if(NOT got STREQUAL sha)
      message(FATAL_ERROR "OPAD: ${tarball} is not ${name} ${version} (SHA256 ${got})")
    endif()
    file(REMOVE_RECURSE "${dir}")
    file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${root}")
    file(TOUCH "${dir}/.opad-unpacked")
  endif()
  set(${out} "${dir}" PARENT_SCOPE)
endfunction()

if(OPAD_MANIFOLD_SOURCE_DIR)
  set(_manifold "${OPAD_MANIFOLD_SOURCE_DIR}")
else()
  opad_fetch_tarball(manifold ${OPAD_MANIFOLD_VERSION} "${OPAD_MANIFOLD_URL}" ${OPAD_MANIFOLD_SHA256} _manifold)
endif()
if(OPAD_MESHOPT_SOURCE_DIR)
  set(_meshopt "${OPAD_MESHOPT_SOURCE_DIR}")
else()
  opad_fetch_tarball(meshoptimizer ${OPAD_MESHOPT_VERSION} "${OPAD_MESHOPT_URL}" ${OPAD_MESHOPT_SHA256} _meshopt)
endif()

# Manifold: src/CMakeLists.txt's MANIFOLD_SRCS without the 2D CrossSection (Clipper2).
string(REPLACE "." ";" _mv "${OPAD_MANIFOLD_VERSION}")
list(GET _mv 0 MANIFOLD_VERSION_MAJOR)
list(GET _mv 1 MANIFOLD_VERSION_MINOR)
list(GET _mv 2 MANIFOLD_VERSION_PATCH)
set(_manifold_gen "${CMAKE_BINARY_DIR}/meshlibs/manifold-include")
configure_file("${_manifold}/cmake/version.h.in" "${_manifold_gen}/manifold/version.h" @ONLY)
set(_manifold_srcs boolean3.cpp boolean_result.cpp constructors.cpp csg_tree.cpp edge_op.cpp execution_impl.cpp face_op.cpp
    impl.cpp manifold.cpp minkowski.cpp polygon.cpp properties.cpp quickhull.cpp sdf.cpp smoothing.cpp sort.cpp
    subdivision.cpp tree2d.cpp)
list(TRANSFORM _manifold_srcs PREPEND "${_manifold}/src/")
add_library(opad_manifold STATIC ${_manifold_srcs})
target_include_directories(opad_manifold SYSTEM PUBLIC "${_manifold}/include" "${_manifold_gen}")
target_compile_features(opad_manifold PUBLIC cxx_std_17)
target_compile_definitions(opad_manifold PUBLIC MANIFOLD_PAR=-1)
target_compile_options(opad_manifold PRIVATE -O3 -ffp-contract=off -w)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fexcess-precision=standard OPAD_HAS_FP_PRECISION)
if(OPAD_HAS_FP_PRECISION)
  target_compile_options(opad_manifold PRIVATE -fexcess-precision=standard)
endif()
set_target_properties(opad_manifold PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(opad::manifold ALIAS opad_manifold)

# meshoptimizer: every source of its library (no options).
file(GLOB _meshopt_srcs "${_meshopt}/src/*.cpp")
add_library(opad_meshopt STATIC ${_meshopt_srcs})
target_include_directories(opad_meshopt SYSTEM PUBLIC "${_meshopt}/src")
target_compile_options(opad_meshopt PRIVATE -O3 -ffp-contract=off -w)
set_target_properties(opad_meshopt PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(opad::meshopt ALIAS opad_meshopt)
message(STATUS "OPAD: Manifold ${OPAD_MANIFOLD_VERSION} and meshoptimizer ${OPAD_MESHOPT_VERSION} from ${CMAKE_BINARY_DIR}/meshlibs")
