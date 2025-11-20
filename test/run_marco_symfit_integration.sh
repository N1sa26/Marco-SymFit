#!/bin/bash

set -euo pipefail

# Flags: use --force to perform aggressive cleanup (pkill), otherwise skip.
FORCE_CLEAN=0
for arg in "$@"; do
  if [ "$arg" = "--force" ]; then FORCE_CLEAN=1; fi
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
sleep 1

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

# Generate 50 initial random seeds if not already present
echo "Preparing 50 initial random seeds..."
# Use seeds from test/workdir/seed if available, otherwise create new ones
SEED_SOURCE_DIR="$WORK_DIR/seed"
SEED_TARGET_DIR="$OUTPUT_DIR/afl-slave/queue"
INITIAL_SEED_COUNT=50

# Clear existing seeds in afl-slave/queue (keep only initial seeds)
rm -f "$SEED_TARGET_DIR"/id:* 2>/dev/null || true

# Generate random seeds
# Note: FastGen's depot requires afl-slave/queue files to have "orig" or "+cov" in the name
# So we add ",orig" suffix to match AFL naming convention
for seed_idx in $(seq 0 $((INITIAL_SEED_COUNT - 1))); do
    target_name=$(printf "id:%06d,orig" "$seed_idx")
    target_path="$SEED_TARGET_DIR/$target_name"
    
    # Generate targeted seeds to explore different paths in complex_target.c
    # The program has multiple branches:
    # 1. First char: 'A', 'B', 'C', or other
    # 2. Length: > 5 or <= 5
    # 3. Second char (if length>5): 'X', 'Y', or other
    # 4. String prefix: "TEST", "HELLO", "WORLD", or other
    # 5. Sum: > 500, > 300, > 100, or <= 100
    # 6. Even/odd distribution
    # 7. Pattern matching (if length>=6)
    
    case $seed_idx in
        # First char variations (A, B, C, other)
        0) echo -n "A" > "$target_path" ;;                    # A, length<=5
        1) echo -n "B" > "$target_path" ;;                    # B, length<=5
        2) echo -n "C" > "$target_path" ;;                    # C, length<=5
        3) echo -n "D" > "$target_path" ;;                    # Other, length<=5
        
        # Length > 5 variations
        4) echo -n "A12345" > "$target_path" ;;               # A, length>5, second char not X/Y
        5) echo -n "AXYZ123" > "$target_path" ;;             # A, length>5, second char X
        6) echo -n "AYZ1234" > "$target_path" ;;             # A, length>5, second char Y
        7) echo -n "B123456" > "$target_path" ;;            # B, length>5
        8) echo -n "BX12345" > "$target_path" ;;             # B, length>5, second char X
        9) echo -n "BY12345" > "$target_path" ;;             # B, length>5, second char Y
        10) echo -n "C123456" > "$target_path" ;;            # C, length>5
        11) echo -n "CX12345" > "$target_path" ;;            # C, length>5, second char X
        12) echo -n "CY12345" > "$target_path" ;;            # C, length>5, second char Y
        13) echo -n "D123456" > "$target_path" ;;            # Other, length>5
        
        # String prefix variations (TEST, HELLO, WORLD, other)
        14) echo -n "TEST" > "$target_path" ;;               # TEST prefix, length=4
        15) echo -n "TEST123" > "$target_path" ;;            # TEST prefix, length>5
        16) echo -n "HELLO" > "$target_path" ;;              # HELLO prefix, length=5
        17) echo -n "HELLO123" > "$target_path" ;;           # HELLO prefix, length>5
        18) echo -n "WORLD" > "$target_path" ;;              # WORLD prefix, length=5
        19) echo -n "WORLD123" > "$target_path" ;;           # WORLD prefix, length>5
        20) echo -n "OTHER" > "$target_path" ;;             # Other prefix, length=5
        
        # Sum variations (>500, >300, >100, <=100)
        21) printf '\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF' > "$target_path" ;;  # Sum > 500 (high values)
        22) printf '\x80\x80\x80\x80\x80\x80\x80\x80' > "$target_path" ;;         # Sum > 300
        23) printf '\x40\x40\x40\x40\x40' > "$target_path" ;;                     # Sum > 100
        24) printf '\x10\x10\x10' > "$target_path" ;;                            # Sum <= 100
        
        # Even/odd distribution variations
        25) printf '\x00\x02\x04\x06\x08\x0A\x0C\x0E' > "$target_path" ;;         # All even
        26) printf '\x01\x03\x05\x07\x09\x0B\x0D\x0F' > "$target_path" ;;         # All odd
        27) printf '\x00\x01\x02\x03\x04\x05\x06\x07' > "$target_path" ;;         # Mixed, even > odd
        28) printf '\x01\x02\x03\x04\x05\x06\x07\x08' > "$target_path" ;;         # Mixed, odd > even
        29) printf '\x00\x01\x02\x03\x04\x05' > "$target_path" ;;                 # Equal even/odd
        
        # Pattern matching variations (length>=6)
        30) echo -n "AABBCC" > "$target_path" ;;             # Pattern match >= 2 (A==A, B==B, C==C)
        31) echo -n "AABCCC" > "$target_path" ;;             # Pattern match == 1 (A==A)
        32) echo -n "ABCDEF" > "$target_path" ;;             # Pattern match == 0 (no matches)
        33) echo -n "AABBCCDD" > "$target_path" ;;           # Pattern match >= 2, longer
        
        # Combined variations
        34) echo -n "ATEST123" > "$target_path" ;;            # A + TEST prefix
        35) echo -n "BHELLO123" > "$target_path" ;;          # B + HELLO prefix
        36) echo -n "CWORLD123" > "$target_path" ;;           # C + WORLD prefix
        37) echo -n "AXTEST123" > "$target_path" ;;          # A + X + TEST prefix
        38) echo -n "AYHELLO123" > "$target_path" ;;         # A + Y + HELLO prefix
        39) echo -n "BXWORLD123" > "$target_path" ;;          # B + X + WORLD prefix
        
        # Edge cases
        40) printf '\x00' > "$target_path" ;;                  # Single null byte
        41) printf '\xFF' > "$target_path" ;;                 # Single 0xFF
        42) printf '\x00\x00\x00\x00\x00' > "$target_path" ;; # 5 null bytes (length=5)
        43) printf '\xFF\xFF\xFF\xFF\xFF\xFF' > "$target_path" ;; # 6 0xFF bytes (length>5)
        
        # Random variations for remaining seeds
        44|45|46|47|48|49)
            # Generate random content with some structure
            seed_length=$((RANDOM % 20 + 1))
            if [ $((seed_idx % 2)) -eq 0 ]; then
                # Random printable
                python3 -c "import random, string; print(''.join(random.choices(string.printable, k=$seed_length)), end='')" > "$target_path" 2>/dev/null || \
                head -c "$seed_length" /dev/urandom | tr -dc '[:print:]' > "$target_path" 2>/dev/null || \
                echo -n "RANDOM$seed_idx" > "$target_path"
            else
                # Random binary
                head -c "$seed_length" /dev/urandom > "$target_path" 2>/dev/null || \
                python3 -c "import os; os.write(1, os.urandom($seed_length))" > "$target_path" || \
                echo -n "RANDOM$seed_idx" > "$target_path"
            fi
            ;;
        *)
            # Fallback
            echo -n "seed$seed_idx" > "$target_path"
            ;;
    esac
    
    # Ensure file is not empty
    if [ ! -s "$target_path" ]; then
        echo "seed$seed_idx" > "$target_path"
    fi
done

echo "Generated $INITIAL_SEED_COUNT initial seeds in $SEED_TARGET_DIR"

# Build target program if needed
echo "Compiling target program..."
if [ ! -f "$TARGET_PROGRAM" ] || [ "$TARGET_SOURCE" -nt "$TARGET_PROGRAM" ]; then
    echo "Compiling $TARGET_SOURCE -> $TARGET_PROGRAM"
    gcc -o "$TARGET_PROGRAM" "$TARGET_SOURCE"
fi

# Environment variables
export MARCO_MODE=1
export SYMCC_OUTPUT_DIR="$OUTPUT_DIR"
# MARCO_QUEUEID will be set per-seed: 0 for initial seeds (afl-slave), 1 for generated cases (fifo)
# Marco-compatible: tree directories should be under output directory
export MARCO_TREE_DIR="$OUTPUT_DIR"
# MARCO_TRACEID will be extracted from filename id:XXXXXX per-seed
# Do not set global TAINT_OPTIONS; set taint_file per-seed
unset TAINT_OPTIONS || true
# Use local-lib if it exists in Marco-SymFit directory, otherwise use system paths
if [ -d "$MARCO_SYMFIT_ROOT/local-lib" ]; then
  export LD_LIBRARY_PATH="$MARCO_SYMFIT_ROOT/local-lib:${LD_LIBRARY_PATH:-}"
else
  export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
fi
export PATH="/usr/lib/llvm-12/bin:${PATH}"

# Set SYMCC_AFL_COVERAGE_MAP=disabled for MARCO_MODE (coverage map not needed)
export SYMCC_AFL_COVERAGE_MAP=disabled

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
LD_LIBRARY_PATH="/lib/x86_64-linux-gnu:$([ -d "$MARCO_SYMFIT_ROOT/local-lib" ] && echo "$MARCO_SYMFIT_ROOT/local-lib:" || echo "")$HOME/lib:${LD_LIBRARY_PATH:-}" \
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
sleep 2

# Start Marco Scheduler (writer of /tmp/myfifo)
# Ensure only one scheduler instance is running
echo "Starting Marco scheduler..."
cd "$WORK_DIR"
# Kill any existing scheduler instances to prevent duplicates
pkill -9 -f "marco/scheduler/main-MS.py" 2>/dev/null || true
sleep 1
# Start scheduler with error handling to prevent crashes
# Set MARCO_LOG_DIR environment variable so scheduler knows where to write logs
MARCO_LOG_DIR="$LOG_DIR" python3 "$MARCO_SCHEDULER" -d 0 -m 2 > "$LOG_DIR/scheduler.log" 2>&1 &
SCHEDULER_PID=$!
echo "Scheduler started with PID: $SCHEDULER_PID"
# Verify scheduler is actually running
sleep 2
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
( sleep 1; 
  : > "$LOG_DIR/pipe_mirror.log"
  stdbuf -oL tail -F "$LOG_DIR/pipe_mirror.log" | sed -u 's/^/[pcpipe] /'
) &
PCPIPE_TAIL_PID=$!

( sleep 1;
  : > "$LOG_DIR/Gscheduler.log"
  stdbuf -oL tail -F "$LOG_DIR/Gscheduler.log" | sed -u 's/^/[sched] /'
) &
SCHED_TAIL_PID=$!

# Sanity check: ensure pcpipe has one reader (scheduler). Writer will appear when fastgen starts
sleep 1
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
    echo "[SymFit] run seed tid=$tid file=$seed_path" | tee -a "$LOG_DIR/symfit.log"
    # queueid will be automatically determined from input file path (Marco-compatible)
    # Marco passes inputid through TAINT_OPTIONS: "taint_file={} tid={} shmid={} pipeid={} inputid={}"
    # For SymFit, we use a simplified format: "taint_file={} inputid={}" (tid, shmid, pipeid are optional)
    echo "[SymFit] Starting fgtest for tid=$tid..." | tee -a "$LOG_DIR/symfit.log"
    timeout 30 env SYMCC_INPUT_FILE="$seed_path" TAINT_OPTIONS="taint_file=$seed_path inputid=$tid" MARCO_MODE=1 \
      "$SYMFIT_FGTEST" "$SYMFIT_QEMU" "$TARGET_PROGRAM" "$seed_path" >> "$LOG_DIR/symfit.log" 2>&1 || {
      echo "[SymFit] WARNING: fgtest for tid=$tid exited with code $? (timeout or error)" | tee -a "$LOG_DIR/symfit.log"
    }
    echo "[SymFit] fgtest completed for tid=$tid" | tee -a "$LOG_DIR/symfit.log"

    # Check scheduler is still running before waiting for END token
    check_scheduler || echo "WARNING: Scheduler check failed after seed tid=$tid"
    
    # After each seed run, wait (bounded) for FastGen to naturally emit END tokens
    # This follows Marco's semantics: solve() sees EOF on /tmp/wp2, then run_solver writes END@@/ENDNEW@@...
    echo "[SymFit] wait for END token after tid=$tid (up to 12s)" | tee -a "$LOG_DIR/symfit.log"
    # Get initial line count to track new END tokens
    INITIAL_PIPE_LINES=$(wc -l < "$LOG_DIR/pipe_mirror.log" 2>/dev/null || echo "0")
    waited=0
    END_DETECTED=false
    MAX_WAIT=12
    # Use timeout to ensure we don't hang indefinitely
    while [ $waited -lt $MAX_WAIT ]; do
      # Force flush any pending output
      sync 2>/dev/null || true
      
      CURRENT_PIPE_LINES=$(wc -l < "$LOG_DIR/pipe_mirror.log" 2>/dev/null || echo "0")
      
      # Debug: log progress every 3 seconds
      if [ $((waited % 3)) -eq 0 ] && [ $waited -gt 0 ]; then
        echo "[SymFit] Still waiting for END token after tid=$tid (waited ${waited}s, INITIAL=$INITIAL_PIPE_LINES, CURRENT=$CURRENT_PIPE_LINES)" | tee -a "$LOG_DIR/symfit.log"
      fi
      
      if [ "$CURRENT_PIPE_LINES" -gt "$INITIAL_PIPE_LINES" ]; then
        # Check only new lines for END tokens
        NEW_LINES=$((CURRENT_PIPE_LINES - INITIAL_PIPE_LINES))
        if tail -n "$NEW_LINES" "$LOG_DIR/pipe_mirror.log" 2>/dev/null | grep -qE '^(END@@|ENDNEW@@|ENDDUP@@|ENDUNSAT@@)$'; then
          echo "[SymFit] detected END token after tid=$tid (waited ${waited}s)" | tee -a "$LOG_DIR/symfit.log"
          END_DETECTED=true
          break
        fi
      fi
      
      # Also check last few lines as fallback (in case of line count issues)
      # This is more robust and checks the actual last lines, not just new ones
      if tail -n 20 "$LOG_DIR/pipe_mirror.log" 2>/dev/null | grep -qE '^(END@@|ENDNEW@@|ENDDUP@@|ENDUNSAT@@)$'; then
        if [ "$END_DETECTED" = "false" ] && [ $waited -ge 2 ]; then
          echo "[SymFit] detected END token after tid=$tid (waited ${waited}s, fallback check)" | tee -a "$LOG_DIR/symfit.log"
          END_DETECTED=true
          break
        fi
      fi
      
      # Check scheduler health during wait
      if [ $((waited % 3)) -eq 0 ]; then
        check_scheduler || echo "[SymFit] WARNING: Scheduler check failed during wait for END token" | tee -a "$LOG_DIR/symfit.log"
      fi
      
      # Use timeout to prevent indefinite blocking
      sleep 1
      waited=$((waited+1))
    done
    
    # CRITICAL: Always continue after timeout, even if END token not detected
    if [ "$END_DETECTED" = "false" ]; then
      echo "[SymFit] WARNING: No END token detected after ${MAX_WAIT}s for tid=$tid, continuing anyway" | tee -a "$LOG_DIR/symfit.log"
      echo "[SymFit] DEBUG: INITIAL_PIPE_LINES=$INITIAL_PIPE_LINES, CURRENT_PIPE_LINES=$(wc -l < "$LOG_DIR/pipe_mirror.log" 2>/dev/null || echo "0")" | tee -a "$LOG_DIR/symfit.log"
      # Force continue - don't block the loop
      END_DETECTED=false
    fi

    # Additionally ensure no writer remains on /tmp/wp2, so FastGen.solve observes EOF.
    echo "[SymFit] ensure no writer holds /tmp/wp2 (seed tid=$tid)" | tee -a "$LOG_DIR/symfit.log"
    waited_close=0
    MAX_WAIT_CLOSE=8
    while [ $waited_close -lt $MAX_WAIT_CLOSE ]; do
      # lsof 退出码 0 表示有进程持有；非0 表示无。仅关心写端 (FD with 'w').
      if ! lsof -Fpcf /tmp/wp2 2>/dev/null | grep -q 'w$'; then
        echo "[SymFit] /tmp/wp2 has no writer after tid=$tid (waited ${waited_close}s)" | tee -a "$LOG_DIR/symfit.log"
        break
      fi
      # Debug: log progress every 3 seconds
      if [ $((waited_close % 3)) -eq 0 ] && [ $waited_close -gt 0 ]; then
        echo "[SymFit] Still waiting for /tmp/wp2 to close after tid=$tid (waited ${waited_close}s)" | tee -a "$LOG_DIR/symfit.log"
      fi
      sleep 1
      waited_close=$((waited_close+1))
    done
    # CRITICAL: Always continue after timeout, even if /tmp/wp2 still has writers
    if lsof -Fpcf /tmp/wp2 2>/dev/null | grep -q 'w$'; then
      echo "[SymFit] WARNING: /tmp/wp2 still has writer after ${MAX_WAIT_CLOSE}s for tid=$tid, continuing anyway" | tee -a "$LOG_DIR/symfit.log"
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
        
        echo "[SymFit] ===== Processing new seed from fifo/queue =====" | tee -a "$LOG_DIR/symfit.log"
        echo "[SymFit] tid=$tid (extracted from filename: $seed_name), file=$seed_path, queueid=1" | tee -a "$LOG_DIR/symfit.log"
        echo "[SymFit] File size: $(stat -c%s "$seed_path" 2>/dev/null || echo "unknown") bytes" | tee -a "$LOG_DIR/symfit.log"
        
        # Check if FastGen is ready to process (by checking if it's waiting on /tmp/wp2)
        # FastGen's solve() function should be waiting to read from /tmp/wp2
        echo "[SymFit] Checking FastGen status before execution..." | tee -a "$LOG_DIR/symfit.log"
        
        # Execute the new seed from fifo/queue
        # SymFit will automatically detect queueid=1 from the file path containing "fifo/queue"
        # traceid will be extracted from filename "id:XXXXXX" format
        # This ensures tree files are written to tree1/ directory with correct traceid
        echo "[SymFit] Starting SymFit execution with tid=$tid, queueid=1..." | tee -a "$LOG_DIR/symfit.log"
        # seed_path is already absolute (from SEED_PATH which uses readlink -f)
        # This ensures SymFit correctly detects queueid=1 from path containing "fifo/queue"
        # Get initial line count of symfit.log to check if data was written
        INITIAL_LOG_LINE_COUNT=$(wc -l < "$LOG_DIR/symfit.log" 2>/dev/null || echo "0")
        
        SYMCC_INPUT_FILE="$seed_path" TAINT_OPTIONS="taint_file=$seed_path inputid=$tid" MARCO_MODE=1 \
          timeout 30 "$SYMFIT_FGTEST" "$SYMFIT_QEMU" "$TARGET_PROGRAM" "$seed_path" >> "$LOG_DIR/symfit.log" 2>&1 || true
        SYMFIT_EXIT_CODE=$?
        echo "[SymFit] SymFit execution completed with exit code=$SYMFIT_EXIT_CODE" | tee -a "$LOG_DIR/symfit.log"
        
        # Check if data was written to /tmp/wp2 by checking if "Writing to /tmp/wp2" appears in the log
        CURRENT_LOG_LINE_COUNT=$(wc -l < "$LOG_DIR/symfit.log" 2>/dev/null || echo "0")
        if [ "$CURRENT_LOG_LINE_COUNT" -gt "$INITIAL_LOG_LINE_COUNT" ]; then
            # Check if "Writing to /tmp/wp2" appears in the new log lines
            if tail -n $((CURRENT_LOG_LINE_COUNT - INITIAL_LOG_LINE_COUNT)) "$LOG_DIR/symfit.log" 2>/dev/null | grep -q "Writing to /tmp/wp2.*queueid=1"; then
                echo "[SymFit] ✓ Data was written to /tmp/wp2 for tid=$tid" | tee -a "$LOG_DIR/symfit.log"
            else
                echo "[SymFit] WARNING: No data written to /tmp/wp2 for tid=$tid (QEMU may not have triggered any branches)" | tee -a "$LOG_DIR/symfit.log"
            fi
        fi
        
        # Check if data was written to /tmp/wp2
        if [ -p /tmp/wp2 ]; then
            echo "[SymFit] /tmp/wp2 exists as FIFO" | tee -a "$LOG_DIR/symfit.log"
        else
            echo "[SymFit] WARNING: /tmp/wp2 does not exist or is not a FIFO" | tee -a "$LOG_DIR/symfit.log"
        fi
        
        # Wait for END token and ensure FastGen has processed the input
        echo "[SymFit] Waiting for END token after tid=$tid (up to 12s)..." | tee -a "$LOG_DIR/symfit.log"
        waited=0
        END_DETECTED=false
        TREE_FILE_CREATED=false
        MAX_WAIT_FIFO=12
        # Get the line count of pipe_mirror.log before waiting
        INITIAL_LINE_COUNT=$(wc -l < "$LOG_DIR/pipe_mirror.log" 2>/dev/null || echo "0")
        while [ $waited -lt $MAX_WAIT_FIFO ]; do
          # Force flush any pending output
          sync 2>/dev/null || true
          
          # Check pipe_mirror.log for END tokens (check new lines since we started waiting)
          # Use a more flexible pattern that handles whitespace
          CURRENT_LINE_COUNT=$(wc -l < "$LOG_DIR/pipe_mirror.log" 2>/dev/null || echo "0")
          
          # Debug: log progress every 3 seconds
          if [ $((waited % 3)) -eq 0 ] && [ $waited -gt 0 ]; then
            echo "[SymFit] Still waiting for END token after tid=$tid (waited ${waited}s, INITIAL=$INITIAL_LINE_COUNT, CURRENT=$CURRENT_LINE_COUNT)" | tee -a "$LOG_DIR/symfit.log"
          fi
          
          if [ "$CURRENT_LINE_COUNT" -gt "$INITIAL_LINE_COUNT" ]; then
            # Check the new lines for END tokens
            if tail -n $((CURRENT_LINE_COUNT - INITIAL_LINE_COUNT)) "$LOG_DIR/pipe_mirror.log" 2>/dev/null | grep -qE '(END@@|ENDNEW@@|ENDDUP@@|ENDUNSAT@@)'; then
              echo "[SymFit] ✓ Detected END token after tid=$tid (waited ${waited}s)" | tee -a "$LOG_DIR/symfit.log"
              END_DETECTED=true
            fi
          fi
          # Also check the last 20 lines as fallback (more robust than 500 lines)
          if tail -n 20 "$LOG_DIR/pipe_mirror.log" 2>/dev/null | grep -qE '(END@@|ENDNEW@@|ENDDUP@@|ENDUNSAT@@)'; then
            if [ "$END_DETECTED" = "false" ] && [ $waited -ge 2 ]; then
              echo "[SymFit] ✓ Detected END token after tid=$tid (waited ${waited}s, fallback check)" | tee -a "$LOG_DIR/symfit.log"
              END_DETECTED=true
            fi
          fi
          # Check if FastGen has processed the case (check for tree file)
          if [ -f "$OUTPUT_DIR/tree1/id:$(printf "%06d" $tid)" ]; then
            if [ "$TREE_FILE_CREATED" = "false" ]; then
              echo "[SymFit] ✓ Tree file created for tid=$tid" | tee -a "$LOG_DIR/symfit.log"
              TREE_FILE_CREATED=true
            fi
            # If we have both END token and tree file, we can proceed
            if [ "$END_DETECTED" = "true" ] && [ "$TREE_FILE_CREATED" = "true" ]; then
              break
            fi
          fi
          check_scheduler || echo "[SymFit] WARNING: Scheduler check failed during wait" | tee -a "$LOG_DIR/symfit.log"
          sleep 1
          waited=$((waited+1))
        done
        
        # CRITICAL: Always continue after timeout, even if END token not detected
        if [ "$END_DETECTED" = "false" ]; then
            echo "[SymFit] WARNING: No END token detected after ${MAX_WAIT_FIFO}s for tid=$tid, continuing anyway" | tee -a "$LOG_DIR/symfit.log"
            echo "[SymFit] DEBUG: INITIAL_LINE_COUNT=$INITIAL_LINE_COUNT, CURRENT_LINE_COUNT=$(wc -l < "$LOG_DIR/pipe_mirror.log" 2>/dev/null || echo "0")" | tee -a "$LOG_DIR/symfit.log"
        fi
        
        if [ "$TREE_FILE_CREATED" = "false" ]; then
            echo "[SymFit] WARNING: No tree file found for tid=$tid after ${MAX_WAIT_FIFO}s, continuing anyway" | tee -a "$LOG_DIR/symfit.log"
            echo "[SymFit] This may indicate that FastGen did not process this input" | tee -a "$LOG_DIR/symfit.log"
        fi
        
        # Ensure no writer on /tmp/wp2
        waited_close=0
        MAX_WAIT_CLOSE_FIFO=8
        while [ $waited_close -lt $MAX_WAIT_CLOSE_FIFO ]; do
          if ! lsof -Fpcf /tmp/wp2 2>/dev/null | grep -q 'w$'; then
            echo "[SymFit] /tmp/wp2 has no writer after tid=$tid (waited ${waited_close}s)" | tee -a "$LOG_DIR/symfit.log"
            break
          fi
          # Debug: log progress every 3 seconds
          if [ $((waited_close % 3)) -eq 0 ] && [ $waited_close -gt 0 ]; then
            echo "[SymFit] Still waiting for /tmp/wp2 to close after tid=$tid (waited ${waited_close}s)" | tee -a "$LOG_DIR/symfit.log"
          fi
          sleep 1
          waited_close=$((waited_close+1))
        done
        # CRITICAL: Always continue after timeout, even if /tmp/wp2 still has writers
        if lsof -Fpcf /tmp/wp2 2>/dev/null | grep -q 'w$'; then
          echo "[SymFit] WARNING: /tmp/wp2 still has writer after ${MAX_WAIT_CLOSE_FIFO}s for tid=$tid, continuing anyway" | tee -a "$LOG_DIR/symfit.log"
        fi
        
        # Additional wait to ensure FastGen has finished processing and is ready for next input
        # This is important to avoid race conditions where we start the next seed before FastGen is ready
        if [ "$TREE_FILE_CREATED" = "true" ]; then
            echo "[SymFit] FastGen has processed tid=$tid, waiting 2s before next seed..." | tee -a "$LOG_DIR/symfit.log"
            sleep 2
        fi
        
        echo "[SymFit] ===== Completed processing seed tid=$tid =====\n" | tee -a "$LOG_DIR/symfit.log"
        # Update FIFO_NEXT_TID to the next expected tid (tid + 1)
        # This ensures we process seeds in order: id:000000, id:000001, id:000002, ...
        FIFO_NEXT_TID=$((tid + 1))
        echo "[SymFit] Updated FIFO_NEXT_TID to $FIFO_NEXT_TID" | tee -a "$LOG_DIR/symfit.log"
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
            echo "[SymFit] Found seed with higher ID: $HIGHEST_TID_FOUND (expected $FIFO_NEXT_TID), will process it" | tee -a "$LOG_DIR/symfit.log"
            FIFO_NEXT_TID=$HIGHEST_TID_FOUND
            continue
        fi
    fi
    
    if [ "$FOUND_NEW_SEED" = "false" ]; then
        # No new seeds, wait a bit before checking again
        # This allows FastGen time to generate new seeds
        sleep 2
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
sleep 10

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
