# OPAD's .gitignore blocks (app/Git.cpp: Qt Core only) and git check-ignore in temporary repositories; without Qt the
# test is not built.
find_package(Qt6 COMPONENTS Core QUIET)
if(Qt6_FOUND AND TARGET opad)
  target_sources(${target} PRIVATE ../app/Git.cpp ../app/Git.hpp)
  target_include_directories(${target} PRIVATE ../app)
  target_link_libraries(${target} PRIVATE Qt6::Core)
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
