# Script side of opad-single on Linux and macOS (cmake/single.cmake passes the -D values; lists arrive '|'-separated).
# After linuxdeploy or macdeployqt filled the bundle: the GPL guard (TODO 11 UI-14) over the libraries they copied in,
# then THIRD-PARTY-NOTICES.txt (UI-13, texts inline) from those libraries traced back to the files they were copied from,
# with OPAD's LICENSE, where the app shows it from (opad::third_party_notices): AppDir/usr/share/doc/opad or
# OPAD.app/Contents/Resources.

foreach(_v OPAD_BUNDLE OPAD_SOURCE_DIR)
  if(NOT ${_v})
    message(FATAL_ERROR "bundle_stage.cmake: ${_v} is not set")
  endif()
endforeach()
string(REPLACE "|" ";" _lib_dirs "${OPAD_LIB_DIRS}")
if(OPAD_BUNDLE MATCHES "\\.app$")
  set(_package "macOS app bundle")
  set(_libs_dir "${OPAD_BUNDLE}/Contents/Frameworks")
  set(_plugins_dir "${OPAD_BUNDLE}/Contents/PlugIns")
  set(_doc "${OPAD_BUNDLE}/Contents/Resources")
else()
  set(_package "AppImage")
  set(_libs_dir "${OPAD_BUNDLE}/usr/lib")
  set(_plugins_dir "${OPAD_BUNDLE}/usr/plugins")
  set(_doc "${OPAD_BUNDLE}/usr/share/doc/opad")
endif()
file(GLOB _libs LIST_DIRECTORIES true "${_libs_dir}/*")  # shared libraries and (macOS) frameworks
file(GLOB_RECURSE _plugins RELATIVE "${_plugins_dir}" "${_plugins_dir}/*.so" "${_plugins_dir}/*.dylib")

include("${OPAD_SOURCE_DIR}/cmake/gpl_guard.cmake")
opad_gpl_guard("${_package}" "${OPAD_BUNDLE}" ${_libs} ${_plugins})

# Each bundled library back to its original (Qt's lib and plugin folders, the prefix and linker folders), else the copy
# itself, which the notices list by name.
string(REPLACE "|" ";" _files "${OPAD_NOTICES_HEADERS}")
foreach(_lib IN LISTS _libs)
  get_filename_component(_name "${_lib}" NAME)
  set(_rel "${_name}")
  set(_found "${_lib}")
  if(_name MATCHES "^(.+)\\.framework$")  # the framework's binary
    set(_rel "${_name}/${CMAKE_MATCH_1}")
    set(_found "${_lib}/${CMAKE_MATCH_1}")
  elseif(IS_DIRECTORY "${_lib}")
    continue()
  endif()
  foreach(_dir IN LISTS OPAD_QT_LIBS _lib_dirs)
    if(EXISTS "${_dir}/${_rel}")
      set(_found "${_dir}/${_rel}")
      break()
    endif()
  endforeach()
  list(APPEND _files "${_found}")
endforeach()
foreach(_rel IN LISTS _plugins)
  if(OPAD_QT_PLUGINS AND EXISTS "${OPAD_QT_PLUGINS}/${_rel}")
    list(APPEND _files "${OPAD_QT_PLUGINS}/${_rel}")
  else()
    list(APPEND _files "${_plugins_dir}/${_rel}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${_doc}")
configure_file("${OPAD_SOURCE_DIR}/LICENSE" "${_doc}/LICENSE" COPYONLY)
string(REPLACE ";" "|" NOTICES_FILES "${_files}")
set(NOTICES_OUT "${_doc}/THIRD-PARTY-NOTICES.txt")
set(NOTICES_OWN "")  # the bundle lies in the build folder: its copies are listed, not skipped
set(NOTICES_VERSION "${OPAD_VERSION}")
set(NOTICES_EXTRA "${OPAD_SOURCE_DIR}/cmake/notices_extra.txt")
string(CONCAT NOTICES_SCOPE "This ${_package} ships the libraries listed below beside OPAD. The LGPL libraries among them "
  "(Qt, Open CASCADE and others) are separate shared libraries that you may replace with compatible builds of your own.")
include("${OPAD_SOURCE_DIR}/cmake/notices.cmake")
