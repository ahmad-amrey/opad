# The test writes its own zips (stored and deflated members): zlib, as the reader does.
target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
