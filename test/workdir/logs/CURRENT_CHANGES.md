# Current Optimal Changes Summary

## Date: 2025-11-12

### 1. Marco/src/scheduler/main-MS.py

**Changes:**
- **Removed immediate actionable nodes check after ENDNEW@@**: After FastGen generates a new seed and sends `ENDNEW@@`, the scheduler no longer immediately checks for actionable nodes. Instead, it waits for the next `END@@` (after FastGen processes the next file) to check actionable nodes.
- **Added debug logging in END@@ handler**: Added detailed logging to show scheduler state:
  - `total_nodes`: Total number of nodes in the graph
  - `nodes_with_pcqueue`: Number of nodes with non-empty pcqueue
  - `actionable_count`: Number of actionable nodes
  - `has_actionable`: Whether there are actionable nodes
- **Scheduler now correctly sends real decisions**: The scheduler now properly identifies actionable nodes and sends real scheduling decisions (not just dummy decisions).

**Key Code Sections:**
- Lines 571-594: `ENDNEW@@` handler - removed immediate actionable nodes check
- Lines 596-650: `END@@` handler - added debug logging and proper actionable nodes checking

**Result:**
- Scheduler sends real scheduling decisions after each `END@@` (when actionable nodes exist)
- Only sends dummy decisions when there are no actionable nodes (e.g., after `ENDDUP@@`)

### 2. Marco/src/CE/fuzzer/cpp_core/util.cc

**Changes:**
- **Reverted to original code**: Restored the original path construction:
  ```cpp
  std::string output_file = outputDir + "/queue/id:" + std::string(6-old_string.size(),'0') + old_string;
  ```
  This ensures correct file naming in `fifo/queue` directory.

**Result:**
- Files are correctly named: `id:000000`, `id:000001`, `id:000002`, etc. (continuous numbering)
- No path construction issues

## System Behavior

### Before Changes:
- FastGen generated only 1-2 test cases then stopped
- Scheduler sent only dummy decisions (no real scheduling decisions)
- File naming had issues (skipping numbers)

### After Changes:
- ✅ FastGen generates multiple test cases continuously (50+ test cases)
- ✅ Scheduler sends real scheduling decisions (51+ real decisions in test)
- ✅ File naming is correct and continuous (id:000000, id:000001, id:000002, ...)
- ✅ Scheduler correctly identifies actionable nodes after each `END@@`

## Test Results

From latest test run:
- Generated 50 test cases in `fifo/queue` (id:000000 to id:000049)
- Scheduler sent 51 real scheduling decisions
- All decisions were real (not dummy) except after `ENDDUP@@` (which is expected)
- Debug logs show: `total_nodes=4, nodes_with_pcqueue=1, actionable_count=1, has_actionable=True`

## Notes

- The code is now in optimal state
- All log files are organized in `workdir/logs/` directory
- Old test run logs have been cleaned up

