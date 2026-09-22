# Target opad-single: the one-file deliverable of each OS, written to build/<preset>/single.
#
#   Windows  OPAD-<version>-windows-x64.exe (+ opad-cli-<version>-windows-x64.exe): the static build's executables,
#            checked to import nothing but Windows' own DLLs (cmake/single_check.cmake). Needs OPAD_STATIC
#            (preset windows-static); nothing to unpack, settings and cache go to <exe dir>/opad-data.
#   Linux    OPAD-<version>-linux-x86_64.AppImage: linuxdeploy with its Qt plugin bundles the app, Qt, OCCT and
#            the other libraries the distro build links (needs `linuxdeploy` and `linuxdeploy-plugin-qt` on PATH,
#            https://github.com/linuxdeploy).
#   macOS    OPAD-<version>-macos.dmg: the app bundle with Qt and the dylibs copied in by macdeployqt.
#
# Included from app/CMakeLists.txt after the opad target exists.

set(_single_dir "${CMAKE_BINARY_DIR}/single")

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
  get_target_property(_single_qmake Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_single_qt_bin "${_single_qmake}" DIRECTORY)
  find_program(OPAD_MACDEPLOYQT NAMES macdeployqt HINTS "${_single_qt_bin}")
  if(NOT OPAD_MACDEPLOYQT)
    message(STATUS "OPAD: macdeployqt not found; the opad-single target is not available")
    return()
  endif()
  add_custom_target(opad-single
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:opad>" "${_single_dir}/OPAD.app"
    COMMAND "${OPAD_MACDEPLOYQT}" "${_single_dir}/OPAD.app" -always-overwrite -dmg
    COMMAND ${CMAKE_COMMAND} -E rename "${_single_dir}/OPAD.dmg" "${_single_dir}/OPAD-${PROJECT_VERSION}-macos.dmg"
    DEPENDS opad
    COMMENT "App bundle + dmg in ${_single_dir}" VERBATIM)

elseif(UNIX)
  find_program(OPAD_LINUXDEPLOY NAMES linuxdeploy linuxdeploy-x86_64.AppImage)
  if(NOT OPAD_LINUXDEPLOY)
    message(STATUS "OPAD: linuxdeploy not found; the opad-single target is not available")
    return()
  endif()
  get_target_property(_single_qmake Qt6::qmake IMPORTED_LOCATION)
  add_custom_target(opad-single
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_single_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/app/res/opad-256.png" "${_single_dir}/opad.png"
    COMMAND ${CMAKE_COMMAND} -E env "OUTPUT=OPAD-${PROJECT_VERSION}-linux-x86_64.AppImage" "QMAKE=${_single_qmake}"
            "${OPAD_LINUXDEPLOY}" --appdir "${_single_dir}/AppDir" --executable "$<TARGET_FILE:opad>"
            --desktop-file "${PROJECT_SOURCE_DIR}/app/res/opad.desktop" --icon-file "${_single_dir}/opad.png"
            --plugin qt --output appimage
    WORKING_DIRECTORY "${_single_dir}"
    DEPENDS opad
    COMMENT "AppImage in ${_single_dir}" VERBATIM)
endif()
