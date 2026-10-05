# Open CASCADE built from its source tarball by OPAD's own configure step, once, into build/<preset>/occt:
# download -> extract -> configure -> build -> install (build/<preset>/occt/install), then cached; delete that install
# folder to build it again. Two users, one recipe, so every platform runs the same kernel:
#   - the static single-file preset (cmake/occt_static.cmake): static toolkits, linked into the executables;
#   - Linux distributions whose OCCT is missing or older than 7.8 (cmake/occt_bundled.cmake): shared libraries the
#     build tree runs from (RPATH) and linuxdeploy bundles.
# Only what OPAD uses is built: six modules, no Draw, DETools, Tcl/Tk, TBB, FFmpeg, OpenVR, FreeImage, Draco, Eigen or
# VTK (about 15 minutes on 16 cores).

set(OPAD_OCCT_VERSION 7.9.2)
string(REPLACE "." "_" _occt_tag "${OPAD_OCCT_VERSION}")
set(OPAD_OCCT_URL "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V${_occt_tag}.tar.gz")
set(OPAD_OCCT_SHA256 3cd080d3fc33ba0c6c157e110afe3e015859524c4694dbb09812ec9d61595639)
set(OPAD_OCCT_ROOT "${CMAKE_BINARY_DIR}/occt")
set(OPAD_OCCT_SOURCE_DIR "${OPAD_OCCT_ROOT}/OCCT-${_occt_tag}")
set(OPAD_OCCT_INSTALL_DIR "${OPAD_OCCT_ROOT}/install")

# opad_occt_from_source(<label> <Static|Shared> [extra OCCT configure arguments...])
# Builds and installs OCCT unless the install is already there, then points OpenCASCADE_DIR at it. <label> prefixes the
# messages (OPAD_STATIC, OPAD_OCCT).
function(opad_occt_from_source label type)
  set(_tarball "${OPAD_OCCT_ROOT}/V${_occt_tag}.tar.gz")
  set(_build "${OPAD_OCCT_ROOT}/build")
  set(_config "${OPAD_OCCT_INSTALL_DIR}/lib/cmake/opencascade/OpenCASCADEConfig.cmake")
  string(TOLOWER "${type}" _kind)
  if(NOT EXISTS "${_config}")
    file(MAKE_DIRECTORY "${OPAD_OCCT_ROOT}")
    if(NOT EXISTS "${_tarball}")
      message(STATUS "${label}: downloading OCCT ${OPAD_OCCT_VERSION} from ${OPAD_OCCT_URL}")
      file(DOWNLOAD "${OPAD_OCCT_URL}" "${_tarball}" EXPECTED_HASH SHA256=${OPAD_OCCT_SHA256}
           SHOW_PROGRESS STATUS _dl)
      list(GET _dl 0 _rc)
      if(NOT _rc EQUAL 0)
        file(REMOVE "${_tarball}")
        message(FATAL_ERROR "${label}: download failed: ${_dl}")
      endif()
    endif()
    if(NOT EXISTS "${OPAD_OCCT_SOURCE_DIR}/CMakeLists.txt")
      message(STATUS "${label}: extracting ${_tarball}")
      file(ARCHIVE_EXTRACT INPUT "${_tarball}" DESTINATION "${OPAD_OCCT_ROOT}")
    endif()

    message(STATUS "${label}: building OCCT ${OPAD_OCCT_VERSION} ${_kind} toolkits in ${_build} (once; this takes a while)")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" -S "${OPAD_OCCT_SOURCE_DIR}" -B "${_build}"
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_INSTALL_PREFIX=${OPAD_OCCT_INSTALL_DIR}" -DINSTALL_DIR_LAYOUT=Unix
        -DBUILD_LIBRARY_TYPE=${type} -DBUILD_USE_PCH=OFF
        -DBUILD_MODULE_FoundationClasses=ON -DBUILD_MODULE_ModelingData=ON -DBUILD_MODULE_ModelingAlgorithms=ON
        -DBUILD_MODULE_Visualization=ON -DBUILD_MODULE_ApplicationFramework=ON -DBUILD_MODULE_DataExchange=ON
        -DBUILD_MODULE_Draw=OFF -DBUILD_MODULE_DETools=OFF -DBUILD_DOC_Overview=OFF -DBUILD_SAMPLES_QT=OFF
        -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_TBB=OFF -DUSE_FFMPEG=OFF -DUSE_OPENVR=OFF -DUSE_FREEIMAGE=OFF -DUSE_DRACO=OFF
        -DUSE_VTK=OFF -DUSE_EIGEN=OFF -DUSE_OPENGL=ON -DUSE_GLES2=OFF -DUSE_D3D=OFF
        -DUSE_FREETYPE=ON
        ${ARGN}
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "${label}: configuring OCCT failed (${_rc})")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --build "${_build}" --target install
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_config}")
      message(FATAL_ERROR "${label}: building OCCT failed (${_rc})")
    endif()
  endif()

  set(OpenCASCADE_DIR "${OPAD_OCCT_INSTALL_DIR}/lib/cmake/opencascade" CACHE PATH "" FORCE)
  message(STATUS "${label}: OCCT ${OPAD_OCCT_VERSION} ${_kind} toolkits from ${OPAD_OCCT_INSTALL_DIR}")
endfunction()
