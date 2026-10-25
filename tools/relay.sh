#!/usr/bin/env bash
# Remote 2DS runs through the dev relay (gc3ds's docs/REMOTE_RUNS.md; the same server and token). N643DS's jobs carry
# the prefix "@n643ds", so only N643DS (core/host3ds/relay.c, GET /next?app=n643ds) takes them and gc3ds never does.
# The 2DS must be in N643DS "Remote runs" (menu, or "auto 1" in sdmc:/n643ds/relay.txt).
#   bash tools/relay.sh run [options]     upload this build if new, queue a job for it, follow its log
#        e.g. relay.sh run --rom "Mario 64" --seconds 120      relay.sh run --tests
#   bash tools/relay.sh queue [options]   the same without following (prints the job id)
#   bash tools/relay.sh log JOB | jobs | status | cancel JOB | push
#   bash tools/relay.sh cmd JOB stop      a live command for the running job (stop, shot)
#   bash tools/relay.sh frames JOB        its pictures into core/out/frames_JOB/
#   bash tools/relay.sh switch PATH       start another .3dsx on the 2DS (e.g. sdmc:/3ds/gc3ds.3dsx)
# Settings: core/host3ds/relay.txt ("url <base>", "token <secret>"; never committed; gc3ds's file works as is).
set -euo pipefail
here="$(cd "$(dirname "$0")/../core" && pwd)"
cfg="$here/host3ds/relay.txt"
[ -f "$cfg" ] || { echo "missing $cfg (copy gc3ds's core/host3ds/relay.txt)" >&2; exit 1; }
url="$(sed -n 's/^url //p' "$cfg" | tr -d '\r')"
token="$(sed -n 's/^token //p' "$cfg" | tr -d '\r')"
build="${RELAY_BUILD:-$here/host3ds/n643ds.3dsx}"
api() { curl -sS --fail-with-body -H "x-relay-token: $token" "$@"; }

push() {
    [ -f "$build" ] || { echo "no $build (bash core/host3ds/build.sh)" >&2; exit 1; }
    local note id
    note="n643ds-$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)$(git -C "$here" diff --quiet 2>/dev/null || echo -dirty)"
    id="$(api -X PUT --data-binary @"$build" -H 'Content-Type: application/octet-stream' "$url/build?note=$note" | sed -n 's/^build //p')"
    mkdir -p "$here/out/elfs"
    [ -n "$id" ] && [ -f "$here/host3ds/n643ds.elf" ] && cp "$here/host3ds/n643ds.elf" "$here/out/elfs/$id.elf"
    echo "$id"
}

post_job() {  # post_job ARGS BUILD
    printf '{"args": %s, "build": "%s"}' "$(printf '@n643ds %s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&"/')" "$2" |
        api -X POST -H 'Content-Type: application/json' --data-binary @- "$url/jobs" | sed -n 's/^job //p'
}

queue() {
    local id
    id="$(push)"
    [ -n "$id" ] || { echo "build upload failed: no job queued" >&2; exit 1; }
    post_job "$*" "$id"
}

follow() {
    local job="$1" out="$here/out/relay_$1.csv" after=-1 status=queued hdr prev page
    mkdir -p "$here/out"
    echo "job $job -> $out" >&2
    hdr="$(mktemp)"
    page="$(mktemp)"
    : > "$out"
    while :; do
        prev="$after"
        if api -D "$hdr" "$url/jobs/$job/log?after=$after" > "$page" 2>/dev/null; then
            cat "$page" >> "$out"
            after="$(sed -n 's/^[Xx]-[Rr]elay-[Ll]ast: *//p' "$hdr" | tr -d '\r')"
            status="$(sed -n 's/^[Xx]-[Rr]elay-[Ss]tatus: *//p' "$hdr" | tr -d '\r')"
            printf '\r%s, %s lines   ' "$status" "$(wc -l < "$out")" >&2
            [ "$after" != "$prev" ] && continue
            case "$status" in done|failed|stopped|cancelled) echo >&2; break;; esac
        fi
        sleep 10
    done
    rm -f "$hdr" "$page"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    push) push ;;
    queue) queue "$@" ;;
    run) job="$(queue "$@")"; follow "$job" ;;
    switch) post_job "--switch $1" "-" ;;
    log) follow "$1" ;;
    jobs) api "$url/jobs" ;;
    status) api "$url/status" ;;
    cancel) api -X POST "$url/jobs/$1/cancel" ;;
    cmd) job="$1"; shift; printf '%s' "$*" | api -X POST -H 'Content-Type: text/plain' --data-binary @- "$url/jobs/$job/cmd" ;;
    # frames JOB: the job's pictures (--shots N, or "cmd JOB shot") into core/out/frames_JOB/ as BMP
    frames) d="$here/out/frames_$1"; mkdir -p "$d"
            for name in $(api "$url/jobs/$1/frames" | tr -d '\r'); do
                [ -f "$d/$name" ] || api -o "$d/$name" "$url/jobs/$1/frame/$name" || rm -f "$d/$name"
                echo "$d/$name"
            done ;;
    *) sed -n '2,11p' "$0"; exit 1 ;;
esac
