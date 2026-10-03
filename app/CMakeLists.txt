find_package(Qt6 COMPONENTS Widgets Gui Network QUIET)
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
  AppSnapshot.cpp SketchRecovery.cpp RecoveryManager.cpp RecoveryManager.hpp
  AgentBridge.cpp AgentBridge.hpp AgentSettings.cpp AgentWork.cpp AgentBench.cpp AgentRegistration.cpp AgentRegistration.hpp
  Theme.cpp Theme.hpp
  Icons.cpp Icons.hpp
  Ribbon.cpp Ribbon.hpp
  EmptyState.cpp EmptyState.hpp
  FileAssociations.cpp FileAssociations.hpp
  Viewport.cpp ViewportPicking.cpp ViewportPickingBench.cpp ViewportDesign.cpp ViewportInspect.cpp ViewportNotes.cpp ViewportRefine.cpp ViewportSection.cpp Viewport.hpp
  ViewportSelection.cpp ViewportSettings.cpp
  ViewportTracking.cpp
  Notes.cpp Notes.hpp AnnotationEditor.cpp AnnotationEditor.hpp
  DesignController.cpp DesignController.hpp PlanePicker.cpp PlanePicker.hpp
  DesignPanels.cpp DesignPanels.hpp
  CheckPanel.cpp CheckPanel.hpp
  SketchEditor.cpp SketchNodes.cpp SketchTools.cpp SketchEditor.hpp SketchGeometryCache.cpp SketchGeometryCache.hpp
  SketchPanel.cpp SketchPanel.hpp SketchWorkflow.cpp
  DimensionHandle.cpp DimensionHandle.hpp
  InstanceBrowser.cpp
  SketchPrimitives.cpp
  SketchModify.cpp
  SketchReference.cpp
  SketchImages.cpp
  GeometrySelection.cpp
  Panels.cpp Panels.hpp
  ShortcutEditor.cpp ShortcutEditor.hpp ShortcutBench.cpp
  Jobs.cpp Jobs.hpp
  GuidedTool.cpp GuidedTool.hpp
  BodyShape.cpp BodyShape.hpp
  NavCube.cpp NavCube.hpp
  CrashLog.cpp CrashLog.hpp
  I18n.cpp I18n.hpp i18n.qrc
  res.qrc
  MainWindow.cpp MainWindow.hpp DrawingWizard.cpp DrawingPlacer.cpp DrawingPlacer.hpp ExportDialog.cpp Todo5Bench.cpp Todo9Bench.cpp LargeSketchBench.cpp SmartBench.cpp)
if(APPLE)
  list(APPEND OPAD_APP_SOURCES ViewportCocoa.mm)
  find_program(OPAD_SIPS NAMES sips REQUIRED)
  find_program(OPAD_ICONUTIL NAMES iconutil REQUIRED)
  set(_opad_mac_iconset "${CMAKE_CURRENT_BINARY_DIR}/opad.iconset")
  set(_opad_mac_icon "${CMAKE_CURRENT_BINARY_DIR}/opad-full.icns")
  set(_opad_icon_source "${CMAKE_CURRENT_SOURCE_DIR}/res/opad-256.png")
  add_custom_command(OUTPUT "${_opad_mac_icon}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_opad_mac_iconset}"
    COMMAND "${OPAD_SIPS}" -z 16 16 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_16x16.png"
    COMMAND "${OPAD_SIPS}" -z 32 32 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_16x16@2x.png"
    COMMAND "${OPAD_SIPS}" -z 32 32 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_32x32.png"
    COMMAND "${OPAD_SIPS}" -z 64 64 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_32x32@2x.png"
    COMMAND "${OPAD_SIPS}" -z 128 128 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_128x128.png"
    COMMAND "${OPAD_SIPS}" -z 256 256 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_128x128@2x.png"
    COMMAND "${OPAD_SIPS}" -z 256 256 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_256x256.png"
    COMMAND "${OPAD_SIPS}" -z 512 512 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_256x256@2x.png"
    COMMAND "${OPAD_SIPS}" -z 512 512 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_512x512.png"
    COMMAND "${OPAD_SIPS}" -z 1024 1024 "${_opad_icon_source}" --out "${_opad_mac_iconset}/icon_512x512@2x.png"
    COMMAND "${OPAD_ICONUTIL}" -c icns "${_opad_mac_iconset}" -o "${_opad_mac_icon}"
    DEPENDS "${_opad_icon_source}" "${CMAKE_CURRENT_LIST_FILE}"
    VERBATIM)
  set_source_files_properties("${_opad_mac_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  list(APPEND OPAD_APP_SOURCES "${_opad_mac_icon}")
endif()
if(WIN32)
  # Exe icon (res/opad.ico, cut from the logo by tools/make_icon.py) and version info.
  enable_language(RC)
  configure_file(res/opad.rc.in ${CMAKE_CURRENT_BINARY_DIR}/opad.rc @ONLY)
  list(APPEND OPAD_APP_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/opad.rc)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/opad.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/opad.ico)
endif()

add_executable(opad WIN32 MACOSX_BUNDLE ${OPAD_APP_SOURCES})
target_link_libraries(opad PRIVATE opad::core ${OPAD_OCCT_VIS_LIBS} Qt6::Widgets Qt6::Gui Qt6::Network)
if(WIN32)
  target_link_libraries(opad PRIVATE dbghelp)
endif()
if(APPLE)
  set_target_properties(opad PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.opad.OPAD
    MACOSX_BUNDLE_BUNDLE_NAME OPAD
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE opad-full.icns
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/res/Info.plist.in)  # declares the formats Finder opens with OPAD
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
