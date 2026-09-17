# Resolve the OCCT toolkits OPAD needs. OCCT >= 7.8 merged the STEP toolkits into TKDESTEP;
# older releases split them. We accept both so vcpkg, MSYS2, Homebrew and apt builds all work.
set(_opad_occt_core
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO
  TKShHealing TKMesh TKXSBase TKLCAF TKCDF TKCAF TKXCAF TKVCAF TKBinL TKBin TKBinXCAF)
set(_opad_occt_step_new TKDESTEP)
set(_opad_occt_step_old TKSTEP TKSTEPBase TKSTEP209 TKSTEPAttr TKXDESTEP)

set(OPAD_OCCT_LIBS "")
foreach(_lib IN LISTS _opad_occt_core)
  if(TARGET ${_lib})
    list(APPEND OPAD_OCCT_LIBS ${_lib})
  endif()
endforeach()
if(TARGET TKDESTEP)
  list(APPEND OPAD_OCCT_LIBS TKDESTEP)
else()
  foreach(_lib IN LISTS _opad_occt_step_old)
    if(TARGET ${_lib})
      list(APPEND OPAD_OCCT_LIBS ${_lib})
    endif()
  endforeach()
endif()
if(TARGET TKDEGLTF)
  list(APPEND OPAD_OCCT_LIBS TKDEGLTF)
  set(OPAD_HAVE_GLTF ON)
elseif(TARGET TKRWMesh)
  list(APPEND OPAD_OCCT_LIBS TKRWMesh)
  set(OPAD_HAVE_GLTF ON)
else()
  set(OPAD_HAVE_GLTF OFF)
endif()

# Visualisation toolkits, only needed by the Qt app.
set(OPAD_OCCT_VIS_LIBS "")
foreach(_lib IN ITEMS TKService TKV3d TKOpenGl)
  if(TARGET ${_lib})
    list(APPEND OPAD_OCCT_VIS_LIBS ${_lib})
  endif()
endforeach()

message(STATUS "OPAD: OCCT ${OpenCASCADE_VERSION} toolkits: ${OPAD_OCCT_LIBS}")
message(STATUS "OPAD: glTF export: ${OPAD_HAVE_GLTF}")
