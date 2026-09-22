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
  Viewport.cpp ViewportDesign.cpp ViewportInspect.cpp ViewportNotes.cpp ViewportSection.cpp Viewport.hpp
  Notes.cpp Notes.hpp
  DesignController.cpp DesignController.hpp
  DesignPanels.cpp DesignPanels.hpp
  SketchEditor.cpp SketchTools.cpp SketchEditor.hpp
  Panels.cpp Panels.hpp
  Jobs.cpp Jobs.hpp
  GuidedTool.cpp GuidedTool.hpp
  BodyShape.cpp BodyShape.hpp
  NavCube.cpp NavCube.hpp
  CrashLog.cpp CrashLog.hpp
  I18n.cpp I18n.hpp i18n.qrc
  res.qrc
  MainWindow.cpp MainWindow.hpp)
if(APPLE)
  list(APPEND OPAD_APP_SOURCES ViewportCocoa.mm)
endif()
if(WIN32)
  # Exe icon (res/opad.ico, cut from the logo by tools/make_icon.py) and version info.
  enable_language(RC)
  configure_file(res/opad.rc.in ${CMAKE_CURRENT_BINARY_DIR}/opad.rc @ONLY)
  list(APPEND OPAD_APP_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/opad.rc)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/opad.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/opad.ico)
endif()

add_executable(opad WIN32 MACOSX_BUNDLE ${OPAD_APP_SOURCES})
target_link_libraries(opad PRIVATE opad::core ${OPAD_OCCT_VIS_LIBS} Qt6::Widgets Qt6::Gui)
if(WIN32)
  target_link_libraries(opad PRIVATE dbghelp)
endif()
if(APPLE)
  target_link_libraries(opad PRIVATE "-framework Cocoa")
endif()
if(OPAD_STATIC)
  target_compile_definitions(opad PRIVATE OPAD_SINGLE_FILE=1)  # portable by default: settings and cache beside the exe
  # Static Qt links every plugin of every type it knows by default; OPAD reads PNG (built into QtGui) and writes
  # PNG/JPEG screenshots, so the other image formats (TIFF alone drags in zstd, lzma, jbig, lerc, libdeflate) and
  # the SVG icon engine stay out. The Windows platform and style plugins come in as usual.
  qt_import_plugins(opad INCLUDE_BY_TYPE imageformats Qt6::QJpegPlugin EXCLUDE_BY_TYPE iconengines)
endif()
install(TARGETS opad RUNTIME DESTINATION bin BUNDLE DESTINATION .)

if(WIN32 AND NOT OPAD_STATIC)
  include(${PROJECT_SOURCE_DIR}/cmake/portable.cmake)  # target opad-portable
endif()
include(${PROJECT_SOURCE_DIR}/cmake/single.cmake)  # target opad-single: the one-file deliverable of each OS
