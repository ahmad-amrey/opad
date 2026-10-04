# opad_paint: PDF and PNG of 2D drawings through Qt's painter (TODO 11 UI-86, UI-87), shared by the app and opad-cli.
find_package(Qt6 COMPONENTS Gui QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "OPAD: Qt 6 Gui not found; PDF and PNG drawings are not available")
  return()
endif()
add_library(opad_paint STATIC paint.cpp include/opad/drawing/paint.hpp)
target_include_directories(opad_paint PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(opad_paint PUBLIC opad::core Qt6::Gui)
# The drawing fonts (SIL OFL 1.1, licences beside them in third_party/fonts) compiled into every program that paints sheets,
# the single-file executables included: nothing to install or stage beside them (UI-139).
qt_add_resources(opad_paint drawing_fonts PREFIX "/opad/fonts" BASE "${PROJECT_SOURCE_DIR}/third_party/fonts"
  FILES
    "${PROJECT_SOURCE_DIR}/third_party/fonts/LiberationSans-Regular.ttf"
    "${PROJECT_SOURCE_DIR}/third_party/fonts/NotoSansArabic-Regular.ttf"
    "${PROJECT_SOURCE_DIR}/third_party/fonts/LiberationSans-LICENSE.txt"
    "${PROJECT_SOURCE_DIR}/third_party/fonts/NotoSansArabic-OFL.txt")
