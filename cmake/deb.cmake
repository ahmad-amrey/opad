# Target opad-deb: the Linux installer, build/<preset>/single/OPAD-<version>-linux-<arch>.deb (Ubuntu and Debian:
# `sudo apt install ./OPAD-…deb`, or a double-click in the software centre). cmake/deb_stage.cmake lays it out:
#
#   /usr/lib/opad/        opad, opad-cli and the OCCT shared libraries built in this tree (RUNPATH $ORIGIN)
#   /usr/bin/opad, opad-cli   links to them (OPAD finds what lies beside its real path)
#   /usr/share/applications/opad.desktop, icons/hicolor/256x256/apps/opad.png, mime/packages/opad.xml (.opad files)
#   /usr/share/doc/opad/  LICENSE and THIRD-PARTY-NOTICES.txt
#
# Qt and the other system libraries are not copied: dpkg-shlibdeps turns what the programs link into the package's
# Depends, so apt installs them (the release targets Ubuntu 24.04 and newer, where it is built). Needs dpkg-dev
# (dpkg-shlibdeps, dpkg-deb); patchelf too for the libraries' RUNPATH (CI has it; without it CMake's file(RPATH_SET) sets
# the programs' and the libraries rely on the programs having loaded them). LibreDWG's converters are left out (GPLv3, as in the AppImage): DWG is read
# through dwg2dxf on PATH (Suggests: libredwg-tools) or the ODA File Converter. Included from app/CMakeLists.txt.

find_program(OPAD_DPKG_DEB dpkg-deb)
find_program(OPAD_DPKG_SHLIBDEPS dpkg-shlibdeps)
find_program(OPAD_PATCHELF patchelf)
if(NOT OPAD_DPKG_DEB OR NOT OPAD_DPKG_SHLIBDEPS)
  message(STATUS "OPAD: dpkg-deb or dpkg-shlibdeps not found (apt install dpkg-dev); no opad-deb target")
  return()
endif()
if(NOT OPAD_PATCHELF)
  set(OPAD_PATCHELF "")
endif()
execute_process(COMMAND dpkg --print-architecture OUTPUT_VARIABLE _deb_arch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _deb_arch)
  set(_deb_arch amd64)
endif()
set(_deb_occt "")
if(OPAD_OCCT_BUNDLED)
  set(_deb_occt "${OPAD_OCCT_INSTALL_DIR}/lib")
endif()
set(_deb_out "${CMAKE_BINARY_DIR}/single/OPAD-${PROJECT_VERSION}-linux-${_deb_arch}.deb")
add_custom_target(opad-deb
  COMMAND ${CMAKE_COMMAND}
          "-DOPAD_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DOPAD_VERSION=${PROJECT_VERSION}" "-DOPAD_ARCH=${_deb_arch}"
          "-DOPAD_APP=$<TARGET_FILE:opad>" "-DOPAD_CLI=$<TARGET_FILE:opad-cli>" "-DOPAD_OCCT_LIBS=${_deb_occt}"
          "-DOPAD_NOTICES=${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" "-DOPAD_ROOT=${CMAKE_BINARY_DIR}/deb-root"
          "-DOPAD_OUT=${_deb_out}" "-DOPAD_DPKG_DEB=${OPAD_DPKG_DEB}" "-DOPAD_DPKG_SHLIBDEPS=${OPAD_DPKG_SHLIBDEPS}"
          "-DOPAD_PATCHELF=${OPAD_PATCHELF}"
          -P "${PROJECT_SOURCE_DIR}/cmake/deb_stage.cmake"
  DEPENDS opad opad-cli
  COMMENT "Debian package ${_deb_out}" VERBATIM)
