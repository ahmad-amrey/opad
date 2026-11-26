# The OpenFOAM solves (a minute or more each, silent meanwhile) as a test of their own, labelled slow: the test presets leave it
# out (ctest -L slow, or the <os>-slow preset, runs it); the plain flow test runs the quick rest. OPAD_TEST_FLOW_3D=1 adds
# the full-part comparison (snappyHexMesh, several minutes).
add_test(NAME flow-solve COMMAND ${target} flow_solve_)
set_tests_properties(flow-solve PROPERTIES ENVIRONMENT "OPAD_TEST_FLOW=1" LABELS slow TIMEOUT 7200)
