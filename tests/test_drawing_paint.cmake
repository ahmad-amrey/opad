# PDF and PNG of drawings; the painter makes its own (offscreen) Qt application, as in opad-cli. No painter (neither the
# CLI nor the app is built): the test is not built.
if(TARGET opad_paint)
  target_link_libraries(${target} PRIVATE opad_paint)
  if(OPAD_STATIC)
    qt_import_plugins(${target} INCLUDE_BY_TYPE platforms Qt6::QOffscreenIntegrationPlugin Qt6::QWindowsIntegrationPlugin EXCLUDE_BY_TYPE imageformats iconengines)
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "OPAD_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache" TIMEOUT 60)
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
