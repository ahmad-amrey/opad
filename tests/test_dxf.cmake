# ctest dxf (tests/CMakeLists.txt includes this with `target` and `name`): a stand-in ODA File Converter, so the test
# sees that the converter is used only when switched on (UI-14).
if(WIN32)
  add_executable(opad-fake-oda fake_oda.cpp)
  if(MINGW)
    target_link_options(opad-fake-oda PRIVATE -static)
  endif()
  target_compile_definitions(${target} PRIVATE OPAD_FAKE_ODA="$<TARGET_FILE:opad-fake-oda>")
  add_dependencies(${target} opad-fake-oda)
endif()
