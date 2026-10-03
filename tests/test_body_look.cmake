# The per-body look compositor (app/BodyLook.cpp, UI-121) on its own; the viewport side is the looks bench.
target_sources(${target} PRIVATE ../app/BodyLook.cpp ../app/BodyLook.hpp)
target_include_directories(${target} PRIVATE ../app)
