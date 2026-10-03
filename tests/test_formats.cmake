# The converter the formats test reads "DWG"s with.
add_executable(opad-fake-dwg2dxf fake_dwg2dxf.cpp)
add_dependencies(${target} opad-fake-dwg2dxf)
target_compile_definitions(${target} PRIVATE OPAD_FAKE_DWG2DXF="$<TARGET_FILE:opad-fake-dwg2dxf>")
