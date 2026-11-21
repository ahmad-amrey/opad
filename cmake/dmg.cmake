# The macOS dmg (run by cmake/single.cmake's opad-single target with cmake -P): the usual drag-to-install window, OPAD.app
# beside a link to /Applications.
#
#   -DDMG_STAGE=<folder>        holds OPAD.app; the Applications link is added here, so the folder is the volume as is
#   -DDMG_OUT=<file.dmg>        written compressed (UDZO)
#   -DDMG_VOLNAME=<name>        the volume's name
#   -DDMG_BACKGROUND=<tiff>     the window's picture (app/res/dmg-background.tiff, tools/make_dmg_background.swift)
#   -DDMG_LAYOUT=ON|OFF         try the Finder layout
#   -DDMG_HDIUTIL=<hdiutil>
#
# The layout (icons placed on the picture's arrow, 128 px, no toolbar) is Finder's .DS_Store on the volume, which only
# Finder writes: the image is made read-write, mounted, laid out by AppleScript, then converted. That needs a Finder
# that answers (a logged-in session, the calling program allowed to control Finder) and is cosmetic, so any failure
# there, including a 2 minute timeout, falls back to the plain dmg of the same folder with a warning: never a failed build.

cmake_minimum_required(VERSION 3.21)

file(REMOVE "${DMG_STAGE}/Applications")
file(CREATE_LINK /Applications "${DMG_STAGE}/Applications" SYMBOLIC)

function(dmg_plain)
  execute_process(COMMAND "${DMG_HDIUTIL}" create -volname "${DMG_VOLNAME}" -srcfolder "${DMG_STAGE}" -fs HFS+
                          -format UDZO -imagekey zlib-level=9 -ov "${DMG_OUT}"
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "hdiutil create failed (${rc})")
  endif()
endfunction()

if(NOT DMG_LAYOUT OR NOT EXISTS "${DMG_BACKGROUND}")
  dmg_plain()
  return()
endif()

set(rw "${DMG_OUT}.rw.dmg")
set(device "")
set(mount "")

# Leaves the layout attempt: detaches what it mounted (Finder may still hold the volume for a moment) and drops the
# read-write image. A failure (the reason in `why`) is reported and the plain dmg written instead.
macro(dmg_layout_done)
  if(device)
    foreach(attempt RANGE 5)
      execute_process(COMMAND "${DMG_HDIUTIL}" detach "${device}" RESULT_VARIABLE drc OUTPUT_QUIET ERROR_QUIET)
      if(drc EQUAL 0)
        break()
      endif()
      execute_process(COMMAND sleep 1)
      if(attempt EQUAL 4)
        execute_process(COMMAND "${DMG_HDIUTIL}" detach -force "${device}" OUTPUT_QUIET ERROR_QUIET)
      endif()
    endforeach()
  endif()
  if(NOT "${why}" STREQUAL "")
    file(REMOVE "${rw}")
    message(WARNING "dmg: no Finder layout (${why}); writing the plain drag-to-install dmg")
    dmg_plain()
    return()
  endif()
endmacro()

# Room for the picture and Finder's files on top of the bundle.
execute_process(COMMAND du -sk "${DMG_STAGE}" OUTPUT_VARIABLE du OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "^[0-9]+" kb "${du}")
math(EXPR mb "${kb} / 1024 + 32")
execute_process(COMMAND "${DMG_HDIUTIL}" create -volname "${DMG_VOLNAME}" -srcfolder "${DMG_STAGE}" -fs HFS+
                        -format UDRW -size ${mb}m -ov "${rw}"
                RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  set(why "hdiutil create -format UDRW failed")
  dmg_layout_done()
endif()

# Mounted where Finder sees it (not -nobrowse); the mount point is read back, since another OPAD volume may be mounted
# already ("/Volumes/OPAD 1").
execute_process(COMMAND "${DMG_HDIUTIL}" attach -readwrite -noverify -noautoopen "${rw}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REGEX MATCH "^/dev/disk[0-9]+" device "${out}")
string(REGEX MATCH "\t(/Volumes/[^\n]+)" _ "${out}")
set(mount "${CMAKE_MATCH_1}")
string(STRIP "${mount}" mount)
if(NOT rc EQUAL 0 OR NOT IS_DIRECTORY "${mount}")
  set(why "hdiutil attach failed: ${err}")
  dmg_layout_done()
endif()

file(COPY "${DMG_BACKGROUND}" DESTINATION "${mount}/.background")
get_filename_component(bg "${DMG_BACKGROUND}" NAME)

# Icon centres and window size match the picture (tools/make_dmg_background.swift).
get_filename_component(outdir "${DMG_OUT}" DIRECTORY)
set(script "${outdir}/dmg_layout.applescript")
file(WRITE "${script}" "on run argv
  set mnt to item 1 of argv
  tell application \"Finder\"
    set d to item (POSIX file mnt as alias)
    open d
    set w to container window of d
    set current view of w to icon view
    set toolbar visible of w to false
    set statusbar visible of w to false
    set bounds of w to {200, 120, 840, 520}
    set o to icon view options of w
    set arrangement of o to not arranged
    set icon size of o to 128
    set text size of o to 13
    set background picture of o to file \".background:${bg}\" of d
    set position of item \"OPAD.app\" of d to {170, 190}
    set position of item \"Applications\" of d to {470, 190}
    close w
    open d
    update d without registering applications
    delay 2
    close container window of d
  end tell
end run
")
execute_process(COMMAND osascript "${script}" "${mount}" TIMEOUT 120 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(REMOVE "${script}")
if(NOT rc EQUAL 0)
  string(STRIP "${err}" err)
  set(why "Finder did not answer: ${rc} ${err}")
  dmg_layout_done()
endif()
# Finder writes .DS_Store when it closes the window; wait for it a little.
foreach(attempt RANGE 10)
  if(EXISTS "${mount}/.DS_Store")
    break()
  endif()
  execute_process(COMMAND sleep 1)
endforeach()
if(NOT EXISTS "${mount}/.DS_Store")
  set(why "Finder wrote no .DS_Store")
  dmg_layout_done()
endif()
file(REMOVE_RECURSE "${mount}/.fseventsd" "${mount}/.Trashes")
execute_process(COMMAND sync)
set(why "")
dmg_layout_done()

execute_process(COMMAND "${DMG_HDIUTIL}" convert "${rw}" -format UDZO -imagekey zlib-level=9 -ov -o "${DMG_OUT}"
                RESULT_VARIABLE rc OUTPUT_QUIET)
file(REMOVE "${rw}")
if(NOT rc EQUAL 0)
  message(WARNING "dmg: converting the laid-out image failed (${rc}); writing the plain drag-to-install dmg")
  dmg_plain()
endif()
