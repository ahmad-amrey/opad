find_package(Python3 COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG)
if(NOT pybind11_FOUND OR NOT Python3_Development.Module_FOUND)
  message(WARNING "OPAD: pybind11 or Python development files not found; skipping the 'opad' Python module")
  return()
endif()

pybind11_add_module(opad_python MODULE bindings.cpp)
set_target_properties(opad_python PROPERTIES OUTPUT_NAME opad)
target_link_libraries(opad_python PRIVATE opad::core)

if(SKBUILD)
  install(TARGETS opad_python LIBRARY DESTINATION . RUNTIME DESTINATION .)
  # The libraries the module links (core/CMakeLists.txt writes the list at build time), beside the wheel's licence
  # files in .dist-info/licenses (scikit-build-core's metadata folder); the repaired wheel adds OCCT's DLLs.
  set(_opad_notices_dir .)
  if(SKBUILD_METADATA_DIR)
    set(_opad_notices_dir ${SKBUILD_METADATA_DIR}/licenses)
  endif()
  install(FILES ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${_opad_notices_dir})
else()
  install(TARGETS opad_python LIBRARY DESTINATION python RUNTIME DESTINATION python)
endif()
