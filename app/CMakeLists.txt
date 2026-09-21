find_package(Qt6 COMPONENTS Widgets Gui QUIET)
if(NOT Qt6_FOUND)
  message(WARNING "OPAD: Qt 6 not found; skipping the desktop app (set OPAD_BUILD_APP=OFF to silence)")
  return()
endif()
if(NOT OPAD_OCCT_VIS_LIBS)
  message(WARNING "OPAD: OCCT visualisation toolkits (TKV3d/TKOpenGl) not found; skipping the desktop app")
  return()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(OPAD_APP_SOURCES
  main.cpp
  AppDocument.cpp AppDocument.hpp
  Theme.cpp Theme.hpp
  Icons.cpp Icons.hpp
  Ribbon.cpp Ribbon.hpp
  EmptyState.cpp EmptyState.hpp
  Viewport.cpp Viewport.hpp
  Panels.cpp Panels.hpp
  Jobs.cpp Jobs.hpp
  GuidedTool.cpp GuidedTool.hpp
  BodyShape.cpp BodyShape.hpp
  NavCube.cpp NavCube.hpp
  CrashLog.cpp CrashLog.hpp
  I18n.cpp I18n.hpp i18n.qrc
  MainWindow.cpp MainWindow.hpp)
if(APPLE)
  list(APPEND OPAD_APP_SOURCES ViewportCocoa.mm)
endif()

add_executable(opad WIN32 MACOSX_BUNDLE ${OPAD_APP_SOURCES})
target_link_libraries(opad PRIVATE opad::core ${OPAD_OCCT_VIS_LIBS} Qt6::Widgets Qt6::Gui)
if(WIN32)
  target_link_libraries(opad PRIVATE dbghelp)
endif()
if(APPLE)
  target_link_libraries(opad PRIVATE "-framework Cocoa")
endif()
install(TARGETS opad RUNTIME DESTINATION bin BUNDLE DESTINATION .)
