# KiCad's export as the kicad test and the kicad-cli bench run it.
add_executable(opad-fake-kicad-cli fake_kicad_cli.cpp)
target_link_libraries(opad-fake-kicad-cli PRIVATE opad::core)
add_dependencies(${target} opad-fake-kicad-cli)
target_compile_definitions(${target} PRIVATE OPAD_FAKE_KICAD_CLI="$<TARGET_FILE:opad-fake-kicad-cli>")
if(TARGET zstd::libzstd_static AND OPAD_STATIC)  # writes boards with embedded (compressed) models
  target_link_libraries(${target} PRIVATE zstd::libzstd_static)
  target_compile_definitions(${target} PRIVATE OPAD_HAVE_ZSTD=1)
elseif(TARGET zstd::libzstd_shared)
  target_link_libraries(${target} PRIVATE zstd::libzstd_shared)
  target_compile_definitions(${target} PRIVATE OPAD_HAVE_ZSTD=1)
endif()
