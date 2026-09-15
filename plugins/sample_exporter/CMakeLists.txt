# Sample native plugin (F35): a PLY exporter written against the C ABI only.
# It does not link opad_core; everything it needs comes through the host API function table.
add_library(opad_sample_exporter MODULE ply_exporter.cpp)
target_include_directories(opad_sample_exporter PRIVATE ${CMAKE_SOURCE_DIR}/plugins/include)
target_link_libraries(opad_sample_exporter PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(opad_sample_exporter PROPERTIES PREFIX "" OUTPUT_NAME "opad_ply_exporter")
install(TARGETS opad_sample_exporter LIBRARY DESTINATION plugins RUNTIME DESTINATION plugins)
