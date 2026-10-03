# Exploded views (core/src/explode.cpp) parse and bound bodies through the shape cache: keep it out of the user's cache.
set_tests_properties(${name} PROPERTIES ENVIRONMENT "OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
