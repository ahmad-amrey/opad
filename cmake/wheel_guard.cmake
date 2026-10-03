# GPL guard for the Python wheel (TODO 11 UI-14, decision D1), run on the repaired wheel before it is handed out:
#   cmake -DWHEEL=<opad-*.whl> -P cmake/wheel_guard.cmake
# delvewheel, auditwheel and delocate copy the module's shared libraries into the wheel, Open CASCADE's among them, and
# with those whatever its TKService loads: MSYS2's, Debian's and Homebrew's builds bring FFmpeg, FreeImage and OpenVR.
# Fails naming them (the wheel is left as it is: it must not be distributed).
include("${CMAKE_CURRENT_LIST_DIR}/gpl_guard.cmake")
if(NOT WHEEL OR NOT EXISTS "${WHEEL}")
  message(FATAL_ERROR "usage: cmake -DWHEEL=<file.whl> -P cmake/wheel_guard.cmake")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar tf "${WHEEL}" OUTPUT_VARIABLE listing ERROR_VARIABLE error RESULT_VARIABLE failed)
if(failed OR NOT listing)
  message(FATAL_ERROR "${WHEEL}: cannot list its files. ${error}")
endif()
string(STRIP "${listing}" listing)
string(REPLACE "\n" ";" files "${listing}")
list(FILTER files EXCLUDE REGEX "/$")  # folders
opad_gpl_libraries(gpl ${files})
if(gpl)
  list(JOIN gpl ", " names)
  message(FATAL_ERROR "${WHEEL} carries ${names}: do not distribute it.\n${OPAD_GPL_WHY}")
endif()
list(LENGTH files count)
message(STATUS "${WHEEL}: ${count} files, no GPL FFmpeg, codec, FreeImage or OpenVR library")
