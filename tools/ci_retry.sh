#!/usr/bin/env sh
# Run a command, retrying it when it fails.
#
#   tools/ci_retry.sh pio pkg install -e seeed-reterminal-e1004
#
# CI only. PlatformIO fetches the ESP32 platform, ESP-IDF and the toolchains
# from GitHub release downloads (pioarduino), and those return 504 often enough
# to fail whole runs on main and on PRs with nothing wrong in the tree. A
# failed download leaves nothing half-installed that a second attempt trips
# over, so retrying is safe. Three attempts, 30 s then 60 s apart. A real
# compile error fails all three the same way and the job still goes red.
set -u

attempts=3
delay=30
n=1
while :; do
    "$@" && exit 0
    status=$?
    if [ "$n" -ge "$attempts" ]; then
        echo "ci_retry: '$*' failed $attempts times (last exit $status)" >&2
        exit "$status"
    fi
    echo "ci_retry: '$*' failed (exit $status), attempt $n of $attempts; retrying in ${delay}s" >&2
    sleep "$delay"
    n=$((n + 1))
    delay=$((delay * 2))
done
