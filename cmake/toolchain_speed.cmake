# Build speed: ccache as the compiler launcher and LLD as the MinGW linker, when they are installed. Included from the
# top-level CMakeLists.txt before any target.
#
# A launcher or linker the user chose is left alone: -DCMAKE_<LANG>_COMPILER_LAUNCHER=... (empty turns ccache off for
# one tree), the CMAKE_<LANG>_COMPILER_LAUNCHER environment variable (CMake turns it into that cache entry),
# -fuse-ld=... in CMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS (or LDFLAGS, which seeds them), CMAKE_LINKER_TYPE.
# -DOPAD_CCACHE=OFF / -DOPAD_LLD=OFF switch the defaults off. Both are set as normal variables, never cached, so
# switching an option off takes effect on the next configure.
#
# The single-file build (OPAD_STATIC) keeps GNU ld: its FreeType group in CMAKE_CXX_STANDARD_LIBRARIES relies on GNU
# archive order (cmake/static.cmake, notes in CLAUDE.md), which LLD resolves differently.

option(OPAD_CCACHE "Compile through ccache when it is installed (unless a compiler launcher is set)" ON)
if(OPAD_STATIC)
  set(_opad_lld_default OFF)
else()
  set(_opad_lld_default ON)
endif()
option(OPAD_LLD "Link with LLD on MinGW when ld.lld is installed (unless a linker is set)" ${_opad_lld_default})

if(OPAD_CCACHE)
  find_program(OPAD_CCACHE_PROGRAM ccache)
  mark_as_advanced(OPAD_CCACHE_PROGRAM)
  if(OPAD_CCACHE_PROGRAM)
    foreach(_opad_lang C CXX)
      if(NOT DEFINED CMAKE_${_opad_lang}_COMPILER_LAUNCHER)
        set(CMAKE_${_opad_lang}_COMPILER_LAUNCHER "${OPAD_CCACHE_PROGRAM}")
        message(STATUS "${_opad_lang} compiler launcher: ${OPAD_CCACHE_PROGRAM} (OPAD_CCACHE)")
      endif()
    endforeach()
  endif()
endif()

if(MINGW AND OPAD_LLD AND NOT DEFINED CMAKE_LINKER_TYPE)
  set(_opad_link_flags "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS}")
  if(NOT _opad_link_flags MATCHES "-fuse-ld=")
    get_filename_component(_opad_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(OPAD_LLD_PROGRAM ld.lld HINTS "${_opad_compiler_dir}")
    mark_as_advanced(OPAD_LLD_PROGRAM)
    if(OPAD_LLD_PROGRAM)
      include(CheckLinkerFlag)
      check_linker_flag(CXX -fuse-ld=lld OPAD_HAVE_FUSE_LD_LLD)
      if(OPAD_HAVE_FUSE_LD_LLD)
        add_link_options(-fuse-ld=lld)
        message(STATUS "Linker: ${OPAD_LLD_PROGRAM} (OPAD_LLD)")
      endif()
    endif()
  endif()
endif()
