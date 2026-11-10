#!/bin/bash
# boots its own crash guest: not through run-qemu.sh
"${TEST_QEMU_CMD[@]}" > "$LOG" 2>&1
