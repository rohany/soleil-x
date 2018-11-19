#!/bin/bash -eu

if [[ ! -z "${PBS_JOBID:-}" ]]; then
    JOBID="$PBS_JOBID"
elif [[ ! -z "${SLURM_JOBID:-}" ]]; then
    JOBID="$SLURM_JOBID"
elif [[ ! -z "${LSB_JOBID:-}" ]]; then
    JOBID="$LSB_JOBID"
else
    JOBID="$(date +%s)"
fi

# Create a directory on the scratch filesystem, for output (if SCRATCH is
# defined, and the user hasn't specified an output directory explicitly).
OUT_DIR=
OUT_DIR_FOLLOWS=false
for ARG in $ARGS; do
    if [[ "$OUT_DIR_FOLLOWS" == true ]]; then
        OUT_DIR="$ARG"
        break
    elif [[ "$ARG" == "-o" ]]; then
        OUT_DIR_FOLLOWS=true
    fi
done
if [[ -z "$OUT_DIR" ]]; then
    if [[ ! -z "${SCRATCH:-}" ]]; then
        OUT_DIR="$SCRATCH"/"$JOBID"
        mkdir "$OUT_DIR"
        ARGS="$ARGS -o $OUT_DIR"
        echo "Redirecting output to $OUT_DIR"
    else
        OUT_DIR=.
    fi
fi

# Add profiling flags
if [[ "$PROFILED_RANKS" == "ALL" ]]; then
    PROFILED_RANKS="$NUM_RANKS"
fi
if (( "$PROFILED_RANKS" > 0 )); then
    ARGS="$ARGS -lg:prof $PROFILED_RANKS -lg:prof_logfile $OUT_DIR/prof_%.log"
fi
