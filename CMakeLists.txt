cmake_minimum_required(VERSION 3.25)

project(opad VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPAD_BUILD_CLI     "Build the opad-cli headless tool"     ON)
option(OPAD_BUILD_APP     "Build the Qt 6 desktop application"   ON)
option(OPAD_BUILD_PYTHON  "Build the pybind11 'opad' module"     ON)
option(OPAD_BUILD_PLUGINS "Build the sample C-ABI plugin"        ON)
option(OPAD_BUILD_TESTS   "Build headless tests"                 ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W3 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Put every runtime artefact side by side so DLL lookup works out of the box on Windows.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
if(MINGW)
  include(cmake/mingw.cmake)  # runtime DLLs staged in bin/
endif()

find_package(OpenCASCADE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

include(cmake/occt.cmake)

add_subdirectory(core)
if(OPAD_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(OPAD_BUILD_PLUGINS)
  add_subdirectory(plugins/sample_exporter)
endif()
if(OPAD_BUILD_PYTHON)
  add_subdirectory(python)
endif()
if(OPAD_BUILD_APP)
  add_subdirectory(app)
endif()
if(OPAD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
