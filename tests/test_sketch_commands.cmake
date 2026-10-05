# The command line's words and coordinate keys are an app header (app/SketchCommands.hpp), with the key rules and the
# tools' steps beside it (InputKeys.hpp, SketchSteps.hpp).
target_include_directories(${target} PRIVATE ../app)
