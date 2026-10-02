configure_file(include/opad/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/opad/version.hpp @ONLY)

# The agent guide is compiled in (TODO 10 B2): a raw string generated from core/res/agent_guide.md, rewritten only when
# the text changed, so editing the guide re-runs the configure step and rebuilds one file.
set(OPAD_AGENT_GUIDE ${CMAKE_CURRENT_SOURCE_DIR}/res/agent_guide.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OPAD_AGENT_GUIDE})
file(READ ${OPAD_AGENT_GUIDE} opad_guide_text)
string(REPLACE "\r" "" opad_guide_text "${opad_guide_text}")
string(FIND "${opad_guide_text}" ")opad_guide" opad_guide_clash)
if(NOT opad_guide_clash EQUAL -1)
  message(FATAL_ERROR "agent_guide.md must not contain the raw-string delimiter )opad_guide")
endif()
set(opad_guide_cpp "// Generated from core/res/agent_guide.md; edit that file.\n#include \"opad/agent.hpp\"\n\nnamespace opad::agent {\nconst std::string& guide() {\n  static const std::string text = R\"opad_guide(${opad_guide_text})opad_guide\";\n  return text;\n}\n}  // namespace opad::agent\n")
set(opad_guide_out ${CMAKE_CURRENT_BINARY_DIR}/agent_guide.cpp)
set(opad_guide_old "")
if(EXISTS ${opad_guide_out})
  file(READ ${opad_guide_out} opad_guide_old)
endif()
if(NOT "${opad_guide_old}" STREQUAL "${opad_guide_cpp}")
  file(WRITE ${opad_guide_out} "${opad_guide_cpp}")
endif()

add_library(opad_core STATIC
  src/util.cpp
  src/document.cpp
  src/scene.cpp
  src/geometry.cpp
  src/mass.cpp
  src/mesh_circles.cpp
  src/cache.cpp
  src/mesh.cpp
  src/mesh_recovery.cpp
  src/step_io.cpp
  src/drawing_io.cpp
  src/formats.cpp
  src/inspect.cpp
  src/render.cpp
  src/diff.cpp
  src/commands.cpp
  src/command_schema.cpp
  src/agent_context.cpp
  src/checks.cpp
  src/live.cpp
  src/plugin_host.cpp
  src/design/expr.cpp
  src/design/sketch.cpp
  src/design/sketch_records.cpp
  src/design/sketch_create.cpp
  src/design/sketch_shapes.cpp
  src/design/sketch_text.cpp
  src/design/sketch_modify.cpp
  src/design/sketch_pattern.cpp
  src/design/sketch_curve.cpp
  src/design/sketch_reference.cpp
  src/design/sketch_trace.cpp
  src/design/drawing_sketch.cpp
  src/design/solver.cpp
  src/design/sketch_edit.cpp
  src/design/sketch_geom.cpp
  src/design/engine.cpp
  src/design/features.cpp
  src/design/commands_design.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/agent_guide.cpp)
add_library(opad::core ALIAS opad_core)

target_include_directories(opad_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/plugins/include>
    ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(opad_core
  PUBLIC ${OPAD_OCCT_LIBS} nlohmann_json::nlohmann_json
  PRIVATE ${CMAKE_DL_LIBS} ZLIB::ZLIB)
target_compile_definitions(opad_core PUBLIC _USE_MATH_DEFINES)
if(TARGET TKV3d AND TARGET TKService)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_FONT=1)
endif()
if(OPAD_HAVE_GLTF)
  target_compile_definitions(opad_core PRIVATE OPAD_HAVE_GLTF=1)
endif()
if(NOT MSVC)
  # Keep the software rasteriser bit-identical across compilers (N4): no fused multiply-add contraction.
  set_source_files_properties(src/render.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
