# Script side of the opad-single target on Windows: fails when the executable imports a DLL that is not part of
# Windows itself, which is what "runs anywhere with nothing to unpack" means. `objdump -p` lists the import table.
#   cmake -DOPAD_EXE=<exe> -DOPAD_OBJDUMP=<objdump> -P single_check.cmake

if(NOT OPAD_EXE OR NOT EXISTS "${OPAD_EXE}")
  message(FATAL_ERROR "single_check.cmake: OPAD_EXE not set or missing: ${OPAD_EXE}")
endif()
if(NOT OPAD_OBJDUMP)
  find_program(OPAD_OBJDUMP objdump REQUIRED)
endif()

execute_process(COMMAND "${OPAD_OBJDUMP}" -p "${OPAD_EXE}" OUTPUT_VARIABLE _dump RESULT_VARIABLE _rc ERROR_QUIET)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "single_check.cmake: objdump failed on ${OPAD_EXE}")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" _imports "${_dump}")
set(_system "^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|uuid|comdlg32|comctl32|ws2_32|wsock32|winmm|imm32|version|"
            "opengl32|glu32|dwmapi|uxtheme|shlwapi|setupapi|netapi32|userenv|mpr|authz|ntdll|d3d9|d3d11|d3d12|dxgi|d2d1|dwrite|"
            "dbghelp|rpcrt4|wtsapi32|winspool|crypt32|bcrypt|ncrypt|secur32|winhttp|iphlpapi|synchronization|runtimeobject|shcore|psapi|"
            "msvcrt|msvcp[0-9]+|msvcr[0-9]+|api-ms-win-[a-z0-9-]+|ucrtbase|vcruntime[0-9]+|usp10)\\.dll$|^winspool\\.drv$")  # the print spooler (Qt PrintSupport: Plot)
string(REPLACE ";" "" _system "${_system}")
set(_foreign "")
set(_all "")
foreach(_line IN LISTS _imports)
  string(REGEX REPLACE "^DLL Name: " "" _dll "${_line}")
  string(TOLOWER "${_dll}" _dll)
  list(APPEND _all "${_dll}")
  if(NOT _dll MATCHES "${_system}")
    list(APPEND _foreign "${_dll}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _all)
list(JOIN _all ", " _all_text)
get_filename_component(_name "${OPAD_EXE}" NAME)
if(_foreign)
  list(REMOVE_DUPLICATES _foreign)
  list(JOIN _foreign ", " _foreign_text)
  message(FATAL_ERROR "${_name} is not self-contained: it imports ${_foreign_text} (all imports: ${_all_text})")
endif()
file(SIZE "${OPAD_EXE}" _size)
math(EXPR _mb "${_size} / 1048576")
message(STATUS "${_name}: ${_mb} MB, imports only Windows DLLs (${_all_text})")
