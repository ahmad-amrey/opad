# GitClient (app/Git.cpp) against real git in temporary repositories, with opad and opad-cli of this build as the
# merge and diff drivers; without the desktop app (no Qt) the test is not built.
if(TARGET opad AND TARGET opad-cli)
  target_sources(${target} PRIVATE ../app/Git.cpp ../app/Git.hpp ../app/OpHistory.cpp ../app/OpHistory.hpp)
  target_include_directories(${target} PRIVATE ../app)
  find_package(Qt6 REQUIRED COMPONENTS Network)  # the test's HTTP git host
  target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Network)
  if(OPAD_STATIC)  # static Qt imports its TLS backends, which want OpenSSL's archives; the test's host is plain HTTP (as the app's list)
    qt_import_plugins(${target} EXCLUDE_BY_TYPE tls networkinformation)
  endif()
  add_dependencies(${target} opad opad-cli)
  set_tests_properties(${name} PROPERTIES TIMEOUT 600)  # 1-2 min alone, several times that on a loaded machine
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
