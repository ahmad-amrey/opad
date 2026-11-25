# CalculiX ccx with SPOOLES and ARPACK, one static program. Configured by make-pack.sh with
#   -DSRC=<folder holding CalculiX/ and SPOOLES.2.2/> -DCCX_VERSION=2.22 -DARPACK=<libarpack.a> -DBLAS=<libopenblas.a>
# The source lists are read from the upstream makefiles (SPOOLES: each package's src/makefile; ccx: src/Makefile.inc), so
# the build compiles exactly what upstream's make does, without needing make.
cmake_minimum_required(VERSION 3.20)
project(opad_ccx C Fortran)

set(spooles "${SRC}/SPOOLES.2.2")
set(ccx "${SRC}/CalculiX/ccx_${CCX_VERSION}/src")

# Old C: K&R definitions and implicit declarations, which GCC 14+ refuses unless told the dialect. MinGW's own printf
# (__USE_MINGW_ANSI_STDIO): two-digit exponents as on Linux, which the .frd file's fixed columns need.
set(old_c -D__USE_MINGW_ANSI_STDIO=1 -std=gnu17 -fcommon -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -w)

# SPOOLES: the packages its top makefile's `lib` target builds (MPI and MT left out, as upstream comments them out).
file(STRINGS "${spooles}/makefile" lib_lines REGEX "^\tcd [A-Za-z0-9]+ +; make lib")
set(spooles_sources)
foreach(line IN LISTS lib_lines)
  string(REGEX REPLACE "^\tcd ([A-Za-z0-9]+) .*" "\\1" pkg "${line}")
  # As SPOOLES's makeLib (perl) does: every "$(OBJ).a(name.o)" in the package's src/makefile is src/name.c.
  file(READ "${spooles}/${pkg}/src/makefile" mk)
  string(REGEX MATCHALL "\\$\\(OBJ\\)\\.a\\([A-Za-z0-9_]+\\.o\\)" members "${mk}")
  if(NOT members)
    message(FATAL_ERROR "no sources listed in ${pkg}/src/makefile")
  endif()
  foreach(m IN LISTS members)
    string(REGEX REPLACE ".*\\(([A-Za-z0-9_]+)\\.o\\)" "\\1" f "${m}")
    list(APPEND spooles_sources "${spooles}/${pkg}/src/${f}.c")
  endforeach()
endforeach()
list(LENGTH spooles_sources n)
message(STATUS "SPOOLES: ${n} files")
add_library(spooles STATIC ${spooles_sources})
target_compile_options(spooles PRIVATE -O2 ${old_c})
# Two packages have a file of the same name (IO.c, util.c ...): objects are kept apart by CMake's per-source paths.

# ccx: Makefile.inc's SCCXF (Fortran) and SCCXC (C) lists, plus the main file.
file(READ "${ccx}/Makefile.inc" inc)
string(REGEX MATCH "SCCXF *=([^=]*)SCCXC" fblock "${inc}")
string(REGEX MATCH "SCCXC *=([^=]*)" cblock "${inc}")  # stops before SCCXCXX = umat_dl.cpp (not built upstream)
string(REGEX MATCHALL "[A-Za-z0-9_.]+\\.f" ffiles "${fblock}")  # names may hold dots: ccx_2.22step.c
string(REGEX MATCHALL "[A-Za-z0-9_.]+\\.c" cfiles "${cblock}")
list(TRANSFORM ffiles PREPEND "${ccx}/")
list(TRANSFORM cfiles PREPEND "${ccx}/")
# SCCXC names mafillmm.c, which does not exist: make builds mafillmm.o from mafillmm.f (also in SCCXF) instead.
foreach(c IN LISTS cfiles)
  if(NOT EXISTS "${c}")
    string(REGEX REPLACE "\\.c$" ".f" f "${c}")
    if(NOT f IN_LIST ffiles)
      message(FATAL_ERROR "ccx source missing: ${c}")
    endif()
    list(REMOVE_ITEM cfiles "${c}")
  endif()
endforeach()
list(LENGTH ffiles nf)
list(LENGTH cfiles nc)
message(STATUS "ccx: ${nf} Fortran, ${nc} C files")
add_executable(ccx "${ccx}/ccx_${CCX_VERSION}.c" ${ffiles} ${cfiles})
set_target_properties(ccx PROPERTIES LINKER_LANGUAGE Fortran OUTPUT_NAME ccx)
target_include_directories(ccx PRIVATE "${spooles}" "${ccx}")
target_compile_definitions(ccx PRIVATE ARCH=Linux SPOOLES ARPACK MATRIXSTORAGE NETWORKOUT)
target_compile_options(ccx PRIVATE -O2 $<$<COMPILE_LANGUAGE:C>:${old_c}> $<$<COMPILE_LANGUAGE:Fortran>:-fallow-argument-mismatch -w>)
target_link_libraries(ccx PRIVATE spooles "${ARPACK}" "${BLAS}" -static -fopenmp pthread m)
