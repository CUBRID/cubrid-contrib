#!/usr/bin/env bash
set -o errexit -o nounset -o pipefail

f=$1
[[ -f "$f" ]] || { echo "error: not a file: $f" >&2; exit 2; }

case "$f" in
  *.c|*.h|*.i)
    # install indent <= 2.2.11
    command -v indent >/dev/null || { echo "error: indent not found" >&2; exit 127; }
    exec indent -l120 -lc120 -st
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
      --suffix=none
    ;;

  *.java)
    [[ -f .github/workflows/google-java-format-1.7-all-deps.jar ]] || {
      echo "error: google-java-format jar not found" >&2; exit 127;
    }
    # google-java-format: use --aosp (if desired) and -i to write in place
    # Remove --aosp if you don't want AOSP style.
    exec java -jar .github/workflows/google-java-format-1.7-all-deps.jar --aosp
    ;;

  *)
    echo "error: unsupported extension for: $f" >&2
    exit 3
    ;;
esac

