# The sketch tools' steps are an app header (app/SketchSteps.hpp); the test reads the app sources beside it.
target_include_directories(${target} PRIVATE ../app)
# Where they are: __FILE__ is relative when ccache rewrites paths under its base_dir (then wrong from the test's folder).
target_compile_definitions(${target} PRIVATE OPAD_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
