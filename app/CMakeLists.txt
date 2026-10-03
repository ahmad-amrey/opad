find_package(Qt6 COMPONENTS Widgets Gui Network QUIET)
find_package(Qt6 QUIET COMPONENTS PrintSupport)  # Plot to a printer (UI-88); without it the Plot dialog writes PDFs only
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

# Every .cpp/.hpp under app/ is part of the app: CONFIGURE_DEPENDS re-globs at each build, so a new file needs no edit
# here and parallel branches never meet in a source list. A file for one platform has its own extension (the .mm below)
# or guards its body; anything else to leave out goes into list(REMOVE_ITEM) here.
file(GLOB_RECURSE OPAD_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
list(SORT OPAD_APP_SOURCES)

# Translations: app/i18n/<code>.json and the area fragments app/i18n/<code>/*.json that I18n.cpp merges after it. The
# qrc is generated from a glob (rewritten only when the list changes), so a new language or fragment is a new file only.
file(GLOB_RECURSE opad_i18n_files CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.json)
list(SORT opad_i18n_files)
set(opad_i18n_qrc "<RCC>\n  <qresource prefix=\"/\">\n")
foreach(file IN LISTS opad_i18n_files)
  string(APPEND opad_i18n_qrc "    <file alias=\"${file}\">${CMAKE_CURRENT_SOURCE_DIR}/${file}</file>\n")
endforeach()
string(APPEND opad_i18n_qrc "  </qresource>\n</RCC>\n")
set(OPAD_I18N_QRC ${CMAKE_CURRENT_BINARY_DIR}/i18n.qrc CACHE INTERNAL "")  # also in tests/test_seams
file(CONFIGURE OUTPUT ${OPAD_I18N_QRC} CONTENT "${opad_i18n_qrc}" @ONLY)
list(APPEND OPAD_APP_SOURCES res.qrc ${OPAD_I18N_QRC})
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
target_link_libraries(opad PRIVATE opad::core opad_paint ${OPAD_OCCT_VIS_LIBS} Qt6::Widgets Qt6::Gui Qt6::Network)
if(TARGET Qt6::PrintSupport)
  target_link_libraries(opad PRIVATE Qt6::PrintSupport)
  target_compile_definitions(opad PRIVATE OPAD_HAVE_PRINT=1)
endif()
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
  # the SVG icon engine stay out. The Windows platform and style plugins come in as usual. No TLS or network
  # information plugins either: OPAD talks only through local sockets, and the OpenSSL backend alone carries OpenSSL.
  qt_import_plugins(opad INCLUDE_BY_TYPE imageformats Qt6::QJpegPlugin EXCLUDE_BY_TYPE iconengines tls networkinformation)
endif()
install(TARGETS opad RUNTIME DESTINATION bin BUNDLE DESTINATION .)

if(WIN32 AND NOT OPAD_STATIC)
  include(${PROJECT_SOURCE_DIR}/cmake/portable.cmake)  # target opad-portable
endif()
include(${PROJECT_SOURCE_DIR}/cmake/single.cmake)  # target opad-single: the one-file deliverable of each OS
