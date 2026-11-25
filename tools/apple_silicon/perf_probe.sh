#!/bin/zsh
# Measure the private Apple Silicon guest with RECOMP_PERF=1.
#
#   perf_probe.sh --guest <private-build>/mechassault2_recomp_guest \
#                 --game-dir <validated-extracted-directory> \
#                 --out <empty-or-new-directory> [--seconds 60] \
#                 [--key-at 25] [--key-at 30] [--sample 10] [--startup-limit 1200]
#
# Enables MA2_PRIVATE_DIAGNOSTICS=1: the runtime log it writes holds retail
# paths and generated symbols, so keep --out private and never share it.
# Runs the guest under an isolated HOME inside --out so saves and logs never
# touch the real profile. After the first [PERF] line (the game is drawing)
# it optionally taps Return at the given offsets (System Events; needs
# Accessibility permission for the terminal), records --seconds of play,
# optionally runs `sample` for --sample seconds, then stops the guest and
# writes summary.txt. Nothing here names a private path: pass them in.
set -u
zmodload zsh/datetime

guest="" game_dir="" out="" seconds=60 sample_secs=0 startup_limit=1200
typeset -a key_at
while (( $# )); do
    case $1 in
        --guest) guest=$2; shift 2 ;;
        --game-dir) game_dir=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --seconds) seconds=$2; shift 2 ;;
        --key-at) key_at+=($2); shift 2 ;;
        --sample) sample_secs=$2; shift 2 ;;
        --startup-limit) startup_limit=$2; shift 2 ;;
        -h|--help) sed -n 2,15p "$0"; exit 0 ;;
        *) print -u2 "unknown argument: $1"; exit 2 ;;
    esac
done
[[ -x $guest && -d $game_dir && -n $out ]] || { print -u2 "need --guest, --game-dir and --out"; exit 2; }

mkdir -p "$out/home" || exit 1
out=${out:A}
log="$out/home/Library/Logs/MechAssault2/runtime-private.log"
summary="$out/summary.txt"

# Record what else is running: contention made an earlier startup take 17 min.
top -l 2 -s 1 -o cpu -n 8 -stats pid,command,cpu > "$out/load-before.txt" 2>&1
print "low power mode: $(pmset -g | awk '/lowpowermode/ {print $2}')" >> "$out/load-before.txt"

start=$EPOCHSECONDS
HOME="$out/home" MA2_GAME_DIR="$game_dir" MA2_PRIVATE_DIAGNOSTICS=1 RECOMP_PERF=1 \
    "$guest" --safe-start > "$out/stdout.log" 2>&1 &
pid=$!
print "guest pid $pid"

stop_guest() {
    kill -TERM $pid 2>/dev/null
    for i in {1..20}; do kill -0 $pid 2>/dev/null || return; sleep 0.5; done
    kill -KILL $pid 2>/dev/null
}
trap 'stop_guest; exit 130' INT TERM

# Wait for drawing to start.
first_perf=""
while kill -0 $pid 2>/dev/null; do
    if [[ -f $log ]] && grep -q '^\[PERF\] [0-9.]* fps' "$log"; then
        first_perf=$EPOCHSECONDS; break
    fi
    # The runtime log only exists with MA2_PRIVATE_DIAGNOSTICS=1.
    if [[ ! -f $log ]] && (( EPOCHSECONDS - start > 60 )); then
        print "no runtime log after 60s: $log"; break
    fi
    if (( EPOCHSECONDS - start > startup_limit )); then
        print "startup exceeded ${startup_limit}s"; break
    fi
    sleep 1
done
startup=$(( ${first_perf:-$EPOCHSECONDS} - start ))
print "startup ${startup}s"

if [[ -n $first_perf ]]; then
    measure_from=$(grep -c '^\[PERF\] [0-9.]* fps' "$log")
    elapsed=0
    for at in "${key_at[@]}"; do
        (( at > elapsed )) && sleep $(( at - elapsed )) && elapsed=$at
        osascript -e 'tell application "System Events" to key code 36' 2>>"$out/keys.log" \
            && print "Return at +${at}s" >> "$out/keys.log"
    done
    rest=$(( seconds - elapsed ))
    if (( sample_secs > 0 && rest > sample_secs )); then
        sleep $(( rest - sample_secs ))
        sample $pid $sample_secs -file "$out/sample.txt" >/dev/null 2>&1
    else
        (( rest > 0 )) && sleep $rest
    fi
fi
alive=0; kill -0 $pid 2>/dev/null && alive=1
stop_guest
wait $pid 2>/dev/null; exit_status=$?

{
    print "startup_seconds=$startup"
    print "guest_alive_at_end=$alive exit_status=$exit_status"
    print "keys: $(tr '\n' ' ' < "$out/keys.log" 2>/dev/null)"
    if [[ -f $log ]]; then
        # FPS of each [PERF] report after the first $measure_from (warm-up excluded).
        grep '^\[PERF\] [0-9.]* fps' "$log" | tail -n +$(( ${measure_from:-0} + 1 )) |
            awk '{print $2}' | sort -n |
            awk '{v[++n]=$1; s+=$1} END {
                if (!n) { print "fps: no samples"; exit }
                printf "fps: n=%d mean=%.2f min=%.2f median=%.2f max=%.2f\n",
                    n, s/n, v[1], v[int((n+1)/2)], v[n] }'
        print "last metal report:"
        grep '^\[NV2A-METAL\] operations=' "$log" | tail -1
        print "decline samples:"
        grep '^\[NV2A-METAL\] .*decline' "$log" | sort | uniq -c | sort -rn | head -15
        print "last GPU line:"
        grep '^\[PERF\] [0-9.]* fps' "$log" | tail -1
    else
        print "no runtime log at $log"
    fi
} > "$summary"
cat "$summary"
