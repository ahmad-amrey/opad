# 2D drawings in the view (app/Drawing2D.cpp, area drawing2d) on their own: colours, layers, wording, drawing coordinates,
# what a plot draws and where (app/Plot2D.cpp); the view side is the drawing2d benches.
target_sources(${target} PRIVATE ../app/Drawing2D.cpp ../app/Drawing2D.hpp ../app/Plot2D.cpp ../app/Plot2D.hpp)
target_include_directories(${target} PRIVATE ../app)
