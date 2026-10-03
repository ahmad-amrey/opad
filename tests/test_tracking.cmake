# The view tracker's pure parts (app/Tracking.cpp, UI-32 cross lock) on their own; the viewport side is the crosslock bench.
target_sources(${target} PRIVATE ../app/Tracking.cpp ../app/Tracking.hpp)
target_include_directories(${target} PRIVATE ../app)
