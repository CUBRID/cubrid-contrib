#!/usr/bin/env bash
set -o errexit -o nounset -o pipefail

show_help() {
  cat <<EOF
Usage: $(basename "$0") <source_file>

Formats a single source file based on its extension.

  .c, .h, .i        -> indent
  .cpp, .hpp, .ipp  -> astyle
  .java             -> google-java-format

Options:
  -h, --help        Show this help.
EOF
}

if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
  show_help
  exit 0
fi

f=$1
[[ -f "$f" ]] || { echo "error: not a file: $f" >&2; exit 2; }

case "$f" in
  *.c|*.h|*.i)
    command -v indent >/dev/null || { echo "error: indent not found" >&2; exit 127; }
    exec indent -l120 -lc120 "$f"
    ;;

  *.cpp|*.hpp|*.ipp)
    command -v astyle >/dev/null || { echo "error: astyle not found" >&2; exit 127; }
    # avoid backup suffixes; format in place
    exec astyle \
      --style=gnu --mode=c \
      --indent-namespaces --indent=spaces=2 \
      -xT8 -xt4 -j \
      --max-code-length=120 \
      --align-pointer=name --indent-classes \
      --pad-header --pad-first-paren-out \
      --suffix=none "$f"
    ;;

  *.java)
    [[ -f .github/workflows/google-java-format-1.7-all-deps.jar ]] || {
      echo "error: google-java-format jar not found" >&2; exit 127;
    }
    # google-java-format: use --aosp (if desired) and -i to write in place
    # Remove --aosp if you don't want AOSP style.
    exec java -jar .github/workflows/google-java-format-1.7-all-deps.jar --aosp -i "$f"
    ;;

  *)
    echo "error: unsupported extension for: $f" >&2
    exit 3
    ;;
esac

