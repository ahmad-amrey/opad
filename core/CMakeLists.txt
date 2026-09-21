configure_file(include/opad/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/opad/version.hpp @ONLY)

add_library(opad_core STATIC
  src/util.cpp
  src/document.cpp
  src/scene.cpp
  src/geometry.cpp
  src/cache.cpp
  src/mesh.cpp
  src/step_io.cpp
  src/inspect.cpp
  src/render.cpp
  src/diff.cpp
  src/commands.cpp
  src/plugin_host.cpp
  src/design/expr.cpp
  src/design/sketch.cpp
  src/design/solver.cpp
  src/design/sketch_geom.cpp
  src/design/engine.cpp
  src/design/features.cpp
  src/design/commands_design.cpp)
add_library(opad::core ALIAS opad_core)

target_include_directories(opad_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/plugins/include>
    ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(opad_core
  PUBLIC ${OPAD_OCCT_LIBS} nlohmann_json::nlohmann_json
  PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(opad_core PUBLIC _USE_MATH_DEFINES)
if(OPAD_HAVE_GLTF)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_GLTF=1)
endif()
if(NOT MSVC)
  # Keep the software rasteriser bit-identical across compilers (N4): no fused multiply-add contraction.
  set_source_files_properties(src/render.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
