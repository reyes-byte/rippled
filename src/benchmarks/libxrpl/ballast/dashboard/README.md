# Ballast C++ terminal dashboard

Run from the repository root:

```sh
bash bin/ballast-dashboard
```

The launcher enters the required GCC Nix devshell, resolves the existing Conan
dependency subset if necessary, and builds the viewer. It does not build or run
the native benchmark. The viewer uses C++23, Boost.JSON, and POSIX terminal APIs;
there is no Python UI or curses dependency. Linux is tested; other platforms are
unverified. Use a terminal of at least 80 columns by 24 rows.

For a static display, redirected output, or a terminal without interactive support:

```sh
bash bin/ballast-dashboard --plain
bash bin/ballast-dashboard --plain --stage verify_only
bash bin/ballast-dashboard --plain --provenance
```

After the first build, run `build/ballast-dashboard/ballast_dashboard` directly
inside `nix develop .#gcc` to skip the launcher's configure/build check.

| Key                  | Action                                                            |
| -------------------- | ----------------------------------------------------------------- |
| `1`, `2`, `3`        | Parse, verify, or combined stage                                  |
| `m`                  | Cycle peak, cumulative requests, operation-end live bytes, timing |
| `r`                  | Cycle combined results and the three individual runs              |
| `j` / `k`, down / up | Select a fixture row                                              |
| `p`                  | Toggle recorded provenance                                        |
| `q`, Ctrl-C, Ctrl-D  | Exit and restore terminal settings                                |

## Execution flow

`Main.cpp` reads arguments and calls `loadSnapshot()`. That function parses JSON
into ordinary C++ structs: a `Snapshot` owns `View` objects, each owning its
`Group` statistics. It checks the measurement scope, complete 18-group matrix,
sample counts, and metric ranges. It does not verify the underlying CSV or
execute cryptography.

`State` stores only the selected view, stage, metric, row, and provenance toggle.
`State::handle()` changes those indices in response to a key. `render()` takes
the snapshot and state by const reference and returns text lines. Selecting
groups returns pointers into the snapshot's vectors; the snapshot stays alive
and unchanged throughout the interactive loop, so those pointers remain valid.

`Terminal.cpp` owns the interactive terminal session. Its `Terminal` constructor
saves settings and enables immediate key input. Its destructor restores settings
and the normal screen: this is C++ RAII, tying cleanup to an object's lifetime.
Signal handlers set a flag so cleanup happens in the normal loop. Forced process
termination such as SIGKILL cannot run destructors. The loop polls for keys and
terminal size changes; it redraws only when necessary.

For a concrete trace, the default selection is ML-DSA-44 with 32 signers in
`parse_only`, across all three recorded runs. It displays 90 samples, peak
312232 B, cumulative requests 877665 B, and operation-end live bytes 131120 B.
Pressing `r` selects `run-1`, with 30 samples and the same allocation values in
this dataset. Pressing `2` selects verification: its peak is 4138 B, with the
already parsed transaction outside the measured scope. Neither key starts a
measurement or signature check.

## Saved data and its limits

The default `artifacts/ballast/dashboard.json` is a derived display packet from
the verified `artifacts/ballast/baseline-2026-10-25.zip`. It contains the original
combined summary, three per-run summaries computed with the existing CSV
validator, run metadata, the bundle's SHA-256, and its recorded 155 passing
tests. These are historical observations, not fresh checks or mocks. The viewer
does not authenticate the display packet or verify its bundle hash on opening.

Verify the immutable source independently with:

```sh
python3 bin/ballast_bundle.py verify artifacts/ballast/baseline-2026-10-25.zip
```

A measurement validator's existing `summary.json` also works directly:

```sh
bash bin/ballast-dashboard --data /path/to/summary.json
```

That format supplies a single combined view. Missing provenance/test counts are
shown as unavailable; the viewer does not invent them. To display new results,
pass their summary explicitly; the committed display packet is a fixed snapshot.

Measurements cover intercepted global C++ new/delete requested bytes. Wire
storage is excluded. Verification additionally excludes the pre-parsed
transaction. C allocation paths, OpenSSL internals, stack/inline objects,
allocator bookkeeping, and tracker metadata are outside coverage. These are
neither node RSS nor a node-wide lower bound. Instrumented timing includes
allocation-hook overhead. Signature validity does not establish ledger
authorization, quorum, fee sufficiency, or network acceptance.

The separate ownership microbenchmark remains unchanged; this viewer displays
the native transaction baseline. It introduces no protocol or optimization
changes.

## Viewer checks

Inside `nix develop .#gcc`, after launching once:

```sh
cmake --build build/ballast-dashboard --parallel 2
ctest --test-dir build/ballast-dashboard --output-on-failure
```

The viewer tests cover loading recorded values, navigation without changing
source statistics, scope labels, provenance, plain summary input, malformed
data, control-character sanitization, narrow terminals, and zero-valued bars.
They are separate from the baseline's recorded native cryptography tests.
