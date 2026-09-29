# Dog-fooding the LSP wire test in FC — assessment

**Status:** tabled (2026-06-26). Captured for later; no work started.

**Question that prompted this:** the LSP integration test is written in Python
(`tests/lsp/lsp_test.py`, driven by `tests/lsp/run_lsp_tests.sh`). Could it be
written in FC instead — i.e. is there a dog-fooding opportunity?

**Short answer:** Yes, there's a real opportunity, but it's gated on one
capability FC doesn't have today — **spawning a subprocess and talking to it over
pipes** — plus, optionally, a JSON helper. Everything else the test needs, FC's
stdlib already covers. The most valuable framing is *not* "rewrite the test" but
"add a reusable process-spawn primitive to the stdlib, then convert the test on
top of it." There is also a genuine trade-off (loss of test independence) that
argues for keeping the wire test in Python even if we add the primitive.

---

## The two files are different candidates

### `tests/lsp/run_lsp_tests.sh` — leave as bash
A ~15-line launcher: check `python3` is present, resolve the binary via
`make print-bin`, export `FCC_STDLIB_DIR="$(pwd)/stdlib"`, exec the test.
Converting to FC buys nothing — you'd still need a launcher to build and run the
FC test binary, and "set env + exec" is exactly what a shell is for. Not a
dog-food opportunity.

### `tests/lsp/lsp_test.py` — the real candidate
This is the wire test: it frames a JSON-RPC session over stdio, sends requests
and notifications, and asserts on responses + `publishDiagnostics`. This is where
dog-fooding would live.

---

## Capability inventory: what the Python test needs vs. what FC has

| Capability the test uses                 | FC stdlib today                              | Gap |
|------------------------------------------|----------------------------------------------|-----|
| Write/read files                         | `std::io` (`open`/`read`/`write`/`read_all`) | none |
| `glob("*.fc")`                           | `std::io.list_dir` + filter on `.fc`         | none |
| Read env var                             | `std::sys.env`                               | none |
| Temp dir                                 | `std::sys.temp_dir` + `std::io.mkdir`        | minor — need a unique name (pid / counter / `std::random`); Python's `mkdtemp` does this for free |
| Build JSON-RPC messages                  | string interpolation / concatenation         | none — the messages are simple literals |
| `Content-Length:` framing (read + write) | string scanning                              | none |
| **Spawn `fcc --lsp` + pipe I/O**         | —                                            | **YES — load-bearing gap** |
| **Parse JSON responses**                 | —                                            | partial — see Gap 2 |

Confirmed by survey (2026-06-26): no `popen`/`pipe`/`fork`/`exec`/`posix_spawn`/
`socketpair`/`waitpid`/`dup2` usage anywhere in `stdlib/`, `demos/`, `tests/`, or
`spec/examples.fc`; and no JSON anywhere in the stdlib or demos.

---

## Gap 1 — process spawning (the real blocker)

There is no process/pipe primitive anywhere in FC. `std::sys` exposes
`env`/`exit`/`time`/`sleep`/`get_pid` but nothing that starts another process.

**Why the bar is lower than it first looks.** The wire harness is *batch-style*:
it writes the *entire* framed input to stdin up front, then reads the *entire*
stdout at the end — no interleaving. (That's deliberate; it's how the test avoids
pipe deadlock.) Batch I/O means you do **not** need `fork` + `dup2` +
bidirectional pipes. You can instead:

1. write the framed input to a temp file,
2. `popen("fcc --lsp < /tmp/in", "r")`,
3. read all of stdout via the existing `std::io.read`.

This needs **two** new `extern` declarations — `popen` and `pclose` — and nothing
else, because `std::io` already models a `FILE*` as opaque `any*`
(`extern fopen/fread/fwrite/fclose` in `stdlib/io.fc`), so a `popen` handle drops
straight into `std::io.read`. (Close with `pclose`, not `fclose`.) POSIX-only,
which is fine: the C side of the LSP wire path is already `#if defined(_WIN32)`-
gated, and the Python test is POSIX in practice too.

The `std::net` module is the precedent to follow for the extern-wrapping style
(a `private module ... from <header>` block of `extern`s behind a clean FC API).

## Gap 2 — JSON

No JSON module exists. **But the test barely needs one.** Most assertions are
substring checks (e.g. `"int32" in json.dumps(hover)`), which become substring
scans over the raw response bytes. The handful of field-precise checks
(`range.start.line == 2`, `range.start.character == 12`) need either a small
hand-rolled scan or a real parser. So JSON is *avoidable* for this test, not a
blocker. A proper `std::data` JSON submodule would be the "clean" version but is a
larger, separable effort.

---

## The catch: independence is a feature here

The wire test is in Python **on purpose** — it's an *independent observer* of the
server. Rewriting it in FC means the test is compiled by the very compiler it is
testing. A codegen regression could then make the LSP test fail to build for
reasons unrelated to the LSP (muddying diagnosis), and in the worst case a
compiler bug could *mask* an LSP bug. For a conformance/wire test specifically,
that independence has real value and is the strongest argument for leaving this
particular test in Python even after we grow the spawn primitive.

---

## Recommendation (tiered)

1. **Best value — add the primitive, then maybe convert.** Add a small reusable
   `std::sys` (or new `std::proc`) spawn/capture API — wrapping `popen`/`pclose`
   to start, `posix_spawn` + pipes later if bidirectional/streaming is needed.
   The payoff is the *primitive*, not the test rewrite: a general-purpose systems
   language with no way to run a subprocess is a genuine hole, and this is a clean
   forcing function to fill it. Reusable in demos. Follow the `std::net`
   extern-wrapping pattern.
2. **Cheap proof-of-concept.** Rewrite `lsp_test.py` in FC with inline
   `extern popen/pclose`, temp-file redirect, and substring assertions. No stdlib
   change; proves FC can do it and surfaces ergonomics feedback — but it's
   throwaway POSIX plumbing in one test file.
3. **Dog-food elsewhere, keep the wire test in Python.** Add the `std::proc`
   capability and exercise it from a small demo, preserving the independent-
   observer property for the actual LSP test. This gets the stdlib win without
   sacrificing test independence.

If/when we pick this up: leaning toward (1) framed as "add `std::sys.spawn` /
`std::proc`," and separately deciding whether the LSP wire test *itself* adopts it
or stays in Python (the independence trade-off above). A JSON helper is a nice-to-
have, not a prerequisite.

---

## Pointers for whoever picks this up

- Test to convert / model after: `tests/lsp/lsp_test.py` (batch write-all /
  read-all session; assertions are mostly substring).
- Launcher (keep): `tests/lsp/run_lsp_tests.sh`; make target `test-lsp`
  (intentionally out of `make check`).
- `FILE*` modeling to reuse: `stdlib/io.fc` (`any*` handle, `extern fopen/fread/
  fwrite/fclose`).
- Extern-wrapping style to imitate: `stdlib/net.fc`.
- Existing `std::sys` surface to extend: `stdlib/sys.fc`
  (`env`/`exit`/`time`/`sleep`/`get_pid`/`temp_dir`/`home_dir`).
- The minimal viable spawn for *this* test = `extern popen`, `extern pclose`
  (POSIX), input via shell redirect from a temp file.
