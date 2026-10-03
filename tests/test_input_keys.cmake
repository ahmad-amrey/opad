# Included by tests/CMakeLists.txt once it builds every test_<name>.cpp by glob (TODO 11 T0): the key model is an app header.
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../app)
