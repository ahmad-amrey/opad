# The view's hover and selection roles (app/Highlight.cpp, UI-38) with the theme tokens they come from; the viewport side
# is the gui_benches case highlight. Without the desktop app (no Qt) the test is not built.
if(TARGET opad)
  target_sources(${target} PRIVATE ../app/Highlight.cpp ../app/Highlight.hpp ../app/Icons.cpp ../app/Theme.cpp ../app/Theme.hpp)
  set_target_properties(${target} PROPERTIES AUTOMOC ON)
  target_include_directories(${target} PRIVATE ../app)
  target_link_libraries(${target} PRIVATE Qt6::Widgets)
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
