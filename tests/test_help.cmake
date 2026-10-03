# UI-106/107: command help records (English + Arabic) for every registered command, the rich hover card, the clips, the
# reference; built offscreen with their app sources. Without the desktop app (no Qt) the test is not built.
if(TARGET opad)
  target_sources(${target} PRIVATE ../app/CommandHelp.cpp ../app/RichTip.cpp ../app/RichTip.hpp ../app/Theme.cpp ../app/Theme.hpp
                 ../app/HelpClip.cpp ../app/HelpClip.hpp ../app/GuidedTool.cpp ../app/GuidedTool.hpp ../app/PanelFooter.cpp ../app/PanelFooter.hpp ../app/HelpReference.cpp ../app/HelpReference.hpp
                 ../app/CommandPalette.cpp ../app/CommandPalette.hpp ../app/HelpWindows.cpp ../app/HelpWindows.hpp
                 ../app/Icons.cpp ../app/I18n.cpp ../app/help.qrc ${OPAD_I18N_QRC})
  set_target_properties(${target} PROPERTIES AUTOMOC ON AUTORCC ON)
  target_include_directories(${target} PRIVATE ../app)
  target_compile_definitions(${target} PRIVATE OPAD_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(${target} PRIVATE Qt6::Widgets Qt6::Test)
  if(OPAD_STATIC)
    qt_import_plugins(${target} INCLUDE Qt6::QOffscreenIntegrationPlugin EXCLUDE_BY_TYPE imageformats iconengines)
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
else()
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_tests_properties(${name} PROPERTIES DISABLED ON)
endif()
