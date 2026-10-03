# GitClient (app/Git.cpp) against real git in temporary repositories, with opad and opad-cli of this build as the
# merge and diff drivers; without the desktop app (no Qt) the test is not built.
if(TARGET opad AND TARGET opad-cli)
  target_sources(${target} PRIVATE ../app/Git.cpp ../app/Git.hpp)
  target_include_directories(${target} PRIVATE ../app)
  target_link_libraries(${target} PRIVATE Qt6::Core)
  add_dependencies(${target} opad opad-cli)
  set_tests_properties(${name} PROPERTIES TIMEOUT 180)
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
