# Agents' git tools (app/GitAgent.cpp) through opad-cli's headless MCP server against temporary repositories; the test
# reads repositories with the panel's git client (app/Git.cpp). opad-cli has the tools only with Qt.
find_package(Qt6 COMPONENTS Core QUIET)
if(TARGET opad-cli AND Qt6_FOUND)
  target_sources(${target} PRIVATE ../app/Git.cpp ../app/Git.hpp ../app/GitAgent.cpp)
  target_include_directories(${target} PRIVATE ../app)
  target_link_libraries(${target} PRIVATE Qt6::Core)
  add_dependencies(${target} opad-cli)
  set_tests_properties(${name} PROPERTIES TIMEOUT 600)
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
