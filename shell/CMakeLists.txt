# Explorer thumbnails (Windows): opad-thumbnails.dll beside opad.exe, registered by Settings > File types. It runs
# opad-cli to render, so it links nothing of OPAD; its C++ runtime is linked in, so Windows' helper process loads it
# without the MinGW DLLs.
add_library(opad-thumbnails SHARED thumbnails.cpp)
set_target_properties(opad-thumbnails PROPERTIES PREFIX "" OUTPUT_NAME "opad-thumbnails")
target_compile_definitions(opad-thumbnails PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(opad-thumbnails PRIVATE ole32 uuid shlwapi gdi32)
if(MINGW)
  target_link_options(opad-thumbnails PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()
if(TARGET opad-cli)
  add_dependencies(opad-thumbnails opad-cli)  # what it runs
endif()
