#!/bin/bash -eu

###############################################################################
# Helper functions
###############################################################################

function quit {
    echo "$1" >&2
    exit 1
}

###############################################################################
# Derived options
###############################################################################

# Compute unique job ID
if [[ ! -z "${PBS_JOBID:-}" ]]; then
    JOBID="$PBS_JOBID"
elif [[ ! -z "${SLURM_JOBID:-}" ]]; then
    JOBID="$SLURM_JOBID"
elif [[ ! -z "${LSB_JOBID:-}" ]]; then
    JOBID="$LSB_JOBID"
else
    JOBID="$(date +%s)"
fi

# Create a directory on the scratch filesystem, for output (if $SCRATCH is
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
    else
        OUT_DIR=.
    fi
fi
if [[ "$OUT_DIR" != "${OUT_DIR%[[:space:]]*}" ]]; then
    quit "Cannot handle spaces in output directory"
fi
echo "Sending output to $OUT_DIR"

# Prepare Legion configuration
CORES_PER_RANK=$(( CORES_PER_NODE / RANKS_PER_NODE ))
RAM_PER_RANK=$(( RAM_PER_NODE / RANKS_PER_NODE ))
THREADS_PER_RANK=$(( CORES_PER_RANK - RESERVED_CORES ))
if (( CORES_PER_NODE < RANKS_PER_NODE ||
      CORES_PER_NODE % RANKS_PER_NODE != 0 ||
      RESERVED_CORES >= CORES_PER_RANK )); then
    quit "Cannot split $CORES_PER_NODE core(s) into $RANKS_PER_NODE rank(s)"
fi
if [[ "$USE_CUDA" == 1 ]]; then
    GPUS_PER_RANK=$(( GPUS_PER_NODE / RANKS_PER_NODE ))
    if (( GPUS_PER_NODE < RANKS_PER_NODE ||
          GPUS_PER_NODE % RANKS_PER_NODE != 0 )); then
        quit "Cannot split $GPUS_PER_NODE GPU(s) into $RANKS_PER_NODE rank(s)"
    fi
fi

# Add debugging flags
DEBUG_OPTS=
if [[ "$DEBUG" == 1 ]]; then
    DEBUG_OPTS="-ll:force_kthreads"
fi
# Add profiling flags
PROFILER_OPTS=
if [[ "$PROFILE" == 1 ]]; then
    PROFILER_OPTS="-lg:prof $NUM_RANKS -lg:prof_logfile $OUT_DIR/prof_%.log"
fi
# Add CUDA options
GPU_OPTS=
if [[ "$USE_CUDA" == 1 ]]; then
    GPU_OPTS="-ll:gpu $GPUS_PER_RANK -ll:fsize $FB_PER_GPU -ll:zsize 15000 -ll:ib_zsize 15000"
fi
# Add GASNET options
GASNET_OPTS=
if [[ "$LOCAL_RUN" == 0 ]]; then
    GASNET_OPTS="-ll:rsize 0 -ll:ib_rsize 1024 -ll:gsize 0"
fi
if [[ ! -v SOLEIL_POST_ARGS ]]; then
    SOLEIL_POST_ARGS=""
fi
# Synthesize final command
COMMAND="$EXECUTABLE $ARGS \
  -logfile $OUT_DIR/%.log $DEBUG_OPTS $PROFILER_OPTS \
  -ll:cpu 0 -ll:ocpu 6 -ll:onuma 0 -ll:okindhack -ll:othr $THREADS_PER_RANK \
  $GPU_OPTS \
  -ll:util 4 -ll:ahandlers 4 -ll:io 1 -ll:dma 2 \
  -ll:csize $RAM_PER_RANK \
  $GASNET_OPTS \
  -ll:stacksize 8 -ll:ostack 8 -lg:sched -1 -lg:hysteresis 0 \
  $SOLEIL_POST_ARGS"
echo "Invoking Legion on $NUM_RANKS rank(s), $NUM_NODES node(s) ($RANKS_PER_NODE rank(s) per node), as follows:"
echo $COMMAND
