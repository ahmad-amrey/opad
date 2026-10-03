# Smart selection's and Del's decisions (app/SmartRules.cpp, UI-95 / UI-04) on the core; the chip itself is the smartselect bench.
target_sources(${target} PRIVATE ../app/SmartRules.cpp ../app/SmartRules.hpp)
target_include_directories(${target} PRIVATE ../app)
set_tests_properties(${name} PROPERTIES ENVIRONMENT "OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
