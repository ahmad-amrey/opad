add_executable(opad-fixtures fixtures.cpp)
target_link_libraries(opad-fixtures PRIVATE opad::core)

add_executable(opad-test-core test_core.cpp)
target_link_libraries(opad-test-core PRIVATE opad::core)
add_test(NAME core COMMAND opad-test-core)

add_executable(opad-test-step test_step.cpp)
target_link_libraries(opad-test-step PRIVATE opad::core)

add_executable(opad-test-sketch test_sketch.cpp)
target_link_libraries(opad-test-sketch PRIVATE opad::core)
add_test(NAME sketch COMMAND opad-test-sketch)

add_executable(opad-test-design test_design.cpp)
target_link_libraries(opad-test-design PRIVATE opad::core)
add_test(NAME design COMMAND opad-test-design)
set_tests_properties(design PROPERTIES ENVIRONMENT "OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")

set(OPAD_FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
add_test(NAME fixtures COMMAND opad-fixtures ${OPAD_FIXTURE_DIR})
set_tests_properties(fixtures PROPERTIES FIXTURES_SETUP step_fixtures)

add_test(NAME step COMMAND opad-test-step ${OPAD_FIXTURE_DIR})
set_tests_properties(step PROPERTIES FIXTURES_REQUIRED step_fixtures ENVIRONMENT "OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND OPAD_BUILD_CLI)
  add_test(NAME cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py $<TARGET_FILE:opad-cli> ${OPAD_FIXTURE_DIR})
  set_tests_properties(cli PROPERTIES FIXTURES_REQUIRED step_fixtures ENVIRONMENT "OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
endif()

if(OPAD_BUILD_PLUGINS)
  add_test(NAME plugin COMMAND opad-cli --plugin $<TARGET_FILE:opad_sample_exporter> export ${OPAD_FIXTURE_DIR}/box.step --format ply --out ${CMAKE_BINARY_DIR}/plugin-out.ply)
  set_tests_properties(plugin PROPERTIES FIXTURES_REQUIRED step_fixtures)
endif()

if(OPAD_BUILD_PYTHON AND Python3_Interpreter_FOUND)
  add_test(NAME python COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/python/test_opad.py ${OPAD_FIXTURE_DIR})
  set_tests_properties(python PROPERTIES FIXTURES_REQUIRED step_fixtures
    ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:opad_python>;OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
endif()
