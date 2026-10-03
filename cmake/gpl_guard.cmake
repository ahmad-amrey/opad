# GPL guard (TODO 11 UI-14, decision D1), shared by the package scripts (portable_stage.cmake, bundle_stage.cmake).
# Open CASCADE's TKService is often built against FFmpeg, FreeImage and OpenVR (MSYS2's, Debian's, Homebrew's), which
# OPAD never uses. Those FFmpeg builds are GPL with the x264/x265/xvid encoders, FreeImage is GPLv2: shipping them makes
# the whole package a GPL work whose every recipient is owed its complete source, and hands out codecs under patent
# pools. So no package carries them unless OPAD_ALLOW_GPL_DLLS says it stays on this machine.

# The file names among ARGN that the guard refuses.
function(opad_gpl_libraries out)
  set(gpl "")
  foreach(f IN LISTS ARGN)
    get_filename_component(name "${f}" NAME)
    string(TOLOWER "${name}" lower)
    if(lower MATCHES "^(lib)?(avcodec|avformat|avutil|avfilter|avdevice|swscale|swresample|postproc|x264|x265|xvidcore|freeimage(plus)?|openvr_api)([-.]|$)")
      list(APPEND gpl "${name}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES gpl)
  set(${out} "${gpl}" PARENT_SCOPE)
endfunction()

# opad_gpl_guard(<package> <folder> <files...>): stops with the reason (removing <folder> and <folder>.zip) when one of
# the files is refused; with OPAD_ALLOW_GPL_DLLS on it warns and writes NOT-FOR-DISTRIBUTION.txt into <folder>.
function(opad_gpl_guard package folder)
  opad_gpl_libraries(gpl ${ARGN})
  if(NOT gpl)
    return()
  endif()
  list(JOIN gpl ", " names)
  string(CONCAT why
    "These come in only because this Open CASCADE (its TKService toolkit) is built against FFmpeg, FreeImage and "
    "OpenVR, and OPAD uses none of them. Such FFmpeg builds are GPL with the GPL x264, x265 and xvid encoders, FreeImage "
    "is GPLv2: handing this package to anyone makes OPAD a GPL work whose every recipient is owed its complete source, "
    "and distributes codecs under patent pools. Build Open CASCADE without them (USE_FFMPEG=OFF USE_FREEIMAGE=OFF "
    "USE_OPENVR=OFF, as cmake/occt_static.cmake does) or, on Windows, ship the single-file build (preset windows-static).")
  if(NOT OPAD_ALLOW_GPL_DLLS)
    file(REMOVE_RECURSE "${folder}")
    file(REMOVE "${folder}.zip")
    message(FATAL_ERROR "${package}: refusing to ship ${names}.\n${why}\n"
                        "For local use that is never distributed, configure with -DOPAD_ALLOW_GPL_DLLS=ON.")
  endif()
  message(WARNING "${package}: ${names} shipped because OPAD_ALLOW_GPL_DLLS is on; NOT FOR DISTRIBUTION.")
  file(WRITE "${folder}/NOT-FOR-DISTRIBUTION.txt"
    "This package was made with OPAD_ALLOW_GPL_DLLS=ON and contains ${names}.\n\n${why}\n\n"
    "Do not give this package to anyone.\n")
endfunction()
