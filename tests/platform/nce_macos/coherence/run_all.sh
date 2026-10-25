#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
python3 "$SCRIPT_DIR/test_read_selection.py"
python3 "$SCRIPT_DIR/test_canonical_writes.py"
python3 "$SCRIPT_DIR/test_observed_copy_plan.py"
python3 "$SCRIPT_DIR/test_observed_copy_queue.py"
python3 "$SCRIPT_DIR/test_page_read_faults.py"
python3 "$SCRIPT_DIR/test_observed_readback.py" --negative-controls
python3 "$SCRIPT_DIR/test_observation_routing.py"
python3 "$SCRIPT_DIR/test_readback_batches.py"
