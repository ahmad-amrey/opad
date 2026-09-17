# MinGW-w64 specifics, shared by the native MSYS2 build and the Linux cross build (linux-mingw64 preset).

# Thread model. OCCT and app/Jobs.cpp use std::thread and std::mutex, which libstdc++ only has with the posix
# (or MSYS2's mcf) model. Debian/Ubuntu ship both flavours and pick win32 as the default alternative.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -v ERROR_VARIABLE _opad_gcc_v OUTPUT_QUIET ERROR_STRIP_TRAILING_WHITESPACE)
if(_opad_gcc_v MATCHES "Thread model: win32")
  message(FATAL_ERROR "OPAD: ${CMAKE_CXX_COMPILER} uses the win32 thread model, which has no std::thread. "
    "Select the posix flavour: apt install g++-mingw-w64-x86-64-posix, then "
    "update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix "
    "and the same for x86_64-w64-mingw32-g++.")
endif()

# Runtime DLLs next to the executables. Git for Windows ships its own, incompatible libstdc++-6.dll and puts it
# first on the PATH of Git Bash and VS Code terminals, so the toolchain's copy has to sit beside the exe for the
# loader to take it first. MSYS2 keeps these DLLs next to the compiler; a Linux cross toolchain keeps them in
# GCC's library directories, which the driver can locate.
set(OPAD_RUNTIME_DLLS "")
get_filename_component(_opad_gcc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
foreach(_rt libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
  set(_path "${_opad_gcc_bin}/${_rt}")
  if(NOT EXISTS "${_path}")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${_rt}
      OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if(IS_ABSOLUTE "${_path}" AND EXISTS "${_path}")
    list(APPEND OPAD_RUNTIME_DLLS "${_path}")
  else()
    message(WARNING "OPAD: ${_rt} not found for ${CMAKE_CXX_COMPILER}; the binaries will need it on PATH")
  endif()
endforeach()

# Cross builds through vcpkg: its applocal step (copying dependency DLLs next to each exe) only runs on Windows
# hosts, so stage every DLL of the target triplet and Qt's platform plugin ourselves. The result in bin/ is
# self-contained and can be zipped as is.
set(_opad_qt_platform "")
if(CMAKE_CROSSCOMPILING AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  file(GLOB _opad_vcpkg_dlls "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll")
  list(APPEND OPAD_RUNTIME_DLLS ${_opad_vcpkg_dlls})
  file(GLOB _opad_qt_platform "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms/qwindows.dll")
  list(LENGTH _opad_vcpkg_dlls _n)
  message(STATUS "OPAD: staging ${_n} vcpkg DLLs from ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
endif()

add_custom_target(opad-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMENT "Staging Windows runtime DLLs in ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  VERBATIM)
if(OPAD_RUNTIME_DLLS)
  add_custom_command(TARGET opad-runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPAD_RUNTIME_DLLS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" VERBATIM)
endif()
if(_opad_qt_platform)
  add_custom_command(TARGET opad-runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_opad_qt_platform}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms" VERBATIM)
endif()
