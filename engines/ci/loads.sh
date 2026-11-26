# Sourced by engines/<id>/smoke.sh: a pack loads nothing but its own files and the system's, so it runs on a computer
# without Homebrew, without the build tree and without the distribution's development packages.
#
#   check_links <pack dir>            every program and library in bin/ and lib/: what it names and where it looks
#   check_loaded <pack dir> <cmd...>  runs the command and checks every library the loader actually loaded (macOS:
#                                     DYLD_PRINT_LIBRARIES; Linux: LD_DEBUG=files)
# Both fail (exit 1) naming each offender. On Windows (MSYS / Git Bash) they do nothing: a program there looks beside
# itself first, and the recipe checks the DLL imports when it builds.

loads_system() {  # path: one of the system's own libraries
  case "$(uname -s)" in
    Darwin) case "$1" in /usr/lib/* | /System/*) return 0 ;; esac ;;
    Linux) case "$1" in /lib/* | /lib64/* | /usr/lib/* | /usr/lib64/*) return 0 ;; esac ;;
  esac
  return 1
}

check_links() {
  local pack bad=0 f r dep
  pack=$(cd "$1" && pwd -P)
  case "$(uname -s)" in
    Linux)
      while IFS= read -r f; do
        readelf -h "$f" >/dev/null 2>&1 || continue  # not ELF
        readelf -d "$f" 2>/dev/null | grep '(NEEDED)' >/dev/null || continue  # static
        for r in $(readelf -d "$f" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p' | tr ':' ' '); do
          case "$r" in '$ORIGIN'*) ;; *) echo "error: ${f#"$pack"/} looks for libraries in $r" >&2; bad=1 ;; esac
        done
        while IFS= read -r line; do
          case "$line" in
            *"not found"*) echo "error: ${f#"$pack"/}: $line" >&2; bad=1 ;;
            *" => /"*)
              dep=${line#*=> }
              dep=${dep%% (*}
              dep=$(realpath "$dep")
              [[ "$dep" == "$pack"/* ]] || loads_system "$dep" || { echo "error: ${f#"$pack"/} loads $dep" >&2; bad=1; }
              ;;
          esac
        done < <(env -u LD_LIBRARY_PATH ldd "$f" 2>/dev/null)
      done < <(find "$pack/bin" "$pack/lib" -type f 2>/dev/null)
      ;;
    Darwin)
      while IFS= read -r f; do
        otool -h "$f" >/dev/null 2>&1 || continue
        otool -hv "$f" 2>/dev/null | grep -E 'EXECUTE|DYLIB' >/dev/null || continue
        for r in $(otool -l "$f" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }'); do
          case "$r" in @*) ;; *) echo "error: ${f#"$pack"/} looks for libraries in $r" >&2; bad=1 ;; esac
        done
        for dep in $(otool -L "$f" | awk 'NR > 1 {print $1}'); do
          case "$dep" in @rpath/* | @loader_path/* | @executable_path/*) ;; *)
            loads_system "$dep" || { echo "error: ${f#"$pack"/} names $dep" >&2; bad=1; } ;;
          esac
        done
        codesign --verify "$f" 2>/dev/null || { echo "error: ${f#"$pack"/} is not signed" >&2; bad=1; }
      done < <(find "$pack/bin" "$pack/lib" -type f 2>/dev/null)
      ;;
    *) return 0 ;;
  esac
  [ "$bad" = 0 ] || return 1
  echo "links: every program and library in $(basename "$pack") names only its own files and the system's"
}

check_loaded() {
  local pack trace bad=0 dep
  pack=$(cd "$1" && pwd -P)
  shift
  if [ "$(uname -s)" = Linux ] && ! readelf -d "$1" 2>/dev/null | grep '(NEEDED)' >/dev/null; then
    echo "loaded: $(basename "$1") is static: nothing to load"
    return 0
  fi
  trace=$(mktemp)
  case "$(uname -s)" in
    Darwin)  # "dyld[pid]: <uuid> /path" (or "dyld: loaded: /path" on older systems): the path is the last field
      env DYLD_PRINT_LIBRARIES=1 "$@" >/dev/null 2>"$trace" || true
      awk '/^dyld/ && $NF ~ /^\// { print $NF }' "$trace" >"$trace.paths" ;;
    Linux)  # "pid: calling init: /path" for every object loaded
      env LD_DEBUG=libs LD_DEBUG_OUTPUT="$trace" "$@" >/dev/null 2>&1 || true
      cat "$trace".* 2>/dev/null | sed -n 's/.*calling init: \(\/.*\)$/\1/p' >"$trace.paths" || true ;;
    *) rm -f "$trace"; return 0 ;;
  esac
  [ -s "$trace.paths" ] || { echo "error: the loader reported nothing for $1 (DYLD_PRINT_LIBRARIES / LD_DEBUG)" >&2; rm -f "$trace"*; return 1; }
  while IFS= read -r dep; do
    [ -e "$dep" ] && dep=$(realpath "$dep")
    [[ "$dep" == "$pack"/* ]] || loads_system "$dep" || { echo "error: $(basename "$1") loaded $dep" >&2; bad=1; }
  done < <(sort -u "$trace.paths")
  echo "loaded: $(basename "$1"): $(sort -u "$trace.paths" | grep -c "^$pack/") of its own files, $(sort -u "$trace.paths" | grep -vc "^$pack/") of the system's"
  rm -f "$trace"*
  [ "$bad" = 0 ]
}
