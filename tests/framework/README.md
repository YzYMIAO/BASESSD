# Test Framework Boundary

`munit.c` and `munit.h` are host-only test-runner code. They depend on C runtime
facilities such as stdio, dynamic allocation, command-line parsing and process
handling, so they must never be linked into firmware.

Production code uses `common/ssd_log.h` and `common/ssd_assert.h`. Test code may
use standard `assert.h` or explicitly include `tests/framework/munit.h` when it
needs suites, fixtures, parameters or randomized iterations.
