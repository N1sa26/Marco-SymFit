#!/bin/bash

set -euo pipefail

# Flags: cleanup is enabled by default; pass --no-force to skip aggressive cleanup.
FORCE_CLEAN=1
for arg in "$@"; do
  if [ "$arg" = "--force" ]; then FORCE_CLEAN=1; fi
  if [ "$arg" = "--no-force" ]; then FORCE_CLEAN=0; fi
done

# Optional: set SUDO=1 in env to use sudo for kill/mkfifo/chmod if needed
SUDO_BIN=""
if [ "${SUDO:-0}" = "1" ]; then SUDO_BIN="sudo"; fi

# Always cleanup/prepare FIFOs; process cleanup is optional
echo "[pre] preparing FIFOs"
$SUDO_BIN rm -f /tmp/pcpipe /tmp/myfifo /tmp/wp2 2>/dev/null || true
$SUDO_BIN mkfifo /tmp/pcpipe /tmp/myfifo /tmp/wp2 2>/dev/null || true
$SUDO_BIN chmod 666 /tmp/pcpipe /tmp/myfifo /tmp/wp2 2>/dev/null || true

echo "[pre] process cleanup: FORCE_CLEAN=$FORCE_CLEAN (no sudo unless SUDO=1)"

safe_kill_others() {
  local pattern="$1"
  local me="$$"
  for pid in $(pgrep -f "$pattern" || true); do
    if [ "$pid" != "$me" ]; then
      $SUDO_BIN kill -9 "$pid" || true
    fi
  done
}

if [ "$FORCE_CLEAN" = "1" ]; then
  $SUDO_BIN pkill -9 -f "marco/fastgen/fastgen" || true
  $SUDO_BIN pkill -9 -f "marco/scheduler/main-MS.py" || true
  safe_kill_others "Marco-SymFit/test/run_marco_symfit_integration.sh"
  $SUDO_BIN pkill -9 -f "symqemu-x86_64" || true
  $SUDO_BIN pkill -9 -f "fgtest" || true
fi

# Project paths - All paths are relative to Marco-SymFit root
# Get the script's directory and find Marco-SymFit root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MARCO_SYMFIT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$MARCO_SYMFIT_ROOT/test/workdir"

# Binaries - All paths relative to Marco-SymFit root
SYMFIT_QEMU="$MARCO_SYMFIT_ROOT/symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64"
SYMFIT_FGTEST="$MARCO_SYMFIT_ROOT/symfit/build/symsan/bin/fgtest"
MARCO_SCHEDULER="$MARCO_SYMFIT_ROOT/marco/scheduler/main-MS.py"
MARCO_FASTGEN="$MARCO_SYMFIT_ROOT/marco/fastgen/fastgen"

# For backward compatibility, also set PROJECT_ROOT
PROJECT_ROOT="$(cd "$MARCO_SYMFIT_ROOT/.." && pwd)"

# Working directories
OUTPUT_DIR="$WORK_DIR/output"
INPUT_DIR="$OUTPUT_DIR/afl-slave/queue"
LOG_DIR="$WORK_DIR/logs"
# Marco-compatible: tree directories should be under output directory
TREE0_DIR="$OUTPUT_DIR/tree0"  # For initial seeds (afl-slave/queue)
TREE1_DIR="$OUTPUT_DIR/tree1"  # For generated test cases (fifo/queue)

# Pipe paths
PCPIPE_PATH="/tmp/pcpipe"
SCHEDULER_PIPE_PATH="/tmp/myfifo"

# Target program (use complex_target.c for testing)
TARGET_PROGRAM="$WORK_DIR/targets/complex_target"
TARGET_SOURCE="$WORK_DIR/complex_target.c"

echo "=== Marco-SymFit Integration Test ==="
echo "Project Root: $PROJECT_ROOT"
echo "Work Directory: $WORK_DIR"
echo "Target Program: $TARGET_PROGRAM"
echo ""

# Create required directories
# Marco-compatible: create tree0 (for initial seeds) and tree1 (for generated cases)
mkdir -p "$INPUT_DIR" "$OUTPUT_DIR" "$LOG_DIR" "$TREE0_DIR" "$TREE1_DIR" "$WORK_DIR/fifo"
# FastGen expects $OUTPUT_DIR/tmp for cur_input/forksrv sockets
rm -rf "$OUTPUT_DIR/tmp"
mkdir -p "$OUTPUT_DIR/tmp"
: > "$LOG_DIR/fastgen.log"
: > "$LOG_DIR/scheduler.log"
: > "$LOG_DIR/fastgen_cxx.log"
: > "$LOG_DIR/Gscheduler.log"
: > "$LOG_DIR/pipe_mirror.log"
: > "$LOG_DIR/symfit.log"

# Optional cleanup of previous runs (lightweight, user-level only)
if [ "$FORCE_CLEAN" = "1" ]; then
  pkill -f "symqemu-x86_64" || true
  pkill -f "fgtest" || true
  pkill -f "main.py" || true
  pkill -f "fastgen" || true
fi
sleep 0.3

# Clear tree directories for a clean run (Marco-compatible)
rm -f "$OUTPUT_DIR/tree0"/id:* 2>/dev/null || true
rm -f "$OUTPUT_DIR/tree1"/id:* 2>/dev/null || true

# Create FIFOs (re-ensure)
echo "Creating pipes..."
$SUDO_BIN rm -f "$PCPIPE_PATH" "$SCHEDULER_PIPE_PATH" 2>/dev/null || true
$SUDO_BIN mkfifo "$PCPIPE_PATH" "$SCHEDULER_PIPE_PATH" 2>/dev/null || true
$SUDO_BIN chmod 666 "$PCPIPE_PATH" "$SCHEDULER_PIPE_PATH" 2>/dev/null || true
echo "✓ Pipes created: $PCPIPE_PATH, $SCHEDULER_PIPE_PATH"

# Reset CE queue; keep only a placeholder task
mkdir -p "$OUTPUT_DIR/fifo/queue"
rm -f "$OUTPUT_DIR/fifo/queue"/id:* 2>/dev/null || true

# Ensure input directory exists (seeds should be prepared beforehand)
mkdir -p "$OUTPUT_DIR/afl-slave/queue"
echo "Using seeds from $INPUT_DIR (seeds should be prepared beforehand)"

# Generate 50 initial random seeds only once
SEED_SOURCE_DIR="$WORK_DIR/seed"
SEED_TARGET_DIR="$OUTPUT_DIR/afl-slave/queue"
INITIAL_SEED_COUNT=50
if ls "$SEED_TARGET_DIR"/id:* >/dev/null 2>&1; then
    echo "Seeds already found in $SEED_TARGET_DIR; reusing existing set."
else
    echo "Preparing $INITIAL_SEED_COUNT initial random seeds..."
    for seed_idx in $(seq 0 $((INITIAL_SEED_COUNT - 1))); do
        target_name=$(printf "id:%06d,orig" "$seed_idx")
        target_path="$SEED_TARGET_DIR/$target_name"
        case $seed_idx in
            0) echo -n "A" > "$target_path" ;;
            1) echo -n "B" > "$target_path" ;;
            2) echo -n "C" > "$target_path" ;;
            3) echo -n "D" > "$target_path" ;;
            4) echo -n "A12345" > "$target_path" ;;
            5) echo -n "AXYZ123" > "$target_path" ;;
            6) echo -n "AYZ1234" > "$target_path" ;;
            7) echo -n "B123456" > "$target_path" ;;
            8) echo -n "BX12345" > "$target_path" ;;
            9) echo -n "BY12345" > "$target_path" ;;
            10) echo -n "C123456" > "$target_path" ;;
            11) echo -n "CX12345" > "$target_path" ;;
            12) echo -n "CY12345" > "$target_path" ;;
            13) echo -n "D123456" > "$target_path" ;;
            14) echo -n "TEST" > "$target_path" ;;
            15) echo -n "TEST123" > "$target_path" ;;
            16) echo -n "HELLO" > "$target_path" ;;
            17) echo -n "HELLO123" > "$target_path" ;;
            18) echo -n "WORLD" > "$target_path" ;;
            19) echo -n "WORLD123" > "$target_path" ;;
            20) echo -n "OTHER" > "$target_path" ;;
            21) printf '\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF' > "$target_path" ;;
            22) printf '\x80\x80\x80\x80\x80\x80\x80\x80' > "$target_path" ;;
            23) printf '\x40\x40\x40\x40\x40' > "$target_path" ;;
            24) printf '\x10\x10\x10' > "$target_path" ;;
            25) printf '\x00\x02\x04\x06\x08\x0A\x0C\x0E' > "$target_path" ;;
            26) printf '\x01\x03\x05\x07\x09\x0B\x0D\x0F' > "$target_path" ;;
            27) printf '\x00\x01\x02\x03\x04\x05\x06\x07' > "$target_path" ;;
            28) printf '\x01\x02\x03\x04\x05\x06\x07\x08' > "$target_path" ;;
            29) printf '\x00\x01\x02\x03\x04\x05' > "$target_path" ;;
            30) echo -n "AABBCC" > "$target_path" ;;
            31) echo -n "AABCCC" > "$target_path" ;;
            32) echo -n "ABCDEF" > "$target_path" ;;
            33) echo -n "AABBCCDD" > "$target_path" ;;
            34) echo -n "ATEST123" > "$target_path" ;;
            35) echo -n "BHELLO123" > "$target_path" ;;
            36) echo -n "CWORLD123" > "$target_path" ;;
            37) echo -n "AXTEST123" > "$target_path" ;;
            38) echo -n "AYHELLO123" > "$target_path" ;;
            39) echo -n "BXWORLD123" > "$target_path" ;;
            40) printf '\x00' > "$target_path" ;;
            41) printf '\xFF' > "$target_path" ;;
            42) printf '\x00\x00\x00\x00\x00' > "$target_path" ;;
            43) printf '\xFF\xFF\xFF\xFF\xFF\xFF' > "$target_path" ;;
            44|45|46|47|48|49)
                seed_length=$((RANDOM % 20 + 1))
                if [ $((seed_idx % 2)) -eq 0 ]; then
                    python3 -c "import random, string; print(''.join(random.choices(string.printable, k=$seed_length)), end='')" > "$target_path" 2>/dev/null || \
                    head -c "$seed_length" /dev/urandom | tr -dc '[:print:]' > "$target_path" 2>/dev/null || \
                    echo -n "RANDOM$seed_idx" > "$target_path"
                else
                    head -c "$seed_length" /dev/urandom > "$target_path" 2>/dev/null || \
                    python3 -c "import os; os.write(1, os.urandom($seed_length))" > "$target_path" || \
                    echo -n "RANDOM$seed_idx" > "$target_path"
                fi
                ;;
            *)
                echo -n "seed$seed_idx" > "$target_path"
                ;;
        esac
        if [ ! -s "$target_path" ]; then
            echo "seed$seed_idx" > "$target_path"
        fi
    done
    echo "Generated $INITIAL_SEED_COUNT initial seeds in $SEED_TARGET_DIR"
fi

# Build target program if needed
echo "Checking target binary..."
if [ ! -f "$TARGET_PROGRAM" ]; then
    echo "Compiling $TARGET_SOURCE -> $TARGET_PROGRAM"
    gcc -o "$TARGET_PROGRAM" "$TARGET_SOURCE"
else
    echo "Target already built at $TARGET_PROGRAM; skipping compilation."
fi

# Environment variables
export MARCO_MODE=1
export SYMCC_OUTPUT_DIR="$OUTPUT_DIR"
# MARCO_QUEUEID will be set per-seed: 0 for initial seeds (afl-slave), 1 for generated cases (fifo)
# Marco-compatible: tree directories should be under output directory
export MARCO_TREE_DIR="$OUTPUT_DIR"
# Disable tmpfs usage in FastGen so repeated runs don't hit existing tmpfs dirs
export ANGORA_DISABLE_TMPFS=1
# MARCO_TRACEID will be extracted from filename id:XXXXXX per-seed
# Do not set global TAINT_OPTIONS; set taint_file per-seed
unset TAINT_OPTIONS || true
# Always include SymFit runtime libs in LD_LIBRARY_PATH
SYM_RUNTIME_LIB="$MARCO_SYMFIT_ROOT/symfit/build/symsan/lib"
RUNTIME_LD_PATH="$SYM_RUNTIME_LIB"
if [ -d "$MARCO_SYMFIT_ROOT/local-lib" ]; then
  RUNTIME_LD_PATH="$MARCO_SYMFIT_ROOT/local-lib:$RUNTIME_LD_PATH"
fi
export LD_LIBRARY_PATH="$RUNTIME_LD_PATH:${LD_LIBRARY_PATH:-}"
export PATH="/usr/lib/llvm-12/bin:${PATH}"

# Set SYMCC_AFL_COVERAGE_MAP=disabled for MARCO_MODE (coverage map not needed)
export SYMCC_AFL_COVERAGE_MAP=disabled

# Automatically determine target program address range for library function filtering
# This allows filtering out library function constraints (e.g., libc) from test program constraints
# Strategy: Try runtime detection first (most accurate), then fall back to static analysis
GET_ADDR_SCRIPT="$SCRIPT_DIR/get_target_address_range.sh"
if [ -f "$GET_ADDR_SCRIPT" ] && [ -f "$TARGET_PROGRAM" ]; then
    echo "Determining target program address range for library function filtering..."
    
    # Method 1: Try to get from a running process (if available)
    # This is the most accurate method for QEMU/SymFit
    RUNTIME_PID=""
    TARGET_PROGRAM_BASENAME=$(basename "$TARGET_PROGRAM")
    # Check if there's a running process that might be the target program
    # In QEMU/SymFit, the target program runs under symqemu-x86_64
    # We can try to find it by looking for processes that have the target binary mapped
    for pid in $(pgrep -f "symqemu.*$TARGET_PROGRAM_BASENAME" 2>/dev/null | head -1); do
        if [ -f "/proc/$pid/maps" ]; then
            # Check if this process has the target binary mapped
            if grep -q "$TARGET_PROGRAM_BASENAME" "/proc/$pid/maps" 2>/dev/null; then
                RUNTIME_PID="$pid"
                echo "Found running process with target binary: PID=$RUNTIME_PID"
                break
            fi
        fi
    done
    
    if [ -n "$RUNTIME_PID" ]; then
        # Try runtime detection first (most accurate)
        echo "Attempting runtime detection from PID $RUNTIME_PID..."
        if eval "$($GET_ADDR_SCRIPT "$TARGET_PROGRAM" --runtime "$RUNTIME_PID" --auto-disable)"; then
            if [ -n "${TARGET_BASE_ADDR:-}" ] && [ -n "${TARGET_SIZE:-}" ]; then
                echo "✓ Library function filtering enabled (runtime): base=$TARGET_BASE_ADDR, size=$TARGET_SIZE"
            else
                echo "⚠ Runtime detection failed, trying static analysis..."
                # Fall through to static analysis
                unset TARGET_BASE_ADDR
                unset TARGET_SIZE
            fi
        else
            echo "⚠ Runtime detection failed, trying static analysis..."
            unset TARGET_BASE_ADDR
            unset TARGET_SIZE
        fi
    fi
    
    # Method 2: Fall back to static analysis if runtime detection failed or not available
    if [ -z "${TARGET_BASE_ADDR:-}" ] || [ -z "${TARGET_SIZE:-}" ]; then
        echo "Attempting static analysis from ELF file..."
        if eval "$($GET_ADDR_SCRIPT "$TARGET_PROGRAM" --auto-disable)"; then
            if [ -n "${TARGET_BASE_ADDR:-}" ] && [ -n "${TARGET_SIZE:-}" ]; then
                echo "✓ Library function filtering enabled (static): base=$TARGET_BASE_ADDR, size=$TARGET_SIZE"
                echo "  Note: Static analysis may not be accurate for QEMU/SymFit runtime"
                echo "        Consider using runtime detection for better accuracy"
            else
                echo "⚠ Library function filtering disabled (could not determine address range)"
            fi
        else
            echo "⚠ Library function filtering disabled (script failed)"
            unset TARGET_BASE_ADDR
            unset TARGET_SIZE
        fi
    fi
else
    echo "⚠ Library function filtering disabled (script or target binary not found)"
    unset TARGET_BASE_ADDR
    unset TARGET_SIZE
fi

echo "Environment variables set:"
echo "  MARCO_MODE=$MARCO_MODE"
echo "  SYMCC_OUTPUT_DIR=$SYMCC_OUTPUT_DIR"
echo "  MARCO_TREE_DIR=$MARCO_TREE_DIR"
echo "  MARCO_QUEUEID and MARCO_TRACEID will be set per-seed from filename"
echo "  SYMCC_AFL_COVERAGE_MAP=$SYMCC_AFL_COVERAGE_MAP"
echo ""

# Start order: FastGen -> Scheduler -> SymFit (per-seed)
# Start FastGen first to open /tmp/myfifo read end
# Ensure /tmp/wp2 exists (created as FIFO); do NOT keep a persistent writer,
# so FastGen.solve can observe EOF per trace and naturally emit END tokens.
if [ ! -p /tmp/wp2 ]; then
  mkfifo /tmp/wp2 2>/dev/null || true
fi
WP2_WRITER_PID=""

echo "Starting Marco FastGen (one-shot)..."
# Marco-compatible: cd to OUTPUT_DIR so tree directories are created relative to it
cd "$OUTPUT_DIR"
# Set MARCO_LOG_DIR environment variable so FastGen knows where to write logs
LD_LIBRARY_PATH="/lib/x86_64-linux-gnu:$SYM_RUNTIME_LIB:$([ -d "$MARCO_SYMFIT_ROOT/local-lib" ] && echo "$MARCO_SYMFIT_ROOT/local-lib:" || echo "")$HOME/lib:${LD_LIBRARY_PATH:-}" \
LLVM_CONFIG="/usr/lib/llvm-12/bin/llvm-config" \
PATH="/usr/lib/llvm-12/bin:$PATH" \
RUST_BACKTRACE=1 RUST_LOG=info \
MARCO_LOG_DIR="$LOG_DIR" \
"$MARCO_FASTGEN" --sync_afl -i "$INPUT_DIR" -o "$OUTPUT_DIR" -t "$TARGET_PROGRAM" \
  -b 1 -f 1 -c 10 -T 60 -M 0 \
  -- "$TARGET_PROGRAM" @@ >> "$LOG_DIR/fastgen.log" 2>&1 &
FASTGEN_PID=$!
echo "FastGen started with PID: $FASTGEN_PID"

# Wait for FastGen to open FIFO
sleep 0.5

# Start Marco Scheduler (writer of /tmp/myfifo)
# Ensure only one scheduler instance is running
echo "Starting Marco scheduler..."
cd "$WORK_DIR"
# Kill any existing scheduler instances to prevent duplicates
pkill -9 -f "marco/scheduler/main-MS.py" 2>/dev/null || true
sleep 0.3
# Start scheduler with error handling to prevent crashes
# Set MARCO_LOG_DIR environment variable so scheduler knows where to write logs
MARCO_LOG_DIR="$LOG_DIR" python3 "$MARCO_SCHEDULER" -d 0 -m 2 > "$LOG_DIR/scheduler.log" 2>&1 &
SCHEDULER_PID=$!
echo "Scheduler started with PID: $SCHEDULER_PID"
# Verify scheduler is actually running
sleep 0.5
if ! ps -p $SCHEDULER_PID > /dev/null 2>&1; then
    echo "ERROR: Scheduler failed to start! Check $LOG_DIR/scheduler.log"
    exit 1
fi
# write a run boundary marker
echo "[run] start $(date)" >> "$LOG_DIR/Gscheduler.log"

# Function to check scheduler status (DO NOT restart to preserve graph state)
check_scheduler() {
    if ! ps -p $SCHEDULER_PID > /dev/null 2>&1; then
        echo "ERROR: Scheduler (PID: $SCHEDULER_PID) has stopped unexpectedly!"
        echo "Check $LOG_DIR/scheduler.log for errors"
        echo "DO NOT restart scheduler automatically to preserve graph state"
        return 1
    fi
    return 0
}

# Live console tails: mirror of pcpipe records (written by scheduler) and scheduler native log
( sleep 0.2; 
  : > "$LOG_DIR/pipe_mirror.log"
  stdbuf -oL tail -F "$LOG_DIR/pipe_mirror.log" | sed -u 's/^/[pcpipe] /'
) &
PCPIPE_TAIL_PID=$!

( sleep 0.2;
  : > "$LOG_DIR/Gscheduler.log"
  stdbuf -oL tail -F "$LOG_DIR/Gscheduler.log" | sed -u 's/^/[sched] /'
) &
SCHED_TAIL_PID=$!

# Sanity check: ensure pcpipe has one reader (scheduler). Writer will appear when fastgen starts
sleep 0.2
echo "[check] lsof /tmp/pcpipe (expect scheduler as reader)"
lsof /tmp/pcpipe || true

# Trigger CE loop: put a placeholder seed so run_solver starts and listens on /tmp/myfifo
mkdir -p "$OUTPUT_DIR/fifo/queue"
# Note: FastGen will generate new test cases in fifo/queue/id:XXXXXX format

# Run SymFit per seed (set MARCO_TRACEID and TAINT_OPTIONS per seed)
# Marco-compatible: extract traceid from filename id:XXXXXX
# IMPORTANT: Only execute initial seeds (tid=0 to tid=49), not files synced by FastGen
echo "Running SymFit per-seed (initial seeds only)..."
cd "$WORK_DIR"
SEED_DIR_IN_AFL="$OUTPUT_DIR/afl-slave/queue"

# Record initial seed list BEFORE FastGen starts syncing files
# This ensures we only execute the initial 50 seeds, not files synced later
INITIAL_SEED_MAX_TID=$((INITIAL_SEED_COUNT - 1))
echo "Will execute only initial seeds with tid=0 to tid=$INITIAL_SEED_MAX_TID"

# Execute only initial seeds (tid 0 to INITIAL_SEED_MAX_TID)
# Note: seed files have ",orig" suffix for FastGen depot compatibility
for seed_idx in $(seq 0 $INITIAL_SEED_MAX_TID); do
    seed=$(printf "id:%06d,orig" "$seed_idx")
    seed_path="$SEED_DIR_IN_AFL/$seed"
    
    # Skip if file doesn't exist (shouldn't happen, but be safe)
    if [ ! -f "$seed_path" ]; then
        echo "WARNING: Initial seed $seed not found, skipping" | tee -a "$LOG_DIR/symfit.log"
        continue
    fi
    # tid is directly the seed_idx (0 to INITIAL_SEED_MAX_TID)
    tid=$seed_idx
    # Execute SymFit (minimal logging for performance)
    timeout 15 env SYMCC_INPUT_FILE="$seed_path" TAINT_OPTIONS="taint_file=$seed_path inputid=$tid" MARCO_MODE=1 \
      TARGET_BASE_ADDR="${TARGET_BASE_ADDR:-}" \
      TARGET_SIZE="${TARGET_SIZE:-}" \
      "$SYMFIT_FGTEST" "$SYMFIT_QEMU" "$TARGET_PROGRAM" "$seed_path" >> "$LOG_DIR/symfit.log" 2>&1 || true
    
    # Ensure tree file exists for this seed (create empty file if FastGen didn't generate one)
    # This ensures all initial seeds have corresponding tree files in tree0/
    tree_file="$OUTPUT_DIR/tree0/id:$(printf "%06d" "$tid")"
    if [ ! -f "$tree_file" ]; then
        # Create empty tree file as placeholder if FastGen didn't generate one
        # This can happen if the seed doesn't trigger any interesting branches
        touch "$tree_file"
        echo "[SymFit] Created placeholder tree file for tid=$tid (no branches triggered)" >> "$LOG_DIR/symfit.log"
    fi
done

# Post-process: Ensure all initial seeds have tree files in tree0/
# Some seeds may not trigger interesting branches, so FastGen doesn't generate tree files
# We create empty placeholder files for missing ones to ensure completeness
echo "Ensuring all initial seeds have tree files in tree0/..."
for seed_idx in $(seq 0 $INITIAL_SEED_MAX_TID); do
    tree_file="$OUTPUT_DIR/tree0/id:$(printf "%06d" "$seed_idx")"
    if [ ! -f "$tree_file" ]; then
        # Create empty tree file as placeholder if FastGen didn't generate one
        touch "$tree_file"
        echo "[Post-process] Created placeholder tree file for tid=$seed_idx (no branches triggered)" >> "$LOG_DIR/symfit.log"
    fi
done

echo "SymFit initial seed runs completed (tid=0 to tid=$INITIAL_SEED_MAX_TID)"
echo "Now continuing with Marco-style continuous execution from fifo/queue..."

# Marco-style continuous execution: monitor fifo/queue for new test cases
# This mimics Marco's sync_fz_cefifo logic: continuously sync and execute new seeds
FIFO_QUEUE_DIR="$OUTPUT_DIR/fifo/queue"
# Allow infinite running (set MAX_RUNTIME to 0 or very large value to disable timeout)
# Set to 0 for infinite running (no timeout) to allow continuous seed generation
MAX_RUNTIME=${MAX_RUNTIME:-0}  # Default to 0 (infinite) to allow continuous seed generation
START_TIME=$(date +%s)
LAST_EXECUTED_TID=$INITIAL_SEED_MAX_TID

# Marco generates cases in fifo/queue starting from id:000000
# tid in fifo/queue starts from 0 (same as traceid)
# The key difference is queueid: queueid=0 for afl-slave/queue, queueid=1 for fifo/queue
# Tree files are organized by queueid: tree0/ for queueid=0, tree1/ for queueid=1
# Both tree0 and tree1 have id:000000, id:000001, etc. (tid starts from 0 for each queueid)
# We need to track which cases in fifo/queue have been executed
FIFO_NEXT_TID=0
# Find the highest tid in fifo/queue that has been executed (check tree1 directory)
# If a tree file exists in tree1/, it means the case has been executed
if [ -d "$OUTPUT_DIR/tree1" ]; then
    for tree_file in "$OUTPUT_DIR/tree1"/id:*; do
        if [ -f "$tree_file" ]; then
            tree_name=$(basename "$tree_file")
            tid_str=$(echo "$tree_name" | sed 's/^id:\([0-9]*\).*/\1/')
            tid_val=$((10#$tid_str))
            if [ $tid_val -ge $FIFO_NEXT_TID ]; then
                FIFO_NEXT_TID=$((tid_val + 1))
            fi
        fi
    done
fi

if [ "$MAX_RUNTIME" -eq 0 ]; then
    echo "Starting continuous execution loop (no timeout, will run indefinitely)..."
else
    echo "Starting continuous execution loop (max runtime: ${MAX_RUNTIME}s)..."
fi
echo "Will process fifo/queue cases starting from tid=$FIFO_NEXT_TID (queueid=1, tree files in tree1/)"

while true; do
    # Check timeout only if MAX_RUNTIME is set and > 0
    if [ "$MAX_RUNTIME" -gt 0 ]; then
        CURRENT_TIME=$(date +%s)
        ELAPSED=$((CURRENT_TIME - START_TIME))
        
        if [ $ELAPSED -ge $MAX_RUNTIME ]; then
            echo "Max runtime ($MAX_RUNTIME s) reached, stopping continuous execution"
            break
        fi
    fi
    
    # Check for new seeds in fifo/queue (Marco's ce_queue_dir)
    # Marco uses id:XXXXXX format, starting from next_ce_queue_id
    FOUND_NEW_SEED=false
    
    # Look for next expected seed in fifo/queue (id:XXXXXX format)
    # Marco generates cases in fifo/queue starting from id:000000
    EXPECTED_SEED=$(printf "id:%06d" "$FIFO_NEXT_TID")
    # Use absolute path to ensure SymFit correctly detects queueid=1 from path
    SEED_PATH=$(readlink -f "$FIFO_QUEUE_DIR/$EXPECTED_SEED" 2>/dev/null || echo "$FIFO_QUEUE_DIR/$EXPECTED_SEED")
    
    if [ -f "$SEED_PATH" ]; then
        FOUND_NEW_SEED=true
        # Extract traceid from filename: id:XXXXXX -> XXXXXX
        tid=$FIFO_NEXT_TID
        seed_path="$SEED_PATH"
        
        # Verify traceid extraction from filename
        seed_name=$(basename "$seed_path")
        tid_from_name=$(echo "$seed_name" | sed 's/^id:\([0-9]*\).*/\1/')
        tid_from_name=$((10#$tid_from_name))
        
        # Use tid from filename if it matches, otherwise use FIFO_NEXT_TID
        if [ "$tid_from_name" -eq "$FIFO_NEXT_TID" ]; then
            tid=$tid_from_name
        else
            echo "[SymFit] WARNING: tid mismatch - expected $FIFO_NEXT_TID, got $tid_from_name from filename" | tee -a "$LOG_DIR/symfit.log"
            tid=$tid_from_name  # Use the actual tid from filename
        fi
        
        # Execute the new seed from fifo/queue (minimal logging for performance)
        # SymFit will automatically detect queueid=1 from the file path containing "fifo/queue"
        # traceid will be extracted from filename "id:XXXXXX" format
        # This ensures tree files are written to tree1/ directory with correct traceid
        # Execute SymFit (minimal logging for performance)
        SYMCC_INPUT_FILE="$seed_path" TAINT_OPTIONS="taint_file=$seed_path inputid=$tid" MARCO_MODE=1 \
          TARGET_BASE_ADDR="${TARGET_BASE_ADDR:-}" \
          TARGET_SIZE="${TARGET_SIZE:-}" \
          timeout 15 "$SYMFIT_FGTEST" "$SYMFIT_QEMU" "$TARGET_PROGRAM" "$seed_path" >> "$LOG_DIR/symfit.log" 2>&1 || true
        FIFO_NEXT_TID=$((tid + 1))
    else
        # No new seed found at expected ID, check if there are any seeds with higher IDs (in case of gaps)
        # This handles cases where FastGen generates seeds out of order
        HIGHEST_TID_FOUND=-1
        for seed_file in "$FIFO_QUEUE_DIR"/id:*; do
            if [ -f "$seed_file" ]; then
                seed_name=$(basename "$seed_file")
                tid_str=$(echo "$seed_name" | sed 's/^id:\([0-9]*\).*/\1/')
                tid_val=$((10#$tid_str))
                if [ $tid_val -ge $FIFO_NEXT_TID ] && [ $tid_val -gt $HIGHEST_TID_FOUND ]; then
                    HIGHEST_TID_FOUND=$tid_val
                    FOUND_NEW_SEED=true
                fi
            fi
        done
        
        if [ "$FOUND_NEW_SEED" = "true" ] && [ $HIGHEST_TID_FOUND -ge $FIFO_NEXT_TID ]; then
            # Found a seed with higher ID, update FIFO_NEXT_TID to process it
            FIFO_NEXT_TID=$HIGHEST_TID_FOUND
            continue
        fi
    fi
    
    if [ "$FOUND_NEW_SEED" = "false" ]; then
        # No new seeds, wait a bit before checking again
        # This allows FastGen time to generate new seeds
        # Reduced wait time for faster response (Marco original doesn't have this loop)
        sleep 0.1
    fi
    
    # Check if FastGen and Scheduler are still running
    if ! ps -p $FASTGEN_PID > /dev/null 2>&1; then
        echo "FastGen (PID: $FASTGEN_PID) has stopped, exiting continuous loop"
        break
    fi
    if ! ps -p $SCHEDULER_PID > /dev/null 2>&1; then
        echo "Scheduler (PID: $SCHEDULER_PID) has stopped, exiting continuous loop"
        break
    fi
done

echo "Continuous execution loop completed"

# Wait a bit for CE to consume and produce outputs
echo "Waiting for FastGen to consume seeds..."
sleep 2

# Check process status
echo "Checking process status..."
if ps -p $SCHEDULER_PID > /dev/null; then echo "✓ Scheduler is running (PID: $SCHEDULER_PID)"; else echo "✗ Scheduler has stopped"; fi
if ps -p $FASTGEN_PID > /dev/null; then echo "✓ FastGen is running (PID: $FASTGEN_PID)"; else echo "✗ FastGen has stopped"; fi
if [ -n "$WP2_WRITER_PID" ] && ps -p $WP2_WRITER_PID > /dev/null; then echo "✓ wp2 writer is running (PID: $WP2_WRITER_PID)"; else echo "✗ wp2 writer has stopped"; fi

# Log summary
echo ""
echo "=== Log Summary ==="
echo "Scheduler log (last 10 lines):"
tail -10 "$LOG_DIR/scheduler.log" 2>/dev/null || echo "Could not read scheduler log"

echo ""
echo "FastGen log (last 10 lines):"
tail -10 "$LOG_DIR/fastgen.log" 2>/dev/null || echo "Could not read fastgen log"

echo ""
echo "SymFit log (last 10 lines):"
tail -10 "$LOG_DIR/symfit.log" 2>/dev/null || echo "Could not read symfit log"

# Keep tails running for interactive observation
echo ""
echo "Tails are running: [pcpipe] and [sched]. Press Ctrl+C to exit."
wait
