# Script side of the opad-deb target (cmake/deb.cmake passes the -D values): stages the package tree, works out its
# dependencies and builds the .deb.

foreach(_v OPAD_SOURCE_DIR OPAD_VERSION OPAD_ARCH OPAD_APP OPAD_CLI OPAD_ROOT OPAD_OUT OPAD_DPKG_DEB OPAD_DPKG_SHLIBDEPS)
  if(NOT ${_v})
    message(FATAL_ERROR "deb_stage.cmake: ${_v} is not set")
  endif()
endforeach()

set(_lib "${OPAD_ROOT}/usr/lib/opad")
file(REMOVE_RECURSE "${OPAD_ROOT}")
foreach(_d usr/lib/opad usr/bin usr/share/applications usr/share/icons/hicolor/256x256/apps usr/share/mime/packages
           usr/share/doc/opad DEBIAN)
  file(MAKE_DIRECTORY "${OPAD_ROOT}/${_d}")
endforeach()

# The programs, and the OCCT libraries built in this tree beside them (symbolic links kept as links).
file(COPY "${OPAD_APP}" "${OPAD_CLI}" DESTINATION "${_lib}")
set(_libs "")
if(OPAD_OCCT_LIBS)
  file(GLOB _occt LIST_DIRECTORIES false "${OPAD_OCCT_LIBS}/*.so*")
  file(COPY ${_occt} DESTINATION "${_lib}")
  file(GLOB _libs LIST_DIRECTORIES false "${_lib}/*.so*")
endif()
include("${OPAD_SOURCE_DIR}/cmake/gpl_guard.cmake")
opad_gpl_guard("Debian package" "${OPAD_ROOT}" ${_libs})

# Found beside themselves wherever they are installed: RUNPATH $ORIGIN, the build tree's paths dropped.
get_filename_component(_app "${OPAD_APP}" NAME)
get_filename_component(_cli "${OPAD_CLI}" NAME)
if(OPAD_PATCHELF)
  foreach(_f "${_lib}/${_app}" "${_lib}/${_cli}" ${_libs})
    if(IS_SYMLINK "${_f}")
      continue()
    endif()
    execute_process(COMMAND "${OPAD_PATCHELF}" --set-rpath "$ORIGIN" "${_f}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "patchelf failed on ${_f}: ${_err}")
    endif()
  endforeach()
else()  # the programs only (their build RUNPATH has room); every OCCT library they need they load themselves
  foreach(_f "${_lib}/${_app}" "${_lib}/${_cli}")
    file(RPATH_SET FILE "${_f}" NEW_RPATH "$ORIGIN")
  endforeach()
endif()
file(CREATE_LINK "../lib/opad/${_app}" "${OPAD_ROOT}/usr/bin/opad" SYMBOLIC)
file(CREATE_LINK "../lib/opad/${_cli}" "${OPAD_ROOT}/usr/bin/opad-cli" SYMBOLIC)

# Menu entry, icon, the .opad file type, licences.
configure_file("${OPAD_SOURCE_DIR}/app/res/opad.desktop" "${OPAD_ROOT}/usr/share/applications/opad.desktop" COPYONLY)
configure_file("${OPAD_SOURCE_DIR}/app/res/opad-256.png" "${OPAD_ROOT}/usr/share/icons/hicolor/256x256/apps/opad.png" COPYONLY)
configure_file("${OPAD_SOURCE_DIR}/app/res/opad-mime.xml" "${OPAD_ROOT}/usr/share/mime/packages/opad.xml" COPYONLY)
configure_file("${OPAD_SOURCE_DIR}/LICENSE" "${OPAD_ROOT}/usr/share/doc/opad/copyright" COPYONLY)
if(OPAD_NOTICES AND EXISTS "${OPAD_NOTICES}")
  configure_file("${OPAD_NOTICES}" "${OPAD_ROOT}/usr/share/doc/opad/THIRD-PARTY-NOTICES.txt" COPYONLY)
endif()

# Depends: what the programs and libraries link outside the package, as dpkg-shlibdeps reads it (it wants a
# debian/control where it runs; the package's own libraries are found through -l).
set(_scratch "${OPAD_ROOT}-shlibdeps")
file(REMOVE_RECURSE "${_scratch}")
file(WRITE "${_scratch}/debian/control" "Source: opad\n\nPackage: opad\nArchitecture: any\n")
set(_scan "${_lib}/${_app}" "${_lib}/${_cli}")
foreach(_f IN LISTS _libs)
  if(NOT IS_SYMLINK "${_f}")
    list(APPEND _scan "${_f}")
  endif()
endforeach()
set(_args "")
foreach(_f IN LISTS _scan)
  list(APPEND _args "-e${_f}")
endforeach()
execute_process(COMMAND "${OPAD_DPKG_SHLIBDEPS}" -O "-l${_lib}" --ignore-missing-info ${_args}
                WORKING_DIRECTORY "${_scratch}" OUTPUT_VARIABLE _shlibs ERROR_VARIABLE _shlibs_err RESULT_VARIABLE _rc)
file(REMOVE_RECURSE "${_scratch}")
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "dpkg-shlibdeps failed: ${_shlibs_err}")
endif()
string(REGEX MATCH "shlibs:Depends=([^\n]*)" _ "${_shlibs}")
set(_depends "${CMAKE_MATCH_1}")
if(NOT _depends)
  message(FATAL_ERROR "dpkg-shlibdeps found no dependencies: ${_shlibs} ${_shlibs_err}")
endif()
# What is loaded at run time and so not seen above: Qt's TLS plugin (in libqt6network6) opens OpenSSL for HTTPS (the
# update check, app/UpdateCheck.hpp). Qt's platform and image plugins come with libqt6gui6, a dependency already.
string(APPEND _depends ", libssl3t64 | libssl3")

# The control file.
file(GLOB_RECURSE _all LIST_DIRECTORIES false "${OPAD_ROOT}/usr/*")
set(_bytes 0)
foreach(_f IN LISTS _all)
  if(NOT IS_SYMLINK "${_f}")
    file(SIZE "${_f}" _s)
    math(EXPR _bytes "${_bytes} + ${_s}")
  endif()
endforeach()
math(EXPR _kib "(${_bytes} + 1023) / 1024")
file(WRITE "${OPAD_ROOT}/DEBIAN/control"
"Package: opad
Version: ${OPAD_VERSION}
Architecture: ${OPAD_ARCH}
Maintainer: OPAD <https://github.com/ahmad-amrey/opad/issues>
Installed-Size: ${_kib}
Depends: ${_depends}
Suggests: libredwg-tools
Section: graphics
Priority: optional
Homepage: https://github.com/ahmad-amrey/opad
Description: Open-source parametric CAD whose files git can diff and merge
 OPAD opens 2D drawings, 3D models and meshes (STEP, IGES, Parasolid, STL, 3MF,
 OBJ, glTF, DXF, DWG, SVG, KiCad), takes a 2D sketch to a solid with an editable
 history, and keeps the whole design in one plain-text file that git can diff and
 merge. AI agents can work on the model in the same window, and every command
 also runs from the command line (opad-cli).
")

get_filename_component(_out_dir "${OPAD_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
execute_process(COMMAND "${OPAD_DPKG_DEB}" --root-owner-group -Zxz --build "${OPAD_ROOT}" "${OPAD_OUT}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "dpkg-deb failed: ${_out}${_err}")
endif()
message(STATUS "Debian package ${OPAD_OUT} (depends on: ${_depends})")
