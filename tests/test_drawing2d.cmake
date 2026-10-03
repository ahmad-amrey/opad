# 2D drawings in the view (app/Drawing2D.cpp, area drawing2d) on their own: colours, layers, wording; the view side is the
# drawing2d benches.
target_sources(${target} PRIVATE ../app/Drawing2D.cpp ../app/Drawing2D.hpp)
target_include_directories(${target} PRIVATE ../app)
