# Link flags of the single-file build (OPAD_STATIC=ON). Included from the top-level CMakeLists.txt before any target.
#
# Every executable links its libraries in: Qt from MSYS2's qt6-static, OCCT from cmake/occt_static.cmake, the
# compiler runtime, FreeType and friends from the OS packages' .a archives. Unreferenced code is dropped at link
# time (the static OCCT toolkits are built with function sections for that, see occt_static.cmake) and the release
# binaries are stripped, which is what keeps the exe at tens of MB instead of hundreds.

set(OPAD_STATIC_CXX_FLAGS "-ffunction-sections -fdata-sections")
add_compile_options(-ffunction-sections -fdata-sections)
if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()
if(NOT APPLE)
  add_link_options(-Wl,--gc-sections)
endif()
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -s")
set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})  # an archive before an import library
# HarfBuzz's own CMake config names its import library outright (libharfbuzz.dll.a, an imported DLL), so Qt's
# FindWrapSystemHarfbuzz must fall back to pkg-config, whose -lharfbuzz the static link resolves to the archive.
set(CMAKE_DISABLE_FIND_PACKAGE_harfbuzz ON)
