# Target opad-single: the one-file deliverable of each OS, written to build/<preset>/single.
#
#   Windows  OPAD-<version>-windows-x64.exe (+ opad-cli-<version>-windows-x64.exe and THIRD-PARTY-NOTICES.txt, which
#            the exes also carry compiled in): the static build's executables,
#            checked to import nothing but Windows' own DLLs (cmake/single_check.cmake). Needs OPAD_STATIC
#            (preset windows-static); nothing to unpack, settings and cache go to <exe dir>/opad-data.
#   Linux    OPAD-<version>-linux-x86_64.AppImage: linuxdeploy with its Qt plugin bundles the app, Qt, OCCT and
#            the other libraries the distro build links (needs `linuxdeploy` and `linuxdeploy-plugin-qt` on PATH,
#            https://github.com/linuxdeploy).
#   macOS    OPAD-<version>-macos.dmg: the app bundle with Qt and the dylibs copied in by macdeployqt.
# On Linux and macOS cmake/bundle_stage.cmake then runs over the bundle: the GPL guard (it fails on FFmpeg, codecs,
# FreeImage or OpenVR unless OPAD_ALLOW_GPL_DLLS) and THIRD-PARTY-NOTICES.txt from the libraries copied in, inside the
# bundle and beside it.
#
# Included from app/CMakeLists.txt after the opad target exists.

set(_single_dir "${CMAKE_BINARY_DIR}/single")

if(NOT WIN32)
  option(OPAD_ALLOW_GPL_DLLS "opad-single: bundle FFmpeg, codec, FreeImage and OpenVR libraries anyway (local use only, never distribute)" OFF)
  get_target_property(_single_qmake Qt6::qmake IMPORTED_LOCATION)
  execute_process(COMMAND "${_single_qmake}" -query QT_INSTALL_LIBS OUTPUT_VARIABLE _single_qt_libs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${_single_qmake}" -query QT_INSTALL_PLUGINS OUTPUT_VARIABLE _single_qt_plugins OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  set(_single_lib_dirs "")
  foreach(_dir IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _single_lib_dirs "${_dir}/lib")
  endforeach()
  list(APPEND _single_lib_dirs ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} /usr/local/lib)
  string(REPLACE ";" "|" _single_lib_dirs "${_single_lib_dirs}")
  set(_single_headers "")
  if(TARGET nlohmann_json::nlohmann_json)
    get_target_property(_single_json_dirs nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_dir IN LISTS _single_json_dirs)
      if(EXISTS "${_dir}/nlohmann/json.hpp")
        set(_single_headers "${_dir}/nlohmann/json.hpp")
      endif()
    endforeach()
  endif()
  set(_single_stage ${CMAKE_COMMAND} "-DOPAD_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DOPAD_VERSION=${PROJECT_VERSION}"
    "-DOPAD_ALLOW_GPL_DLLS=${OPAD_ALLOW_GPL_DLLS}" "-DOPAD_QT_LIBS=${_single_qt_libs}" "-DOPAD_QT_PLUGINS=${_single_qt_plugins}"
    "-DOPAD_LIB_DIRS=${_single_lib_dirs}" "-DOPAD_NOTICES_HEADERS=${_single_headers}")
endif()

if(WIN32)
  if(NOT OPAD_STATIC)
    message(STATUS "OPAD: opad-single needs OPAD_STATIC=ON (preset windows-static); not available in this tree")
    return()
  endif()
  set(_single_app "${_single_dir}/OPAD-${PROJECT_VERSION}-windows-x64.exe")
  set(_single_cli "${_single_dir}/opad-cli-${PROJECT_VERSION}-windows-x64.exe")
  set(_single_cmds
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:opad>" "${_single_app}"
    # The notices are compiled into the exes too (Help > Third-party licences, opad-cli licenses); this is the same text.
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" "${_single_dir}/THIRD-PARTY-NOTICES.txt"
    COMMAND ${CMAKE_COMMAND} "-DOPAD_EXE=${_single_app}" "-DOPAD_OBJDUMP=${CMAKE_OBJDUMP}"
            -P "${PROJECT_SOURCE_DIR}/cmake/single_check.cmake")
  set(_single_deps opad)
  if(TARGET opad-cli)
    list(APPEND _single_cmds
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:opad-cli>" "${_single_cli}"
      COMMAND ${CMAKE_COMMAND} "-DOPAD_EXE=${_single_cli}" "-DOPAD_OBJDUMP=${CMAKE_OBJDUMP}"
              -P "${PROJECT_SOURCE_DIR}/cmake/single_check.cmake")
    list(APPEND _single_deps opad-cli)
  endif()
  add_custom_target(opad-single ${_single_cmds} DEPENDS ${_single_deps}
    COMMENT "Single-file executables in ${_single_dir}" VERBATIM)

elseif(APPLE)
  get_filename_component(_single_qt_bin "${_single_qmake}" DIRECTORY)
  find_program(OPAD_MACDEPLOYQT NAMES macdeployqt HINTS "${_single_qt_bin}")
  find_program(OPAD_CODESIGN NAMES codesign REQUIRED)
  find_program(OPAD_HDIUTIL NAMES hdiutil REQUIRED)
  if(NOT OPAD_MACDEPLOYQT)
    message(STATUS "OPAD: macdeployqt not found; the opad-single target is not available")
    return()
  endif()
  if(NOT EXISTS "${_single_qt_plugins}/platforms/libqcocoa.dylib")
    message(FATAL_ERROR "OPAD: Qt Cocoa platform plugin not found in ${_single_qt_plugins}")
  endif()
  # macdeployqt normally copies every installed image and input plugin, including unrelated
  # ones whose optional Qt frameworks may not be present in its rpath search path.
  set(_single_plugin_commands)
  set(_single_plugin_args)
  foreach(_plugin IN ITEMS platforms/libqcocoa.dylib imageformats/libqjpeg.dylib styles/libqmacstyle.dylib)
    if(EXISTS "${_single_qt_plugins}/${_plugin}")
      get_filename_component(_plugin_dir "${_plugin}" DIRECTORY)
      set(_plugin_dest "${_single_dir}/OPAD.app/Contents/PlugIns/${_plugin}")
      list(APPEND _single_plugin_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_single_dir}/OPAD.app/Contents/PlugIns/${_plugin_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_single_qt_plugins}/${_plugin}" "${_plugin_dest}")
      list(APPEND _single_plugin_args "-executable=${_plugin_dest}")
    endif()
  endforeach()
  add_custom_target(opad-single
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:opad>" "${_single_dir}/OPAD.app"
    ${_single_plugin_commands}
    COMMAND "${OPAD_MACDEPLOYQT}" "${_single_dir}/OPAD.app" -always-overwrite -no-codesign
            -no-plugins ${_single_plugin_args}
    COMMAND ${_single_stage} "-DOPAD_BUNDLE=${_single_dir}/OPAD.app" -P "${PROJECT_SOURCE_DIR}/cmake/bundle_stage.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy "${_single_dir}/OPAD.app/Contents/Resources/THIRD-PARTY-NOTICES.txt" "${_single_dir}"
    COMMAND "${OPAD_CODESIGN}" --force --deep --sign - "${_single_dir}/OPAD.app"
    COMMAND "${OPAD_CODESIGN}" --verify --deep --strict "${_single_dir}/OPAD.app"
    COMMAND "${OPAD_HDIUTIL}" create -volname OPAD -srcfolder "${_single_dir}/OPAD.app"
            -format UDZO -ov "${_single_dir}/OPAD-${PROJECT_VERSION}-macos.dmg"
    DEPENDS opad
    COMMENT "App bundle + dmg in ${_single_dir}" VERBATIM)

elseif(UNIX)
  find_program(OPAD_LINUXDEPLOY NAMES linuxdeploy linuxdeploy-x86_64.AppImage)
  if(NOT OPAD_LINUXDEPLOY)
    message(STATUS "OPAD: linuxdeploy not found; the opad-single target is not available")
    return()
  endif()
  # Two linuxdeploy passes: deploy into AppDir, then (after the guard and the notices went in) make the AppImage. The
  # working folder (where the AppImage is written) is kept; only the AppDir is made afresh.
  file(MAKE_DIRECTORY "${_single_dir}")
  add_custom_target(opad-single
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_single_dir}/AppDir"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/app/res/opad-256.png" "${_single_dir}/opad.png"
    COMMAND ${CMAKE_COMMAND} -E env "QMAKE=${_single_qmake}"
            "${OPAD_LINUXDEPLOY}" --appdir "${_single_dir}/AppDir" --executable "$<TARGET_FILE:opad>"
            --desktop-file "${PROJECT_SOURCE_DIR}/app/res/opad.desktop" --icon-file "${_single_dir}/opad.png"
            --plugin qt
    COMMAND ${_single_stage} "-DOPAD_BUNDLE=${_single_dir}/AppDir" -P "${PROJECT_SOURCE_DIR}/cmake/bundle_stage.cmake"
    COMMAND ${CMAKE_COMMAND} -E env "OUTPUT=OPAD-${PROJECT_VERSION}-linux-x86_64.AppImage"
            "${OPAD_LINUXDEPLOY}" --appdir "${_single_dir}/AppDir" --output appimage
    COMMAND ${CMAKE_COMMAND} -E copy "${_single_dir}/AppDir/usr/share/doc/opad/THIRD-PARTY-NOTICES.txt" "${_single_dir}"
    WORKING_DIRECTORY "${_single_dir}"
    DEPENDS opad
    COMMENT "AppImage in ${_single_dir}" VERBATIM)
endif()
