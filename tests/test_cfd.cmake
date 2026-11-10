# The OpenFOAM solves (several minutes each, silent meanwhile) as a test of their own, labelled slow: the test presets leave it
# out (ctest -L slow, or the <os>-slow preset, runs it); the plain cfd test runs the quick rest.
add_test(NAME cfd-solve COMMAND ${target} cfd_)
set_tests_properties(cfd-solve PROPERTIES ENVIRONMENT "OPAD_TEST_CFD=1" LABELS slow TIMEOUT 7200)
