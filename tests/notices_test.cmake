# ctest notices-script (TODO 11 UI-13/14): cmake/notices.cmake the way the build step and the package scripts run it,
# over a stand-in build.ninja that links a library of this host (zlib), with the host's package lookup (MSYS2's pacman,
# Homebrew or Debian's dpkg); and the GPL guard's file names. -DSOURCE=<repo> -DWORK=<scratch dir> -DLIB=<zlib library>
# [-DPACMAN=<pacman>]

set(failures 0)
macro(check message)  # the rest of the arguments: the condition
  if(${ARGN})
    message(STATUS "ok: ${message}")
  else()
    message(SEND_ERROR "FAIL: ${message}")
    math(EXPR failures "${failures} + 1")
  endif()
endmacro()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
# A program of OPAD (read) and a test program (not read), which names a library nobody ships.
file(WRITE "${WORK}/build.ninja"
  "build bin/opad.exe: CXX_EXECUTABLE_LINKER__opad_Release core/x.obj\n"
  "  LINK_LIBRARIES = lib/libopad_core.a ${LIB}\n"
  "build bin/opad-test-core.exe: CXX_EXECUTABLE_LINKER__opad-test-core_Release core/y.obj\n"
  "  LINK_LIBRARIES = ${WORK}/libnotshipped.a\n")
file(WRITE "${WORK}/libnotshipped.a" "")
file(WRITE "${WORK}/extra.txt" "* Extra notice appended.\n")

set(NOTICES_OUT "${WORK}/THIRD-PARTY-NOTICES.txt")
set(NOTICES_CPP "${WORK}/notices.cpp")
set(NOTICES_NINJA "${WORK}/build.ninja")
set(NOTICES_TARGETS "^bin/opad(-cli)?([.](exe|cp[^/]*|so))?$")
set(NOTICES_OWN "${WORK}")
set(NOTICES_PACMAN "${PACMAN}")
set(NOTICES_VERSION "9.8.7")
set(NOTICES_SCOPE "Test scope.")
set(NOTICES_EXTRA "${WORK}/extra.txt")
include("${SOURCE}/cmake/notices.cmake")

file(READ "${NOTICES_OUT}" text)
file(READ "${NOTICES_CPP}" cpp)
string(FIND "${text}" "OPAD 9.8.7: third-party notices\n" at)
check("the text opens with the version" at EQUAL 0)
string(FIND "${text}" "Test scope." at)
check("the scope is stated" NOT at EQUAL -1)
string(FIND "${text}" "* Extra notice appended." at)
check("the extra file is appended" NOT at EQUAL -1)
string(FIND "${text}" "libnotshipped" at)
check("a program that is not OPAD's is not read" at EQUAL -1)
string(FIND "${text}" "libopad_core" at)
check("OPAD's own build output is not listed" at EQUAL -1)
string(FIND "${cpp}" "R\"opad_notices(OPAD 9.8.7" at)
check("the C++ source carries the text" NOT at EQUAL -1)
get_filename_component(lib_name "${LIB}" NAME)
file(REAL_PATH "${LIB}" lib_real)
get_filename_component(real_name "${lib_real}" NAME)
if(PACMAN OR lib_real MATCHES "/Cellar/" OR EXISTS "/var/lib/dpkg/status")
  # A package line for zlib with its version, licence and source, and the licence text after the list.
  string(REGEX MATCH "\n\\* (zlib[^ ]*) ([^ \n]+): [^\n]*\n  Licence: ([^\n]+)\n  Home: +[^\n]*\n  Source: +([^\n]+)\n  Files: +([^\n]+)" entry "${text}")
  check("zlib is listed as a package: ${CMAKE_MATCH_1} ${CMAKE_MATCH_2}, ${CMAKE_MATCH_3}, ${CMAKE_MATCH_4}" entry)
  set(pkg "${CMAKE_MATCH_1}")
  string(FIND "${CMAKE_MATCH_5}" "${lib_name}" by_name)
  string(FIND "${CMAKE_MATCH_5}" "${real_name}" by_real)
  check("its file is named" NOT by_name EQUAL -1 OR NOT by_real EQUAL -1)
  string(FIND "${text}" "\nLicence texts\n" at)
  string(FIND "${text}" "\n${pkg}: " text_at)
  check("its licence text follows the list" NOT at EQUAL -1 AND text_at GREATER at)
  string(REGEX MATCH "\nThe libraries come from (MSYS2 packages|Homebrew formulae|the system's Debian packages)" origin "${text}")
  check("the header says where the packages come from" origin)

  # The package form: licence files copied to licenses/<package>/ instead of inlined.
  set(NOTICES_CPP "")
  set(NOTICES_OUT "${WORK}/package/THIRD-PARTY-NOTICES.txt")
  set(NOTICES_LICENSES_DIR "${WORK}/package/licenses")
  include("${SOURCE}/cmake/notices.cmake")
  file(READ "${NOTICES_OUT}" text)
  file(GLOB copied "${WORK}/package/licenses/${pkg}/*")
  check("licence files copied to licenses/${pkg}/" pkg AND copied)
  string(FIND "${text}" "\nLicence texts\n" at)
  check("no inline texts in the package form" at EQUAL -1)
else()
  string(FIND "${text}" "${lib_name}" at)
  check("without package information the library is listed by name" NOT at EQUAL -1)
endif()

# GPL guard: what MSYS2, Debian and Homebrew name the FFmpeg, codec, FreeImage and OpenVR libraries, and look-alikes.
include("${SOURCE}/cmake/gpl_guard.cmake")
opad_gpl_libraries(gpl C:/x/avcodec-62.dll avutil-60.dll swscale-9.dll libx264-165.dll libx265-215.dll xvidcore.dll
  libfreeimage-3.dll libopenvr_api.dll /usr/lib/libavformat.so.60 libxvidcore.so.4 libfreeimageplus.so.3
  libswresample.5.dylib libpostproc.58.dylib
  Qt6Core.dll libTKService.dll libavahi-client.so.3 libswiftCore.dylib libx264ish.dll zlib1.dll libopenvr.dll)
list(LENGTH gpl count)
check("the guard refuses the 13 GPL names and passes the rest (${gpl})" count EQUAL 13)
opad_gpl_guard("test package" "${WORK}/clean" Qt6Core.dll zlib1.dll)
check("a clean package passes the guard" NOT EXISTS "${WORK}/clean/NOT-FOR-DISTRIBUTION.txt")
set(OPAD_ALLOW_GPL_DLLS ON)
file(MAKE_DIRECTORY "${WORK}/local")
opad_gpl_guard("test package" "${WORK}/local" avcodec-62.dll)
check("allowed GPL libraries mark the package NOT FOR DISTRIBUTION" EXISTS "${WORK}/local/NOT-FOR-DISTRIBUTION.txt")

if(failures)
  message(FATAL_ERROR "${failures} check(s) failed")
endif()
