# MinGW-w64 (MSYS2) specifics.

# Runtime DLLs next to the executables. Git for Windows ships its own, incompatible libstdc++-6.dll and puts it
# first on the PATH of Git Bash and VS Code terminals, so the toolchain's copy has to sit beside the exe for the
# loader to take it first. MSYS2 keeps these DLLs next to the compiler.
set(OPAD_RUNTIME_DLLS "")
get_filename_component(_opad_gcc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
foreach(_rt libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
  if(EXISTS "${_opad_gcc_bin}/${_rt}")
    list(APPEND OPAD_RUNTIME_DLLS "${_opad_gcc_bin}/${_rt}")
  else()
    message(WARNING "OPAD: ${_rt} not found next to ${CMAKE_CXX_COMPILER}; the binaries will need it on PATH")
  endif()
endforeach()

add_custom_target(opad-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMENT "Staging MinGW runtime DLLs in ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  VERBATIM)
if(OPAD_RUNTIME_DLLS)
  add_custom_command(TARGET opad-runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPAD_RUNTIME_DLLS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" VERBATIM)
endif()
