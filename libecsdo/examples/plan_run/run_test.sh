#!/usr/bin/env bash
# Plug-and-play test loop: wait for the bus, probe it, make PDO mapping plans,
# apply them, go to OP and run a small test per device (motor move / LED
# walk) until Ctrl+C. When the cable is pulled or the devices change,
# plan_run winds down and exits 3; the loop then waits for the bus again and
# starts over with whatever is plugged in.
#
#   libecsdo/examples/plan_run/run_test.sh [--mode csp|csv|cst] [plan_run options]
#
# Default mode is csp. Run from anywhere; paths are relative to the
# ecsdo_ws root. Needs the IgH master running (not reserved). Ctrl+C stops.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"

MODE=csp
if [[ "${1:-}" == "--mode" ]]; then
    MODE="$2"
    shift 2
fi

SUDO=""
[[ $EUID -ne 0 ]] && SUDO=sudo
RC_BUS_CHANGED=3

STOP=0
trap 'STOP=1' INT TERM

# Slaves on the bus without the state column (position, alias, name).
bus_ids() {
    ethercat slaves 2>/dev/null | awk '{ $3 = ""; $4 = ""; print }'
}

# Wait until slaves are present and the list has not changed for 2 s
# (the master rescans after a cable is plugged in).
wait_bus() {
    local last="" cur same=0

    echo "== waiting for EtherCAT devices (plug in the cable, Ctrl+C to quit)"
    while (( ! STOP )); do
        cur="$(bus_ids)"
        if [[ -n "$cur" && "$cur" == "$last" ]]; then
            (( ++same >= 4 )) && return 0
        else
            same=0
        fi
        last="$cur"
        sleep 0.5
    done
    return 1
}

# After a failure: wait until the set of devices changes before retrying.
wait_change() {
    local before="$1"

    echo "== waiting for the devices to change (Ctrl+C to quit)"
    while (( ! STOP )) && [[ "$(bus_ids)" == "$before" ]]; do
        sleep 0.5
    done
}

while (( ! STOP )); do
    wait_bus || break
    ids="$(bus_ids)"
    echo "$ids"

    echo "== 1. device_probe -> dump/"
    $SUDO rm -rf dump plans llm_out
    if ! $SUDO ./build/device_probe -o dump; then
        (( STOP )) && break
        echo "== device_probe failed"
        wait_change "$ids"
        continue
    fi
    $SUDO chown -R "${SUDO_USER:-$USER}" dump

    echo "== 2. rule_engine (mode $MODE) -> plans/"
    if ! python3 tools/ecat_rules/rule_engine.py dump/*.json --mode "$MODE" \
            --out plans; then
        (( STOP )) && break
        echo "== rule_engine failed"
        wait_change "$ids"
        continue
    fi

    # LLM step only when a plan needs it (plan_run refuses unresolved devices).
    if grep -q '"needs_llm": true' plans/*.plan.json; then
        echo "== 2b. llm_step -> llm_out/ (candidate rules need review, see README)"
        python3 tools/ecat_rules/llm_step.py dump/*.json --out llm_out || true
    fi

    echo "== 3. plan_run (Ctrl+C to stop)"
    $SUDO ./build/examples/plan_run/plan_run "$@" plans/*.plan.json
    rc=$?
    (( STOP )) && break
    case $rc in
        "$RC_BUS_CHANGED")
            echo "== devices changed, starting over" ;;
        0)
            echo "== test finished"
            break ;;                        # only with -t
        *)
            echo "== plan_run failed (exit $rc)"
            wait_change "$ids" ;;
    esac
done
echo "== stopped"
