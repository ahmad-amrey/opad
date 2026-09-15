add_executable(opad-cli main.cpp)
target_link_libraries(opad-cli PRIVATE opad::core)
install(TARGETS opad-cli RUNTIME DESTINATION bin)
