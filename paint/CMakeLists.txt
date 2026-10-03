# opad_paint: PDF and PNG of 2D drawings through Qt's painter (TODO 11 UI-86, UI-87), shared by the app and opad-cli.
find_package(Qt6 COMPONENTS Gui QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "OPAD: Qt 6 Gui not found; PDF and PNG drawings are not available")
  return()
endif()
add_library(opad_paint STATIC paint.cpp include/opad/drawing/paint.hpp)
target_include_directories(opad_paint PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(opad_paint PUBLIC opad::core Qt6::Gui)
