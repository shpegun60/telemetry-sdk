# Installed Qt/TCP Mixed Batch

`run.py` installs the selected SDK components, moves their installation prefix,
copies consumers into an external directory and compiles only against that
relocated package. No source include paths, UX, firmware or hardware are used.

```sh
python -B tests/qt_batch/run.py --build-dir /tmp/telemetry-qt-batch \
  --cxx clang++-18 --sanitize
```

`Session.cpp` uses public `ConnectionSession` and real localhost sockets. It
checks TRM1/TBM1 sharing one session, ordinary reads, ordered FieldWrite → Command
Accepted → Service → FieldRead, canonical request rejection, accepted mutation
invalidation, old handles, malformed/mismatched capabilities, legacy and
ReadMany-only peers, callback lifetime and publication reentry. A deliberately
dropped mutating reply yields OutcomeUnknown; reconnect cannot replay it.

`Native.cpp` exercises real Resource session providers and both dispatcher
routes, ACK preflight, invalid session controls, overlap, replay guards and
capability bytes. `Legacy.cpp` links only `tcp_server`, demonstrating that old
constructors do not acquire a Batch or Qt link dependency.

`Workbench.cpp` uses a controlled clock for queued/offered/prepared timeouts and
checks a single FIFO, QueueFull/refusal atomicity, stale acknowledgements retaining
execution statuses, accepted mutation snapshot/editor retirement, read-only
preservation and callback reentry. `copyDraft()` checks short/aliased/stale output,
consumed writer, invalid staging, exact positive/negative qNaN/sNaN and signed
zero bits, and Void without consuming or rearming the editor.

`NoHeap.cpp` repeats the portable Workbench suite with allocation traps. Linux
also wraps malloc/calloc/realloc/free and all six negative controls must exit86;
Windows exercises new/aligned-new controls. Qt-owned allocations are permitted
and are outside that portable no-heap proof.

The JSON receipt preserves tool and source digests, commands, logs, maintained
nonzero check counts and negative-control outcomes. These are host tests; they
do not claim UART/DMA, STM32, atomic execution, rollback or exactly-once behavior.
