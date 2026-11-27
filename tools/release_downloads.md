

---

## Downloads

| Platform | File | How to start |
|---|---|---|
| Windows 10/11 (64-bit) | `OPAD-@LABEL@-windows-x64-setup.exe` | Run the installer: OPAD goes to your Start menu (no administrator needed). Updating keeps your settings. |
| Windows, no install | `OPAD-@LABEL@-windows-x64.exe` | One file: run it from any folder. For a portable copy that keeps its settings beside it, put an empty file named `opad.portable` next to it. |
| macOS 15 or later (Apple silicon) | `OPAD-@LABEL@-macos.dmg` | Open the dmg and drag OPAD to Applications. |
| Ubuntu / Debian (24.04 or newer, 64-bit) | `OPAD-@LABEL@-linux-amd64.deb` | Open it in the software centre, or run `sudo apt install ./OPAD-@LABEL@-linux-amd64.deb`. OPAD appears in the applications menu. |
| Other Linux (x86-64) | `OPAD-@LABEL@-linux-x86_64.AppImage` | Make it executable (`chmod +x`), then run it. |

Installing, updating and uninstalling never touch your settings: they live in your user folders, not beside the
program. `opad-cli-@LABEL@-windows-x64.exe` is the command-line tool. On a release, the same packages also come
without the version in their names, for the update check of copies before 0.11.0. `SHA256SUMS.txt` holds the files' checksums; the
`THIRD-PARTY-NOTICES-*.txt` files list the libraries each package carries and their licences.

**First start of an unsigned build.** Windows may show "Windows protected your PC": choose *More info*, then *Run
anyway*. macOS 15 and later say Apple could not verify OPAD: choose *Done*, open *System Settings > Privacy &
Security*, choose *Open Anyway* next to the message about OPAD and confirm (once).
