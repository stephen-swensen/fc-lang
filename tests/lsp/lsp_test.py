#!/usr/bin/env python3
"""Wire-level tests for `fcc --lsp`.

Frames a JSON-RPC session over stdio, then asserts on the responses and
publishDiagnostics notifications. Kept separate from the compiler suite. The
most important assertion is server survival: a syntactically broken document
(which makes the lexer diag_fatal) must NOT kill the server.
"""
import json, subprocess, sys, tempfile

BIN = sys.argv[1]

def frame(obj):
    b = json.dumps(obj).encode()
    return b"Content-Length: " + str(len(b)).encode() + b"\r\n\r\n" + b

def req(i, method, params):  return {"jsonrpc": "2.0", "id": i, "method": method, "params": params}
def note(method, params):    return {"jsonrpc": "2.0", "method": method, "params": params}

# Use a fresh empty directory so the server's same-directory compilation-unit
# scan finds no unrelated sibling .fc files.
URI = "file://" + tempfile.mkdtemp(prefix="fc_lsp_") + "/doc.fc"

# Programs (0-based lines noted where used by positions below).
CLEAN = (
    "let helper = (n: i32) ->\n"      # line 0
    "    n + 1\n"                        # line 1
    "let main = (args: str[]) ->\n"     # line 2
    "    let x = helper(41)\n"           # line 3   ('x' col 8, 'helper' col 12)
    "    return x\n"                     # line 4
)
TYPEERR = (
    "let main = (args: str[]) ->\n"
    "    let z = 1 + true\n"             # line 1: arithmetic on bool
    "    return 0\n"
)
BROKEN = (
    "let main = (args: str[]) ->\n"
    '    let s = "unterminated\n'        # lexer diag_fatal -> must not kill server
    "    return 0\n"
)
UNICODE = (
    "let main = (args: str[]) ->\n"
    '    let greeting = "héllo wörld"\n' # multibyte bytes on this line
    "    let z = greeting\n"             # line 2: hover 'greeting' (col 12)
    "    return 0\n"
)
ASSERT_NOPAREN = (
    "let main = (args: str[]) ->\n"
    "    assert 1 == 1\n"                # missing parens: recovery once made the
    "    return 0\n"                     # assert text capture exit(1) ("out of
)                                        # memory") -- must not kill the server

def open_doc(v, text):  return note("textDocument/didOpen",
    {"textDocument": {"uri": URI, "languageId": "fc", "version": v, "text": text}})
def change(v, text):    return note("textDocument/didChange",
    {"textDocument": {"uri": URI, "version": v}, "contentChanges": [{"text": text}]})
def hover(i, l, c):     return req(i, "textDocument/hover",
    {"textDocument": {"uri": URI}, "position": {"line": l, "character": c}})

# Analysis is deferred: didOpen/didChange only mark the doc dirty, and the server
# analyzes (and publishes diagnostics) at the next idle point or before the next
# request — coalescing a burst of edits into one analysis (see the dedicated
# coalescing test near the end). So each revision whose diagnostics we want to
# observe is followed by a request, which forces a flush of THAT revision before
# the next change overwrites it; hence one publishDiagnostics per state, in order.
msgs = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    open_doc(1, CLEAN),
    hover(2, 3, 8),                                                      # flush CLEAN -> diag[0]; x -> i32
    hover(3, 3, 13),                                                     # helper -> func
    req(4, "textDocument/definition", {"textDocument": {"uri": URI}, "position": {"line": 3, "character": 13}}),
    req(5, "textDocument/codeLens", {"textDocument": {"uri": URI}}),
    req(6, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 4, "character": 4}}),
    req(10, "textDocument/inlayHint", {"textDocument": {"uri": URI},     # inline type hints
        "range": {"start": {"line": 0, "character": 0}, "end": {"line": 5, "character": 0}}}),
    change(2, TYPEERR),
    hover(11, 1, 8),                                                     # flush TYPEERR -> diag[1] (z on line 1)
    change(3, BROKEN),
    hover(7, 0, 4),                                                      # flush BROKEN -> diag[2]; SURVIVAL: must still reply
    change(4, UNICODE),
    hover(8, 2, 12),                                                     # flush UNICODE -> diag[3]; greeting after a multibyte line
    change(5, ASSERT_NOPAREN),
    hover(12, 0, 4),                                                     # flush ASSERT_NOPAREN -> diag[4]; SURVIVAL: must still reply
    req(9, "shutdown", None),
    note("exit", None),
]

def run_session(messages, env=None):
    """Spawn `fcc --lsp`, send framed messages, return (responses{id}, diags[],
    diags_by_file{name->[msgs]}, returncode, stderr)."""
    inp = b"".join(frame(m) for m in messages)
    p = subprocess.run([BIN, "--lsp"], input=inp, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=60, env=env)
    responses, diags, by_file = {}, [], {}
    out, i = p.stdout, 0
    while True:
        h = out.find(b"\r\n\r\n", i)
        if h < 0:
            break
        clen = None
        for line in out[i:h].decode().split("\r\n"):
            if line.lower().startswith("content-length:"):
                clen = int(line.split(":", 1)[1])
        obj = json.loads(out[h + 4:h + 4 + clen])
        i = h + 4 + clen
        if "id" in obj and "method" not in obj:
            responses[obj["id"]] = obj
        elif obj.get("method") == "textDocument/publishDiagnostics":
            diags.append(obj["params"]["diagnostics"])
            name = obj["params"]["uri"].split("/")[-1]
            by_file.setdefault(name, []).append([d["message"] for d in obj["params"]["diagnostics"]])
    return responses, diags, by_file, p.returncode, p.stderr

responses, diags, _, returncode, stderr = run_session(msgs)

failures = []
def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"  {status}  {name}" + (f"   {detail}" if (detail and not cond) else ""))
    if not cond:
        failures.append(name)

# --- assertions ---
caps = responses.get(1, {}).get("result", {}).get("capabilities", {})
check("initialize advertises capabilities",
      caps.get("hoverProvider") and caps.get("definitionProvider")
      and caps.get("completionProvider") and caps.get("codeLensProvider")
      and caps.get("inlayHintProvider") and caps.get("textDocumentSync") == 1, str(caps))
# `>` must be a trigger char so finishing `->` auto-pops member completion (not
# just Ctrl+Space); `.` and `:` cover value/module/type-name members and `::`.
_trig = caps.get("completionProvider", {}).get("triggerCharacters", [])
check("completion trigger characters include . : and > (for '->')",
      set(_trig) >= {".", ":", ">"}, str(_trig))

check("clean document has no diagnostics",
      len(diags) >= 1 and diags[0] == [], str(diags[0] if diags else None))

hx = responses.get(2, {}).get("result") or {}
check("hover 'x' shows i32",
      "i32" in json.dumps(hx), json.dumps(hx))

hh = responses.get(3, {}).get("result") or {}
check("hover 'helper' shows a function type",
      "->" in json.dumps(hh), json.dumps(hh))

dfn = responses.get(4, {}).get("result")
check("definition of 'helper' returns line 0",
      isinstance(dfn, dict) and dfn.get("range", {}).get("start", {}).get("line") == 0,
      json.dumps(dfn))

cl = responses.get(5, {}).get("result") or []
cl_titles = [x.get("command", {}).get("title", "") for x in cl]
check("codeLens returns type lenses",
      isinstance(cl, list) and len(cl) > 0 and any(": " in t for t in cl_titles),
      json.dumps(cl)[:200])
# Standalone CodeLens labels read fine tight: function bindings use ':-> ret'.
check("codeLens: function binding uses tight ':-> i32'",
      ":-> i32" in cl_titles, str(cl_titles))

comp = responses.get(6, {}).get("result") or {}
labels = [it.get("label") for it in (comp.get("items") if isinstance(comp, dict) else comp) or []]
check("completion includes keywords", "let" in labels and "match" in labels,
      str(labels[:10]))

ih = responses.get(10, {}).get("result") or []
check("inlayHint returns inline type hints (kind=Type)",
      isinstance(ih, list) and len(ih) >= 2
      and all(h.get("kind") == 1 and str(h.get("label", "")).startswith(":") for h in ih),
      json.dumps(ih)[:300])
check("inlayHint: non-function binding 'x' renders full ': i32' on its own line",
      any(h.get("position", {}).get("line") == 3 and h.get("label") == ": i32" for h in ih),
      json.dumps(ih)[:300])
# A lambda binding writes its param types at the site, so only the inferred
# return type is new: the hint is ': -> ret' (inline keeps a space after the
# colon to match the plain ': T' hints), not the full function type.
check("inlayHint: lambda binding shows return-only ': -> i32' (spaced inline)",
      any(h.get("label") == ": -> i32" for h in ih)
      and not any("(" in str(h.get("label", "")) for h in ih),
      json.dumps(ih)[:300])

check("type-error diagnostic reported",
      len(diags) >= 2 and any("numeric" in d["message"] or "bool" in d["message"]
                              for d in diags[1]), str(diags[1] if len(diags) > 1 else None))

check("broken document -> unterminated diagnostic",
      len(diags) >= 3 and any("unterminated" in d["message"] for d in diags[2]),
      str(diags[2] if len(diags) > 2 else None))

check("SERVER SURVIVED broken document (hover still replies)",
      7 in responses, "no response to hover after broken doc")

hg = responses.get(8, {}).get("result") or {}
rng = hg.get("range", {}) if isinstance(hg, dict) else {}
check("hover after a multibyte line maps correctly",
      "str" in json.dumps(hg) and rng.get("start", {}).get("line") == 2
      and rng.get("start", {}).get("character") == 12, json.dumps(hg))

check("assert-without-parens -> syntax diagnostic",
      len(diags) >= 5 and any("expected '('" in d["message"] for d in diags[4]),
      str(diags[4] if len(diags) > 4 else None))
check("SERVER SURVIVED assert-without-parens (hover still replies)",
      12 in responses, "no response to hover after assert-without-parens doc")

check("shutdown replies", 9 in responses)
check("clean process exit", returncode == 0, f"rc={returncode}")

if stderr.strip():
    sys.stderr.write("server stderr:\n" + stderr.decode() + "\n")

# --- multi-file compilation unit (sibling .fc files in the same directory) ---
import os
proj = tempfile.mkdtemp(prefix="fc_lsp_proj_")
with open(os.path.join(proj, "prelude.fc"), "w") as f:
    f.write("module prelude =\n    let double = (n: i32) ->\n        n * 2\n")
with open(os.path.join(proj, "main.fc"), "w") as f:
    f.write("import double from prelude\n\nlet main = (args: str[]) ->\n    let y = double(21)\n    return y\n")

def puri(name): return "file://" + os.path.join(proj, name)

# Open ONLY main.fc: prelude.fc must be picked up from disk so `prelude` resolves.
mf = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": puri("main.fc"), "languageId": "fc",
         "version": 1, "text": open(os.path.join(proj, "main.fc")).read()}}),
    note("textDocument/didOpen", {"textDocument": {"uri": puri("prelude.fc"), "languageId": "fc",
         "version": 1, "text": open(os.path.join(proj, "prelude.fc")).read()}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, by_file, _, _ = run_session(mf)
check("multi-file: main.fc resolves sibling 'prelude' module (no errors)",
      by_file.get("main.fc", [["?"]])[-1] == [], str(by_file.get("main.fc")))
check("multi-file: prelude.fc OK (let main provided by sibling)",
      by_file.get("prelude.fc", [["?"]])[-1] == [], str(by_file.get("prelude.fc")))

# --- cross-file diagnostic propagation: editing prelude.fc must refresh the
# diagnostics of the *other* open file (main.fc) that depends on it, WITHOUT
# main.fc itself being touched. Regression for the bug where a fix in prelude.fc
# left a stale error on main.fc until main.fc was edited: the idle flush now
# re-analyzes every open document when any one is dirty. Each edit below is
# followed by a request, forcing a discrete flush so we observe one
# publishDiagnostics per state (main.fc is never edited after didOpen). ---
proj2 = tempfile.mkdtemp(prefix="fc_lsp_xprop_")
PRE_OK  = "module prelude =\n    let double = (n: i32) ->\n        n * 2\n"
PRE_BAD = "module prelude =\n    let triple = (n: i32) ->\n        n * 2\n"  # no `double`
MAIN2   = ("import double from prelude\n\nlet main = (args: str[]) ->\n"
           "    let y = double(21)\n    return y\n")
with open(os.path.join(proj2, "prelude.fc"), "w") as f: f.write(PRE_OK)
with open(os.path.join(proj2, "main.fc"),    "w") as f: f.write(MAIN2)
def p2uri(name): return "file://" + os.path.join(proj2, name)
def p2open(name, v, text): return note("textDocument/didOpen",
    {"textDocument": {"uri": p2uri(name), "languageId": "fc", "version": v, "text": text}})
def p2change(name, v, text): return note("textDocument/didChange",
    {"textDocument": {"uri": p2uri(name), "version": v}, "contentChanges": [{"text": text}]})
def p2hover(i, name): return req(i, "textDocument/hover",
    {"textDocument": {"uri": p2uri(name)}, "position": {"line": 0, "character": 0}})
xp = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    p2open("main.fc", 1, MAIN2),
    p2open("prelude.fc", 1, PRE_OK),
    p2hover(2, "main.fc"),                 # flush #1: both clean -> main.fc []
    p2change("prelude.fc", 2, PRE_BAD),    # break the member main.fc imports
    p2hover(3, "prelude.fc"),              # flush #2: main.fc's import now errors
    p2change("prelude.fc", 3, PRE_OK),     # restore it (main.fc NOT touched)
    p2hover(4, "prelude.fc"),              # flush #3: main.fc's error must clear
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, xbf, _, _ = run_session(xp)
xmain = xbf.get("main.fc", [])
# Editing only prelude.fc republished main.fc's diagnostics 3 times (open, break,
# fix). On the buggy server main.fc was analyzed only at didOpen -> 1 publish.
check("cross-file: prelude.fc edits republish the dependent main.fc's diagnostics",
      len(xmain) >= 3, str(xmain))
check("cross-file: breaking prelude.fc surfaces an error on main.fc",
      any(len(d) > 0 for d in xmain), str(xmain))
check("cross-file: fixing prelude.fc clears main.fc's error without touching main.fc",
      xmain and xmain[-1] == [], str(xmain))

# --- project-wide diagnostics: a file that is NOT open in the editor but is part
# of the compilation unit must still receive diagnostics (and have them cascade on
# edits to the file it depends on). Here ONLY lib.fc is opened; app.fc lives on
# disk and depends on lib.fc. Breaking lib.fc must surface an error on app.fc even
# though app.fc was never opened; fixing lib.fc must clear it. ---
proj3 = tempfile.mkdtemp(prefix="fc_lsp_pw_")
LIB_OK  = "module lib =\n    let val = (n: i32) ->\n        n * 2\n"
LIB_BAD = "module lib =\n    let valx = (n: i32) ->\n        n * 2\n"   # `val` renamed away
APP     = ("import val from lib\n\nlet main = (args: str[]) ->\n"
           "    let y = val(21)\n    return y\n")
with open(os.path.join(proj3, "lib.fc"), "w") as f: f.write(LIB_OK)
with open(os.path.join(proj3, "app.fc"), "w") as f: f.write(APP)   # never opened
def p3uri(name): return "file://" + os.path.join(proj3, name)
def p3open(name, v, text): return note("textDocument/didOpen",
    {"textDocument": {"uri": p3uri(name), "languageId": "fc", "version": v, "text": text}})
def p3change(name, v, text): return note("textDocument/didChange",
    {"textDocument": {"uri": p3uri(name), "version": v}, "contentChanges": [{"text": text}]})
def p3hover(i, name): return req(i, "textDocument/hover",
    {"textDocument": {"uri": p3uri(name)}, "position": {"line": 0, "character": 0}})
pw = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    p3open("lib.fc", 1, LIB_OK),           # open ONLY lib.fc; app.fc stays on disk
    p3hover(2, "lib.fc"),                   # flush #1: app.fc resolves `val` -> clean
    p3change("lib.fc", 2, LIB_BAD),         # rename the member app.fc imports
    p3hover(3, "lib.fc"),                   # flush #2: app.fc (unopened) must error
    p3change("lib.fc", 3, LIB_OK),          # restore it
    p3hover(4, "lib.fc"),                   # flush #3: app.fc's error must clear
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, pwbf, _, _ = run_session(pw)
papp = pwbf.get("app.fc", [])
# app.fc is never opened, yet the server publishes diagnostics for it: absent while
# clean (flush #1 publishes only the open lib.fc), an error once lib.fc breaks, and
# an explicit clear once lib.fc is fixed.
check("project-wide: an unopened dependent file receives an error when its dep breaks",
      any(len(d) > 0 for d in papp), str(papp))
check("project-wide: the unopened dependent's error is cleared when the dep is fixed",
      papp and papp[-1] == [], str(papp))
# lib.fc itself is well-formed throughout (only app.fc's import breaks), so its own
# stream is clean the whole time — the error is attributed to the right file.
check("project-wide: the edited (open) file stays clean; the error lands on the dependent",
      all(d == [] for d in pwbf.get("lib.fc", [[]])), str(pwbf.get("lib.fc")))

# --- project-wide: a non-open file with its OWN pre-existing error is surfaced as
# soon as a sibling is opened (not only after an edit). ---
proj4 = tempfile.mkdtemp(prefix="fc_lsp_pw2_")
with open(os.path.join(proj4, "broken.fc"), "w") as f:
    f.write("module broken =\n    let f = (n: i32) ->\n        n + n\n    let g = nope\n")  # `nope` undefined
with open(os.path.join(proj4, "clean.fc"), "w") as f:
    f.write("let main = (args: str[]) ->\n    return 0\n")
pw2 = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": "file://" + os.path.join(proj4, "clean.fc"),
         "languageId": "fc", "version": 1, "text": open(os.path.join(proj4, "clean.fc")).read()}}),
    req(2, "textDocument/hover", {"textDocument": {"uri": "file://" + os.path.join(proj4, "clean.fc")},
         "position": {"line": 0, "character": 0}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, pw2bf, _, _ = run_session(pw2)
check("project-wide: an unopened sibling's own pre-existing error is surfaced on open",
      any(len(d) > 0 for d in pw2bf.get("broken.fc", [])), str(pw2bf.get("broken.fc")))
check("project-wide: the opened clean file has no diagnostics of its own",
      pw2bf.get("clean.fc", [["?"]])[-1] == [], str(pw2bf.get("clean.fc")))

# --- didClose with project-wide diagnostics: closing a file that still has an
# error and is still a unit member must NOT hide the error (it is republished for
# the now-unopened file); closing the LAST document clears everything. ---
proj5 = tempfile.mkdtemp(prefix="fc_lsp_close_")
with open(os.path.join(proj5, "err.fc"), "w") as f:
    f.write("module err =\n    let g = nope\n")          # `nope` undefined -> err.fc errors
with open(os.path.join(proj5, "ok.fc"), "w") as f:
    f.write("let main = (args: str[]) ->\n    return 0\n")
def c5uri(name): return "file://" + os.path.join(proj5, name)
def c5open(name): return note("textDocument/didOpen", {"textDocument": {
    "uri": c5uri(name), "languageId": "fc", "version": 1,
    "text": open(os.path.join(proj5, name)).read()}})
def c5close(name): return note("textDocument/didClose",
    {"textDocument": {"uri": c5uri(name)}})
cl5 = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    c5open("ok.fc"),
    c5open("err.fc"),
    req(2, "textDocument/hover", {"textDocument": {"uri": c5uri("ok.fc")},   # flush: err.fc errors
        "position": {"line": 0, "character": 0}}),
    c5close("err.fc"),          # still a broken unit member on disk -> error must persist
    c5close("ok.fc"),           # last document closed -> clear everything
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, c5bf, _, _ = run_session(cl5)
cerr = c5bf.get("err.fc", [])
# err.fc: errored while open, STILL errored right after it was closed (index 1),
# then cleared ([]) when the final document closed.
check("didClose: a still-broken, still-referenced closed file keeps its error",
      len(cerr) >= 2 and cerr[0] != [] and cerr[1] != [], str(cerr))
check("didClose: closing the last document clears project-wide diagnostics",
      cerr and cerr[-1] == [], str(cerr))

# --- stdlib feed: every module resolves (regression for the hardcoded list) ---
sd = tempfile.mkdtemp(prefix="fc_lsp_std_")
sprog = ("import io from std::\nimport random from std::\nimport text from std::\n\n"
         "let main = (args: str[]) ->\n    return 0\n")
sl = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": "file://" + sd + "/m.fc",
         "languageId": "fc", "version": 1, "text": sprog}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, sbf, _, _ = run_session(sl)
check("stdlib feed resolves std::random / io / text",
      sbf.get("m.fc", [["?"]])[-1] == [], str(sbf.get("m.fc")))

# --- library mode: a document with no `let main` is tolerated (no entry point) ---
# The CLI requires `let main`, but the server analyzes library code (e.g. a stdlib
# module being edited) that has no entry point. It must not be flagged.
libns = tempfile.mkdtemp(prefix="fc_lsp_libns_")
LIBNS = (
    "namespace mylib::\n\n"
    "module util =\n"
    "    let triple = (n: i32) ->\n"
    "        n * 3\n"
)
ln = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": "file://" + libns + "/util.fc",
         "languageId": "fc", "version": 1, "text": LIBNS}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, lnbf, _, _ = run_session(ln)
check("library module (no `let main`) is not flagged as missing entry point",
      lnbf.get("util.fc", [["?"]])[-1] == [], str(lnbf.get("util.fc")))

# A global file with top-level `let` bindings and no main: the entry-point-file
# restriction can't apply (there is no entry file), so it must be suppressed too,
# rather than flagging every top-level `let`.
libg = tempfile.mkdtemp(prefix="fc_lsp_libg_")
LIBG = (
    "let answer = 42\n"
    "let greet = (name: str) ->\n"
    "    name\n"
)
lg = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": "file://" + libg + "/lib.fc",
         "languageId": "fc", "version": 1, "text": LIBG}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, lgbf, _, _ = run_session(lg)
check("library globals (top-level `let`, no main) are not flagged",
      lgbf.get("lib.fc", [["?"]])[-1] == [], str(lgbf.get("lib.fc")))

# --- opening a stdlib file the feed already provides (the Go To Definition case) ---
# The server merges the installed stdlib into every analysis. Following Go To
# Definition into a stdlib symbol opens the very file the feed provides, so the
# same module would be analyzed twice -> pass1 "redefinition" -> pass2 never runs
# -> no diagnostics AND no type info (hover / CodeLens silently empty). The feed
# must drop the entry that is the SAME on-disk file as the open document.
import glob
std_dir = os.path.abspath("stdlib")
cand = next((f for f in sorted(glob.glob(os.path.join(std_dir, "*.fc")))
             if "let " in open(f).read()), None)
check("found a stdlib file to open", cand is not None, std_dir)
if cand:
    curi = "file://" + cand
    se = [
        req(1, "initialize", {"capabilities": {}}),
        note("initialized", {}),
        note("textDocument/didOpen", {"textDocument": {"uri": curi, "languageId": "fc",
             "version": 1, "text": open(cand).read()}}),
        req(2, "textDocument/codeLens", {"textDocument": {"uri": curi}}),
        req(9, "shutdown", None),
        note("exit", None),
    ]
    resp_se, _, sebf, _, _ = run_session(se)
    name = os.path.basename(cand)
    check("opening a feed-provided stdlib file: no spurious redefinition diagnostics",
          sebf.get(name, [["?"]])[-1] == [], str(sebf.get(name)))
    lenses = resp_se.get(2, {}).get("result") or []
    check("opening a feed-provided stdlib file: type CodeLens still provided",
          isinstance(lenses, list) and len(lenses) > 0,
          str(len(lenses) if isinstance(lenses, list) else lenses))

# --- a project file named like a stdlib module must NOT shadow that std:: module ---
# Dedup keys on canonical path, not basename: a workspace `data.fc` is a DIFFERENT
# on-disk file than the stdlib's `data.fc`, so the feed's `std::data` must survive.
# (Basename dedup would drop it and break `import data from std::`.)
ns = tempfile.mkdtemp(prefix="fc_lsp_ownname_")
with open(os.path.join(ns, "data.fc"), "w") as f:        # same basename as std's data.fc
    f.write("module mydata =\n    let f = (x: i32) ->\n        x\n")
with open(os.path.join(ns, "main.fc"), "w") as f:
    f.write("import data from std::\n\nlet main = (args: str[]) ->\n    return 0\n")
nm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": "file://" + os.path.join(ns, "main.fc"),
         "languageId": "fc", "version": 1, "text": open(os.path.join(ns, "main.fc")).read()}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, nbf, _, _ = run_session(nm)
check("own file named like a stdlib module: `import data from std::` still resolves",
      nbf.get("main.fc", [["?"]])[-1] == [], str(nbf.get("main.fc")))

# --- content dedup: a byte-identical COPY of a stdlib file opened at a DIFFERENT
# path must still type-check. The feed provides the stdlib's own copy (a different
# realpath), so a path-only dedup would miss it -> the module is merged twice ->
# pass1 redefinition -> pass2 gated -> empty CodeLens/hover with the error filtered
# out (a silently blank editor). Content-identity dedup collapses the two copies.
# This is the regression guard for the former "-O0 data.fc CodeLens empty" issue,
# which was really a path-mismatch dedup miss (installed stdlib vs. opened file).
if cand:
    cpdir = tempfile.mkdtemp(prefix="fc_lsp_copy_")
    cpath = os.path.join(cpdir, os.path.basename(cand))      # same basename, different dir
    with open(cpath, "w") as f:
        f.write(open(cand).read())                           # byte-identical content
    cpuri = "file://" + cpath
    cp = [
        req(1, "initialize", {"capabilities": {}}),
        note("initialized", {}),
        note("textDocument/didOpen", {"textDocument": {"uri": cpuri, "languageId": "fc",
             "version": 1, "text": open(cpath).read()}}),
        req(2, "textDocument/codeLens", {"textDocument": {"uri": cpuri}}),
        req(9, "shutdown", None),
        note("exit", None),
    ]
    cresp, _, cbf, _, _ = run_session(cp)
    cname = os.path.basename(cpath)
    check("identical stdlib copy at a different path: no spurious redefinition diagnostics",
          cbf.get(cname, [["?"]])[-1] == [], str(cbf.get(cname)))
    clenses = cresp.get(2, {}).get("result") or []
    check("identical stdlib copy at a different path: type CodeLens still provided",
          isinstance(clenses, list) and len(clenses) > 0,
          str(len(clenses) if isinstance(clenses, list) else clenses))

# --- ungated pass2 (Item 2): an error in a MERGED sibling file no longer blanks the
# open file. pass2 now runs past a recoverable pass1 error (here, a duplicate top-level
# name in the sibling), so the open document still type-checks — its overlays stay live
# and there is NO "analysis incomplete" diagnostic. The sibling's own error lives in
# another file and is filtered out of the open file's diagnostics. (This is the payoff
# the old "safety net" diagnostic existed only to explain.)
sndir = tempfile.mkdtemp(prefix="fc_lsp_ungated_")
with open(os.path.join(sndir, "broken.fc"), "w") as f:       # duplicate top-level name -> pass1 error
    f.write("let dup = (n: i32) ->\n    n\nlet dup = (n: i32) ->\n    n\n")
with open(os.path.join(sndir, "main.fc"), "w") as f:         # clean; must still type-check despite the sibling
    f.write("let inc = (n: i32) ->\n    n + 1\n")
snuri = "file://" + os.path.join(sndir, "main.fc")
sn = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": snuri,
         "languageId": "fc", "version": 1, "text": open(os.path.join(sndir, "main.fc")).read()}}),
    req(2, "textDocument/codeLens", {"textDocument": {"uri": snuri}}),
    req(9, "shutdown", None),
    note("exit", None),
]
snresp, _, snbf, _, _ = run_session(sn)
sn_msgs = snbf.get("main.fc", [[]])[-1]
check("ungated pass2: a merged sibling's recoverable error no longer blanks the open file (no 'analysis incomplete')",
      not any("analysis incomplete" in m for m in sn_msgs), str(sn_msgs))
sn_lenses = snresp.get(2, {}).get("result") or []
check("ungated pass2: the open file still type-checks despite the sibling error (CodeLens present)",
      isinstance(sn_lenses, list) and len(sn_lenses) > 0,
      str(len(sn_lenses) if isinstance(sn_lenses, list) else sn_lenses))

# --- ungated pass2 + error-recovery parsing (the headline payoff): a buffer with a
# syntactically broken line mid-function still answers hover / CodeLens on the OTHER,
# well-formed lines — served from the FRESH analysis (not stale fallback). Before this
# work the broken line aborted the parse (or gated pass2), blanking the whole file.
brkuri = "file:///tmp/fc_lsp_broken_live.fc"
BROKEN_LIVE = (
    "let helper = (n: i32) ->\n"   # line 0
    "    n + 1\n"                   # line 1
    "let main = () ->\n"           # line 2
    "    let a = helper(2)\n"      # line 3: 'helper' starts at col 12 — hover here
    "    let b =\n"                # line 4: BROKEN (missing RHS) — recovered, not fatal
    "    let c = a\n"              # line 5
    "    c\n"                       # line 6
)
bl = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": brkuri, "languageId": "fc",
         "version": 1, "text": BROKEN_LIVE}}),
    req(2, "textDocument/hover", {"textDocument": {"uri": brkuri},
         "position": {"line": 3, "character": 13}}),       # hover 'helper' on a valid line
    req(3, "textDocument/codeLens", {"textDocument": {"uri": brkuri}}),
    req(9, "shutdown", None),
    note("exit", None),
]
blresp, _, blbf, _, _ = run_session(bl)
bl_hover = (blresp.get(2, {}).get("result") or {}).get("contents", {})
bl_hover = bl_hover.get("value", "") if isinstance(bl_hover, dict) else str(bl_hover)
check("recovery payoff: hover on a valid line works despite a broken line in the same file",
      "i32" in bl_hover, repr(bl_hover))
bl_lenses = blresp.get(3, {}).get("result") or []
check("recovery payoff: CodeLens still provided for a file with a broken line",
      isinstance(bl_lenses, list) and len(bl_lenses) > 0,
      str(len(bl_lenses) if isinstance(bl_lenses, list) else bl_lenses))
bl_msgs = blbf.get("fc_lsp_broken_live.fc", [[]])[-1]
check("recovery payoff: the broken line still produces a diagnostic (squiggle stays live)",
      len(bl_msgs) > 0, str(bl_msgs))

# --- go-to-definition beyond plain identifiers: module members, struct-literal
# type names, and union variant constructors all resolve to their declarations.
gd = tempfile.mkdtemp(prefix="fc_lsp_gotodef_")
GOTODEF = (
    "module mathx =\n"                          # line 0
    "    let twice = (n: i32) ->\n"           # line 1: def of twice
    "        n * 2\n"                            # line 2
    "struct point =\n"                          # line 3: def of point
    "    x: i32\n"                            # line 4
    "    y: i32\n"                            # line 5
    "union shape =\n"                           # line 6: def of shape
    "    | circle(i32)\n"                     # line 7
    "    | empty\n"                             # line 8
    "let main = (args: str[]) ->\n"             # line 9
    "    let a = mathx.twice(21)\n"             # line 10: 'mathx' col 12, 'twice' col 18
    "    let p = point{x = 1, y = 2}\n"         # line 11: 'point' col 12
    "    let s = shape.circle(3)\n"             # line 12: 'shape' col 12, 'circle' col 18
    "    return a\n"                            # line 13
)
guri = "file://" + os.path.join(gd, "doc.fc")
def gdef(i, l, c): return req(i, "textDocument/definition",
    {"textDocument": {"uri": guri}, "position": {"line": l, "character": c}})
gm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": guri, "languageId": "fc",
         "version": 1, "text": GOTODEF}}),
    gdef(2, 10, 19),   # 'twice'  -> module member decl, line 1
    gdef(3, 11, 13),   # 'point'  -> struct decl, line 3
    gdef(4, 12, 20),   # 'circle' -> union decl, line 6
    gdef(5, 10, 13),   # 'mathx'  -> module decl, line 0 (regression guard)
    req(9, "shutdown", None),
    note("exit", None),
]
gresp, _, _, _, _ = run_session(gm)
def def_line(iid):
    r = gresp.get(iid, {}).get("result")
    return r.get("range", {}).get("start", {}).get("line") if isinstance(r, dict) else None
check("go-to-def on module member 'mathx.twice' -> member declaration (line 1)",
      def_line(2) == 1, str(gresp.get(2, {}).get("result")))
check("go-to-def on struct-literal type 'point' -> struct declaration (line 3)",
      def_line(3) == 3, str(gresp.get(3, {}).get("result")))
check("go-to-def on variant constructor 'shape.circle' -> union declaration (line 6)",
      def_line(4) == 6, str(gresp.get(4, {}).get("result")))
check("go-to-def on module name 'mathx' -> module declaration (line 0)",
      def_line(5) == 0, str(gresp.get(5, {}).get("result")))

# --- go-to-definition for block-locals: parameters, lets, and uses resolve to
# the binding's own name location (not just module-level symbols).
ld = tempfile.mkdtemp(prefix="fc_lsp_locals_")
LOCALS = (
    "let f = (x: i32) ->\n"             # line 0: param 'x' name at char 9
    "    x + 1\n"                        # line 1: 'x' use at char 4
    "let main = (args: str[]) ->\n"      # line 2
    "    let count = 10\n"               # line 3: 'count' name at char 8
    "    let r = count + f(2)\n"          # line 4: 'count' use char 12, 'r' name char 8
    "    return r\n"                      # line 5: 'r' use at char 11
)
luri = "file://" + os.path.join(ld, "doc.fc")
def ldef(i, l, c): return req(i, "textDocument/definition",
    {"textDocument": {"uri": luri}, "position": {"line": l, "character": c}})
lm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": luri, "languageId": "fc",
         "version": 1, "text": LOCALS}}),
    ldef(2, 1, 4),     # 'x'     -> param def, line 0
    ldef(3, 4, 12),    # 'count' -> let def, line 3
    ldef(4, 5, 11),    # 'r'     -> let def, line 4
    req(9, "shutdown", None),
    note("exit", None),
]
lresp, _, _, _, _ = run_session(lm)
def lline(iid):
    r = lresp.get(iid, {}).get("result")
    return r.get("range", {}).get("start", {}).get("line") if isinstance(r, dict) else None
check("go-to-def on parameter use 'x' -> parameter declaration (line 0)",
      lline(2) == 0, str(lresp.get(2, {}).get("result")))
check("go-to-def on local use 'count' -> let binding (line 3)",
      lline(3) == 3, str(lresp.get(3, {}).get("result")))
check("go-to-def on local use 'r' -> let binding (line 4)",
      lline(4) == 4, str(lresp.get(4, {}).get("result")))

# --- exact field-name targeting + plain-struct-field go-to-definition + the
# field's trailing doc comment. Spacing around the dot must not mislocate.
fd = tempfile.mkdtemp(prefix="fc_lsp_fields_")
FIELDS = (
    "struct point =\n"                       # line 0
    "    x: i32  // the abscissa\n"           # line 1: field 'x' + trailing comment
    "    y: i32\n"                            # line 2: field 'y'
    "let main = (args: str[]) ->\n"           # line 3
    "    let p = point{x = 1, y = 2}\n"       # line 4
    "    let q = p . x\n"                      # line 5: spaced access, 'x' at char 16
    "    return q\n"                           # line 6
)
furi = "file://" + os.path.join(fd, "doc.fc")
fm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": furi, "languageId": "fc",
         "version": 1, "text": FIELDS}}),
    req(2, "textDocument/definition",
        {"textDocument": {"uri": furi}, "position": {"line": 5, "character": 16}}),
    req(3, "textDocument/hover",
        {"textDocument": {"uri": furi}, "position": {"line": 5, "character": 16}}),
    req(9, "shutdown", None),
    note("exit", None),
]
fresp2, _, _, _, _ = run_session(fm)
fr = fresp2.get(2, {}).get("result")
check("go-to-def on spaced field access 'p . x' -> field declaration (line 1)",
      isinstance(fr, dict) and fr.get("range", {}).get("start", {}).get("line") == 1,
      str(fr))
fhov = (fresp2.get(3, {}).get("result") or {}).get("contents", {}).get("value", "")
check("hover on field 'x' targets it (type i32) despite spaces",
      "x: i32" in fhov, fhov)
check("hover on field 'x' includes its trailing doc comment",
      "the abscissa" in fhov, fhov)

# --- doc-comment hover: a run of `//` lines above a definition is shown on hover
# (top-level symbol and block-local binding).
dd = tempfile.mkdtemp(prefix="fc_lsp_docs_")
DOCS = (
    "// returns n doubled\n"              # line 0
    "// (second line of the doc)\n"       # line 1
    "let twice = (n: i32) ->\n"           # line 2: 'twice' def
    "    n * 2\n"                          # line 3
    "let main = (args: str[]) ->\n"       # line 4
    "    // running total\n"              # line 5
    "    let total = 0\n"                  # line 6: 'total' def
    "    let r = twice(total)\n"           # line 7: 'twice' use char 12, 'total' use char 18
    "    return r\n"                       # line 8
)
duri = "file://" + os.path.join(dd, "doc.fc")
dm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": duri, "languageId": "fc",
         "version": 1, "text": DOCS}}),
    req(2, "textDocument/hover",
        {"textDocument": {"uri": duri}, "position": {"line": 7, "character": 12}}),  # 'twice'
    req(3, "textDocument/hover",
        {"textDocument": {"uri": duri}, "position": {"line": 7, "character": 18}}),  # 'total'
    req(9, "shutdown", None),
    note("exit", None),
]
dresp, _, _, _, _ = run_session(dm)
def dval(iid):
    return (dresp.get(iid, {}).get("result") or {}).get("contents", {}).get("value", "")
check("hover on top-level 'twice' shows the doc comment above its definition",
      "returns n doubled" in dval(2) and "second line of the doc" in dval(2), dval(2))
check("hover on block-local 'total' shows the comment above its let binding",
      "running total" in dval(3), dval(3))

# --- parameter hover: a function parameter renders as `name: type` with NO
# doc-comment scan, consistently whether it sits on the function's decl line or a
# continuation line. A param has no doc of its own — the line above it holds the
# function decl or an earlier param, never the param's doc — so the old behavior
# of decl-line params "inheriting" the function's doc (while continuation-line
# params didn't) was an accident of layout. Go-to-def still resolves the param.
pd = tempfile.mkdtemp(prefix="fc_lsp_param_")
PARAMS = (
    "// doubles and offsets\n"                   # line 0: function doc comment
    "// (second line of the doc)\n"              # line 1
    "let scale = (factor: i32, base: i32,\n"     # line 2: 'factor' name at char 13
    "             extra: i32) ->\n"              # line 3: 'extra' name at char 13
    "    factor * base + extra\n"                # line 4: 'factor' char 4, 'extra' char 20
    "let main = (args: str[]) ->\n"              # line 5
    "    let r = scale(2, 3, 4)\n"               # line 6
    "    return r\n"                             # line 7
)
puri = "file://" + os.path.join(pd, "doc.fc")
def phov(i, l, c): return req(i, "textDocument/hover",
    {"textDocument": {"uri": puri}, "position": {"line": l, "character": c}})
def pdef(i, l, c): return req(i, "textDocument/definition",
    {"textDocument": {"uri": puri}, "position": {"line": l, "character": c}})
pm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": puri, "languageId": "fc",
         "version": 1, "text": PARAMS}}),
    phov(2, 4, 4),      # 'factor' use — decl-line param
    phov(3, 4, 20),     # 'extra'  use — continuation-line param
    pdef(4, 4, 4),      # 'factor' use -> its declaration (line 2)
    pdef(5, 4, 20),     # 'extra'  use -> its declaration (line 3)
    req(9, "shutdown", None),
    note("exit", None),
]
presp, _, _, _, _ = run_session(pm)
def pval(iid):
    return (presp.get(iid, {}).get("result") or {}).get("contents", {}).get("value", "")
def pline(iid):
    r = presp.get(iid, {}).get("result")
    return r.get("range", {}).get("start", {}).get("line") if isinstance(r, dict) else None
check("hover on decl-line param 'factor' shows 'factor: i32'",
      "factor: i32" in pval(2), pval(2))
check("hover on decl-line param 'factor' does NOT inherit the function's doc comment",
      "doubles and offsets" not in pval(2), pval(2))
check("hover on continuation-line param 'extra' shows 'extra: i32'",
      "extra: i32" in pval(3), pval(3))
check("hover on continuation-line param 'extra' has no doc comment (consistent with decl-line)",
      "doubles and offsets" not in pval(3), pval(3))
check("go-to-def on decl-line param 'factor' -> its declaration (line 2)",
      pline(4) == 2, str(presp.get(4, {}).get("result")))
check("go-to-def on continuation-line param 'extra' -> its declaration (line 3)",
      pline(5) == 3, str(presp.get(5, {}).get("result")))

# --- stale-overlay retention: typing through a transient unrecoverable state
# (a parse abort, or a pass1 error that gates pass2) must NOT blank type-aware
# overlays. The fresh analysis still drives diagnostics (the squiggle stays
# live), but hover / CodeLens fall back to the last analysis that type-checked,
# so they don't flicker off every other keystroke. Regression guard for the
# "type info disappears while typing `let r2 = ...`" report.
st = tempfile.mkdtemp(prefix="fc_lsp_stale_")
STALE_BASE = (
    "let helper = (n: i32) ->\n"     # line 0
    "    n + 1\n"                      # line 1
    "let main = (args: str[]) ->\n"   # line 2
    "    let x = helper(41)\n"         # line 3: 'x' at char 8
    "    return x\n"                   # line 4
)
# Identical first five lines (so unchanged positions still resolve against the
# retained AST), plus a trailing unterminated string: a lexer diag_fatal that
# longjmps out -> program is NULL, the harshest of the degraded states.
STALE_BROKEN = STALE_BASE + 'let broken = "unterminated\n'   # line 5
suri = "file://" + os.path.join(st, "doc.fc")
def s_cl(i):  return req(i, "textDocument/codeLens", {"textDocument": {"uri": suri}})
def s_ch(v, t): return note("textDocument/didChange",
    {"textDocument": {"uri": suri, "version": v}, "contentChanges": [{"text": t}]})
sm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": suri, "languageId": "fc",
         "version": 1, "text": STALE_BASE}}),
    s_cl(2),                                                              # baseline lenses
    s_ch(2, STALE_BROKEN),                                               # -> parse abort (program NULL)
    s_cl(3),                                                             # must still answer (stale)
    req(4, "textDocument/hover",
        {"textDocument": {"uri": suri}, "position": {"line": 3, "character": 8}}),  # 'x' -> i32 (stale)
    s_ch(3, STALE_BASE),                                                 # recover
    s_cl(5),                                                            # fresh again
    req(9, "shutdown", None),
    note("exit", None),
]
sresp, _, sbf2, _, _ = run_session(sm)
base_lenses  = sresp.get(2, {}).get("result") or []
stale_lenses = sresp.get(3, {}).get("result") or []
stale_hover  = json.dumps(sresp.get(4, {}).get("result") or {})
recov_lenses = sresp.get(5, {}).get("result") or []
all_doc_msgs = [m for lst in sbf2.get("doc.fc", []) for m in lst]
check("stale retention: baseline CodeLens present",
      isinstance(base_lenses, list) and len(base_lenses) > 0, str(len(base_lenses)))
check("stale retention: CodeLens survives a parse-abort edit (served from last good)",
      isinstance(stale_lenses, list) and len(stale_lenses) == len(base_lenses),
      f"base={len(base_lenses)} stale={len(stale_lenses) if isinstance(stale_lenses, list) else stale_lenses}")
check("stale retention: hover on an unchanged line still resolves (i32)",
      "i32" in stale_hover, stale_hover)
check("stale retention: the broken edit's diagnostic is still reported (squiggle stays live)",
      any("unterminated" in m for m in all_doc_msgs), str(sbf2.get("doc.fc")))
check("stale retention: CodeLens refreshes after recovery",
      isinstance(recov_lenses, list) and len(recov_lenses) > 0, str(len(recov_lenses)))

# --- lsp.rsp: a response file discovered by walking up from the open file pins
# the compilation unit to its (globbed, file-relative) inputs across subdirs,
# exactly like `fcc @lsp.rsp` — resolving cross-directory imports the flat
# sibling-glob heuristic cannot reach, with no blanket stdlib feed. ---
def rwrite(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)

UTIL = "namespace acme::\n\nmodule util =\n    let helper = (x: i32) ->\n        x * 2\n"
MAIN = ("import helper from acme::util\n\n"
        "let main = (args: str[]) ->\n    assert(helper(21) == 42)\n    0\n")

def open_main_session(proj):
    mp = os.path.join(proj, "src", "main.fc")
    return [
        req(1, "initialize", {"capabilities": {}}),
        note("initialized", {}),
        note("textDocument/didOpen", {"textDocument": {
            "uri": "file://" + mp, "languageId": "fc", "version": 1,
            "text": open(mp).read()}}),
        req(9, "shutdown", None),
        note("exit", None),
    ]

# 1) with lsp.rsp globbing src/ and lib/, the cross-dir import resolves cleanly.
rp = tempfile.mkdtemp(prefix="fc_lsp_rsp_")
rwrite(os.path.join(rp, "src", "main.fc"), MAIN)
rwrite(os.path.join(rp, "lib", "util.fc"), UTIL)
rwrite(os.path.join(rp, "lsp.rsp"), "# project unit\nsrc/*.fc\nlib/*.fc\n")
_, _, rbf, _, _ = run_session(open_main_session(rp))
rmsgs = rbf.get("main.fc", [["?"]])[-1]
check("lsp.rsp: cross-directory import resolves (no diagnostics)", rmsgs == [], str(rmsgs))

# 2) negative control: identical layout WITHOUT lsp.rsp — the sibling glob sees
# only src/, so the import into lib/ is unresolved (proves the rsp did the work).
npj = tempfile.mkdtemp(prefix="fc_lsp_norsp_")
rwrite(os.path.join(npj, "src", "main.fc"), MAIN)
rwrite(os.path.join(npj, "lib", "util.fc"), UTIL)
_, _, nbf, _, _ = run_session(open_main_session(npj))
nmsgs = nbf.get("main.fc", [[]])[-1]
check("no lsp.rsp: cross-directory import is unresolved (control)", len(nmsgs) > 0, str(nmsgs))

# 3) a broken lsp.rsp (references a missing @file) must not silently behave as if
# absent: fall back to the heuristic AND surface one 'lsp.rsp ignored' note.
bp = tempfile.mkdtemp(prefix="fc_lsp_rspbad_")
rwrite(os.path.join(bp, "src", "main.fc"), "let main = (args: str[]) ->\n    return 0\n")
rwrite(os.path.join(bp, "lsp.rsp"), "@does_not_exist.rsp\n")
_, _, bbf, _, _ = run_session(open_main_session(bp))
bmsgs = bbf.get("main.fc", [["?"]])[-1]
check("broken lsp.rsp surfaces an 'lsp.rsp ignored' diagnostic (fallback)",
      any("lsp.rsp ignored" in m for m in bmsgs), str(bmsgs))

# 4) project-wide diagnostics through an lsp.rsp unit (the euler-fc setup): open
# ONLY lib.fc; app.fc is a unit member via the rsp glob but never opened. Breaking
# lib.fc must surface app.fc's resulting error even though app.fc is not open, and
# fixing lib.fc must clear it — the lsp.rsp path, distinct from the sibling glob. ---
pwr = tempfile.mkdtemp(prefix="fc_lsp_rsp_pw_")
rwrite(os.path.join(pwr, "lsp.rsp"), "*.fc\n")
rwrite(os.path.join(pwr, "lib.fc"), LIB_OK)          # reused from the heuristic test
rwrite(os.path.join(pwr, "app.fc"), APP)             # imports `val` from lib; not opened
def pwr_uri(name): return "file://" + os.path.join(pwr, name)
def pwr_open(v, text): return note("textDocument/didOpen",
    {"textDocument": {"uri": pwr_uri("lib.fc"), "languageId": "fc", "version": v, "text": text}})
def pwr_change(v, text): return note("textDocument/didChange",
    {"textDocument": {"uri": pwr_uri("lib.fc"), "version": v}, "contentChanges": [{"text": text}]})
def pwr_hover(i): return req(i, "textDocument/hover",
    {"textDocument": {"uri": pwr_uri("lib.fc")}, "position": {"line": 0, "character": 0}})
pwrs = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    pwr_open(1, LIB_OK),
    pwr_hover(2),                    # flush #1: app.fc resolves `val` -> clean
    pwr_change(2, LIB_BAD),
    pwr_hover(3),                    # flush #2: unopened app.fc errors
    pwr_change(3, LIB_OK),
    pwr_hover(4),                    # flush #3: app.fc clears
    req(9, "shutdown", None),
    note("exit", None),
]
_, _, pwrbf, _, _ = run_session(pwrs)
pwrapp = pwrbf.get("app.fc", [])
check("lsp.rsp project-wide: an unopened unit member errors when its dep breaks",
      any(len(d) > 0 for d in pwrapp), str(pwrapp))
check("lsp.rsp project-wide: the unopened member's error clears when the dep is fixed",
      pwrapp and pwrapp[-1] == [], str(pwrapp))

# --- built-in intrinsic hover: alloc/alloca/free/some/none/default/sizeof/
# alignof/assert/atomics and the stdin/stdout/stderr globals are not user
# declarations, so they carry curated documentation surfaced on hover.
bid = tempfile.mkdtemp(prefix="fc_lsp_builtins_")
BUILTIN_LINES = [
    "struct point =",                                  # 0
    "    x: i32",                                       # 1
    "    y: i32",                                       # 2
    "let main = (args: str[]) ->",                      # 3
    "    let p = alloc(point)",                         # 4
    "    defer free(p!)",                               # 5
    "    let q = some(42)",                             # 6
    "    let r = none(i32)",                            # 7
    "    let d = default(point)",                       # 8
    "    let sz = sizeof(point)",                       # 9
    "    let al = alignof(point)",                      # 10
    "    let buf = alloca(i32, 4)",                     # 11
    "    let mut counter = 0",                          # 12
    "    atomic_store_release(&counter, 1)",            # 13
    "    let cur = atomic_load_acquire(&counter)",      # 14
    "    assert(cur == 1)",                             # 15
    "    let h = stdout",                               # 16
    "    return 0",                                     # 17
]
BUILTINS = "\n".join(BUILTIN_LINES) + "\n"
biuri = "file://" + os.path.join(bid, "doc.fc")
# (request id, 0-based line, keyword on that line, a phrase only its doc contains)
probes = [
    (20, 4,  "alloc",                "Allocates `T` on the **heap**"),
    (21, 5,  "free",                 "Releases heap memory"),
    (22, 6,  "some",                 "Wraps a present value"),
    (23, 7,  "none",                 "The **empty** option"),
    (24, 8,  "default",              "the **zero value**"),
    (25, 9,  "sizeof",               "The size of type"),
    (26, 10, "alignof",              "The alignment requirement"),
    (27, 11, "alloca",               "on the **dynamic stack**"),
    (28, 13, "atomic_store_release", "acquire-loads"),
    (29, 14, "atomic_load_acquire",  "visible afterward"),
    (30, 15, "assert",               "calls `abort()`"),
    (31, 16, "stdout",               "standard output stream"),
]
bm = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": biuri, "languageId": "fc",
         "version": 1, "text": BUILTINS}}),
]
for (rid, ln, kw, _phrase) in probes:
    col = BUILTIN_LINES[ln].find(kw) + 1   # one char into the keyword
    bm.append(req(rid, "textDocument/hover",
                  {"textDocument": {"uri": biuri}, "position": {"line": ln, "character": col}}))
bm += [req(9, "shutdown", None), note("exit", None)]
bresp, _, _, _, _ = run_session(bm)
def bval(rid):
    return (bresp.get(rid, {}).get("result") or {}).get("contents", {}).get("value", "")
for (rid, ln, kw, phrase) in probes:
    v = bval(rid)
    check(f"builtin hover '{kw}' is rendered as an fc-fenced doc",
          v.startswith("```fc"), v[:80])
    check(f"builtin hover '{kw}' contains its documentation",
          phrase in v, v)
# The generic signature appears in the fence, and the concrete result type of the
# occurrence is appended for non-void builtins (here alloc(point) -> point*?).
av = bval(20)
check("builtin hover 'alloc' shows the generic signature 'alloc(T) -> T*?'",
      "alloc(T) -> T*?" in av, av)
check("builtin hover 'alloc' appends the concrete result type 'point*?'",
      "point*?" in av and "Result type" in av, av)
# void builtins (free) omit the result-type line.
fv = bval(21)
check("builtin hover 'free' (void) omits the result-type line",
      "Result type" not in fv, fv)
# A plain local that happens to sit next to builtins still hovers as `name: type`,
# never picking up builtin prose.
bm2 = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": biuri, "languageId": "fc",
         "version": 1, "text": BUILTINS}}),
    req(40, "textDocument/hover",
        {"textDocument": {"uri": biuri}, "position": {"line": 6, "character": 8}}),  # 'q'
    req(9, "shutdown", None),
    note("exit", None),
]
bresp2, _, _, _, _ = run_session(bm2)
qv = (bresp2.get(40, {}).get("result") or {}).get("contents", {}).get("value", "")
check("plain local 'q' hovers as 'q: i32?' (no builtin doc bleed-through)",
      "q: i32?" in qv and "option" not in qv, qv)

# --- edit coalescing: a burst of changes with no intervening request collapses
# into ONE analysis. didOpen/didChange only mark the doc dirty; while more input is
# already queued the server keeps draining and analyzes only the latest revision
# (before the next request, or at the next idle point). So the intermediate states
# never publish diagnostics — the editor doesn't flash a squiggle for text the user
# already typed past. Here open(CLEAN) + change(TYPEERR) + change(BROKEN) arrive
# back-to-back; only BROKEN's `unterminated` diagnostic should ever be published.
co_uri = "file:///tmp/fc_lsp_coalesce.fc"
co = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": co_uri, "languageId": "fc",
         "version": 1, "text": CLEAN}}),
    note("textDocument/didChange", {"textDocument": {"uri": co_uri, "version": 2},
         "contentChanges": [{"text": TYPEERR}]}),
    note("textDocument/didChange", {"textDocument": {"uri": co_uri, "version": 3},
         "contentChanges": [{"text": BROKEN}]}),
    req(2, "textDocument/hover", {"textDocument": {"uri": co_uri},       # forces the single flush
        "position": {"line": 0, "character": 4}}),
    req(9, "shutdown", None),
    note("exit", None),
]
_, co_diags, _, _, _ = run_session(co)
# Exactly one publishDiagnostics for three buffered revisions (CLEAN/TYPEERR/BROKEN).
check("coalescing: a burst of 3 edits yields a single diagnostics publish",
      len(co_diags) == 1, f"{len(co_diags)} publishes: {co_diags}")
# The one published state is the latest (BROKEN -> unterminated), never an
# intermediate (no clean [] and no type-error squiggle was ever emitted).
flat = [m for d in co_diags for m in (x["message"] for x in d)]
check("coalescing: only the final revision's diagnostic is published (unterminated)",
      any("unterminated" in m for m in flat)
      and not any("numeric" in m or "bool" in m for m in flat)
      and [] not in co_diags, str(co_diags))

# --- interactive idle-flush: the pure timing-driven path the batch tests cannot
# reach. The batch harness pipes every frame at once, so input_pending() stays
# true through a burst and the flush is always triggered by a trailing REQUEST
# (flush_dirty in dispatch). Here we drive the server like a real editor: send a
# notification, then STOP — so the only thing that can publish diagnostics is the
# idle flush once the input queue drains (lsp_main's `if (!input_pending())
# flush_dirty`). A paused edit must publish on its own, and two edits separated by
# a pause must publish SEPARATELY (the inverse of the coalescing test above).
# Skipped on Windows, where input_pending() is a no-op (every message flushes
# immediately) and select() on a pipe is unavailable.
import os, select, time

if sys.platform.startswith("win"):
    check("interactive idle-flush (skipped on Windows)", True)
else:
    class FramedReader:
        """Incrementally decodes Content-Length-framed JSON-RPC from a pipe fd,
        buffering across reads. next(timeout) returns the next message or None if
        nothing complete arrives within `timeout` seconds (select-based wait)."""
        def __init__(self, fd):
            self.fd, self.buf = fd, bytearray()
        def _extract(self):
            h = self.buf.find(b"\r\n\r\n")
            if h < 0:
                return None
            clen = None
            for line in self.buf[:h].decode("latin1").split("\r\n"):
                if line.lower().startswith("content-length:"):
                    clen = int(line.split(":", 1)[1])
            if clen is None or len(self.buf) < h + 4 + clen:
                return None
            obj = json.loads(bytes(self.buf[h + 4:h + 4 + clen]))
            del self.buf[:h + 4 + clen]
            return obj
        def next(self, timeout):
            deadline = time.monotonic() + timeout
            while True:
                obj = self._extract()
                if obj is not None:
                    return obj
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                r, _, _ = select.select([self.fd], [], [], remaining)
                if not r:
                    return None
                chunk = os.read(self.fd, 65536)
                if not chunk:                       # server closed stdout (EOF)
                    return None
                self.buf += chunk

    def publishes_after(rd, first_timeout=5.0, settle=0.4):
        """Collect publishDiagnostics following a paused edit: wait up to
        first_timeout for the first frame, then keep reading until `settle`
        seconds pass with nothing new — so a stray SECOND publish (which would
        mean the edit wasn't coalesced/flushed exactly once) is also caught."""
        pubs, got = [], False
        while True:
            msg = rd.next(first_timeout if not got else settle)
            if msg is None:
                break
            got = True
            if msg.get("method") == "textDocument/publishDiagnostics":
                pubs.append(msg["params"]["diagnostics"])
        return pubs

    idir = tempfile.mkdtemp(prefix="fc_lsp_interactive_")
    iuri = "file://" + os.path.join(idir, "doc.fc")
    proc = subprocess.Popen([BIN, "--lsp"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

    def send(*ms):
        proc.stdin.write(b"".join(frame(m) for m in ms))
        proc.stdin.flush()

    rd = FramedReader(proc.stdout.fileno())
    p1 = p2 = p3 = None
    ierr = b""
    try:
        # Handshake; sync on the initialize response so subsequent reads see only
        # the publishes provoked by our paused edits.
        send(req(1, "initialize", {"capabilities": {}}), note("initialized", {}))
        init = rd.next(5.0)
        check("interactive: server completes the initialize handshake",
              isinstance(init, dict) and init.get("id") == 1, str(init))

        # 1) didOpen alone, then STOP. No request follows, so only the idle flush
        # can produce this publish — the path no batch test exercises.
        send(note("textDocument/didOpen", {"textDocument": {"uri": iuri,
            "languageId": "fc", "version": 1, "text": CLEAN}}))
        p1 = publishes_after(rd)

        # 2) one paused edit -> one publish, on its own.
        send(note("textDocument/didChange", {"textDocument": {"uri": iuri, "version": 2},
            "contentChanges": [{"text": TYPEERR}]}))
        p2 = publishes_after(rd)

        # 3) a second paused edit publishes SEPARATELY (not coalesced with #2),
        # because the pause between them let the queue drain and flush.
        send(note("textDocument/didChange", {"textDocument": {"uri": iuri, "version": 3},
            "contentChanges": [{"text": BROKEN}]}))
        p3 = publishes_after(rd)

        send(req(9, "shutdown", None), note("exit", None))
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        ierr = proc.stderr.read() or b""

    check("interactive: a paused didOpen is published by the idle flush (no request forced it)",
          p1 is not None and len(p1) == 1 and p1[0] == [], str(p1))
    check("interactive: a paused edit publishes on its own (type error)",
          p2 is not None and len(p2) == 1
          and any("numeric" in d["message"] or "bool" in d["message"] for d in p2[0]), str(p2))
    check("interactive: a second paused edit publishes separately, not coalesced (unterminated)",
          p3 is not None and len(p3) == 1
          and any("unterminated" in d["message"] for d in p3[0]), str(p3))
    check("interactive: clean process exit", proc.returncode == 0, f"rc={proc.returncode}")
    if ierr.strip():
        sys.stderr.write("interactive server stderr:\n" + ierr.decode(errors="replace") + "\n")

# --- lex cache: repeated edits keep the cached stdlib feed resolving correctly.
# Each didChange re-analyzes; the stdlib feed is served from the session lex cache
# (re-parsed from cached tokens, not re-lexed). A clean revision using std::math
# stays clean only if the cached stdlib still resolves `math.sqrt`, so [] across
# many consecutive cache hits — with an error revision in the middle and a return
# to clean — proves the cache doesn't corrupt resolution as edits accumulate.
lcd = tempfile.mkdtemp(prefix="fc_lsp_lexcache_")
lcuri = "file://" + os.path.join(lcd, "doc.fc")
def lc_doc(n, bad=False):
    return ("import math from std::\n\n"
            "let main = (args: str[]) ->\n"
            f"    let r = math.sqrt({n}.0)\n"
            + ("    let oops = 1 + true\n" if bad else "")
            + "    return 0\n")
def lc_change(v, n, bad=False): return note("textDocument/didChange",
    {"textDocument": {"uri": lcuri, "version": v}, "contentChanges": [{"text": lc_doc(n, bad)}]})
def lc_hover(i, l, c): return req(i, "textDocument/hover",
    {"textDocument": {"uri": lcuri}, "position": {"line": l, "character": c}})
# A request after each change forces that revision to flush before the next edit
# overwrites it (deferred/coalesced analysis), so we observe one publish per state.
lc = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": lcuri, "languageId": "fc",
         "version": 1, "text": lc_doc(4)}}),
    lc_hover(20, 3, 8), lc_change(2, 9),              # clean -> clean
    lc_hover(21, 3, 8), lc_change(3, 16, bad=True),   # clean -> error
    lc_hover(22, 3, 8), lc_change(4, 25),             # error -> clean
    lc_hover(23, 3, 8),
    req(9, "shutdown", None), note("exit", None),
]
lcresp, _, lcbf, _, _ = run_session(lc)
lc_states = lcbf.get("doc.fc", [])
# Four observed revisions: clean, clean, error, clean.
check("lex cache: 4 revisions each publish diagnostics", len(lc_states) == 4, str(lc_states))
if len(lc_states) == 4:
    check("lex cache: clean revisions stay clean across cache hits (rev 1,2,4 == [])",
          lc_states[0] == [] and lc_states[1] == [] and lc_states[3] == [],
          str(lc_states))
    check("lex cache: the bad revision still errors (cache hit doesn't mask it)",
          any("numeric" in m or "bool" in m for m in lc_states[2]), str(lc_states[2]))
# Hover on `r` resolves through the cached stdlib `math.sqrt` (-> f64) every time.
lc_hov = (lcresp.get(23, {}).get("result") or {}).get("contents", {})
lc_hov = lc_hov.get("value", "") if isinstance(lc_hov, dict) else str(lc_hov)
check("lex cache: hover still resolves a cached-stdlib-typed value (f64) after edits",
      "f64" in lc_hov, repr(lc_hov))

# --- lex cache: an edited SIBLING feed file is re-lexed (cache slot replaced).
# Open A (imports from B's module) and B. Analyzing A caches B's tokens; editing B
# changes its content; analyzing A again must re-lex B (slot replace + free old)
# rather than reuse stale tokens. Exercises the replacement/free path (ASan-relevant)
# and confirms the importer keeps resolving against B's current definition.
sib = tempfile.mkdtemp(prefix="fc_lsp_lexcache_sib_")
ap = os.path.join(sib, "a.fc"); bp = os.path.join(sib, "b.fc")
auri = "file://" + ap; buri = "file://" + bp
def a_src(n):  return ("import triple from bee\n\nlet main = (args: str[]) ->\n"
                       f"    return triple({n})\n")
def b_src(k):  return f"module bee =\n    let triple = (n: i32) ->\n        n * {k}\n"
with open(ap, "w") as f: f.write(a_src(2))
with open(bp, "w") as f: f.write(b_src(3))
def od(uri, text): return note("textDocument/didOpen",
    {"textDocument": {"uri": uri, "languageId": "fc", "version": 1, "text": text}})
def ch(uri, v, text): return note("textDocument/didChange",
    {"textDocument": {"uri": uri, "version": v}, "contentChanges": [{"text": text}]})
def hv(i, uri): return req(i, "textDocument/hover",
    {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}})
sibm = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    od(auri, a_src(2)), od(buri, b_src(3)),
    ch(auri, 2, a_src(3)), hv(30, auri),          # analyze A: caches B (v1, *3)
    ch(buri, 2, b_src(4)), hv(31, buri),          # edit B -> *4
    ch(auri, 3, a_src(5)), hv(32, auri),          # analyze A: B feed changed -> slot replace
    req(9, "shutdown", None), note("exit", None),
]
_, _, sibbf, _, _ = run_session(sibm)
a_last = sibbf.get("a.fc", [["?"]])[-1]
check("lex cache: edited sibling feed re-lexed; importer stays clean (slot replace)",
      a_last == [], str(a_last))

# --- completion: module-scoped types never leak their mangled C twin -----------
# pass1 registers each module struct/union a SECOND time under its mangled name
# (e.g. `vgagraph__huffnode`) so type stubs resolve, and that twin lands in BOTH
# the module member table AND the global symtab. Completion must hide these: at
# top level it must not offer `vgagraph__huffnode`, and after `vgagraph.` it must
# list `huffnode` exactly once (not the mangled twin, not a duplicate). It must
# also not offer the same global twice (symtab entry + a harvested reference).
COMP = (
    "module vgagraph =\n"
    "    struct huffnode =\n"
    "        bit0: i32\n"
    "        bit1: i32\n"
    "    let helper = (n: i32) ->\n"
    "        n\n"
    "\n"
    "let main = (args: str[]) ->\n"
    "    let v = vgagraph.helper(3)\n"   # line 8: a reference to `vgagraph`
    "    return 0\n"                      # line 9: top-level completion anchor
)
cm = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, COMP),
    req(2, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 9, "character": 4}}),
    req(3, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 8, "character": 22}}),
    req(9, "shutdown", None), note("exit", None),
]
cresp, _, _, _, _ = run_session(cm)
def comp_labels(rid):
    res = cresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return [it.get("label") for it in (its or [])]
top_labels = comp_labels(2)
mem_labels = comp_labels(3)
check("completion: top level offers the module name, never its mangled type twin",
      "vgagraph" in top_labels and "vgagraph__huffnode" not in top_labels, str(top_labels))
check("completion: a referenced global appears once, not duplicated by harvesting",
      top_labels.count("vgagraph") == 1, str([l for l in top_labels if l == "vgagraph"]))
check("completion: after 'vgagraph.' the member type is 'huffnode' once, no mangled twin",
      mem_labels.count("huffnode") == 1 and "helper" in mem_labels
      and "vgagraph__huffnode" not in mem_labels, str(mem_labels))

# --- completion: a field's TYPE DETAIL never shows the mangled C name ----------
# A struct field whose type is a file-scope struct (`weapon` -> `fc__weapon`) or a
# module-scoped struct (`inv.slot` -> `inv__slot`) is stored as a stub that pass2
# canonicalizes to the mangled C name for codegen. Completion renders each field's
# type as the item `detail`; that render must show the SOURCE spelling, never the
# `fc__`/`__` twin — even through pointer/slice/option decorations.
FDET = (
    "struct weapon =\n"                 # 0
    "    damage: i32\n"                  # 1
    "module inv =\n"                     # 2
    "    struct slot =\n"                # 3
    "        count: i32\n"               # 4
    "struct game =\n"                    # 5
    "    best: weapon\n"                 # 6
    "    ptr: weapon*\n"                 # 7
    "    many: weapon[]\n"               # 8
    "    maybe: weapon?\n"               # 9
    "    slot: inv.slot\n"               # 10
    "let use_it = (g: game*) ->\n"       # 11
    "    let x = g.best\n"               # 12  cursor after 'g.'
    "    return\n"                       # 13
    "let main = (args: str[]) ->\n"      # 14
    "    return 0\n"                     # 15
)
fd = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, FDET),
    req(2, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 12, "character": 14}}),
    req(9, "shutdown", None), note("exit", None),
]
fdresp, _, _, _, _ = run_session(fd)
fres = fdresp.get(2, {}).get("result") or {}
fits = fres.get("items") if isinstance(fres, dict) else fres
fdetail = {it.get("label"): it.get("detail") for it in (fits or [])}
check("completion: field type detail demangles a file-scope struct",
      fdetail.get("best") == "weapon", str(fdetail))
check("completion: field type detail demangles through pointer/slice/option",
      fdetail.get("ptr") == "weapon*" and fdetail.get("many") == "weapon[]"
      and fdetail.get("maybe") == "weapon?", str(fdetail))
check("completion: field type detail shows a module-scoped struct qualified, not mangled",
      fdetail.get("slot") == "inv.slot", str(fdetail))
check("completion: no field detail leaks a mangled '__' twin",
      all("__" not in (v or "") for v in fdetail.values()), str(fdetail))

# --- completion: lexical scope at the cursor (module siblings + function locals)
# Inside a module function, its sibling members are in scope as bare names, and
# the enclosing function's params/lets are in scope — completion must offer both.
# A module-internal local must NOT leak to a sibling top-level function's scope.
SCOPE = (
    "module vgagraph =\n"                                 # 0
    "    struct huffnode =\n"                             # 1
    "        bit0: i32\n"                                 # 2
    "    let huff_expand = (n: i32) ->\n"                 # 3
    "        n\n"                                         # 4
    "    let decompress_at = (raw: i32, dict: i32) ->\n"  # 5
    "        let expanded_len = raw + 1\n"                # 6
    "        \n"                                          # 7  cursor: inside decompress_at
    "    let decompress_chunk = (chunk: i32) ->\n"        # 8
    "        chunk\n"                                     # 9
    "\n"                                                  # 10
    "let main = (args: str[]) ->\n"                       # 11
    "    let top_local = 5\n"                             # 12
    "    \n"                                              # 13  cursor: inside top-level main
    "    return 0\n"                                      # 14
)
sc = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, SCOPE),
    req(2, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 7, "character": 8}}),
    req(3, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 13, "character": 4}}),
    req(9, "shutdown", None), note("exit", None),
]
scresp, _, _, _, _ = run_session(sc)
def sc_labels(rid):
    res = scresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return [it.get("label") for it in (its or [])]
inmod = sc_labels(2)
intop = sc_labels(3)
check("completion: a module's sibling members are in scope inside its functions",
      all(x in inmod for x in ("huff_expand", "decompress_chunk", "huffnode")),
      str([l for l in inmod if "decompress" in (l or "") or "huff" in (l or "")]))
check("completion: the enclosing function's params and locals are in scope",
      all(x in inmod for x in ("raw", "dict", "expanded_len")), str(inmod))
check("completion: an in-scope sibling member is not duplicated",
      inmod.count("huff_expand") == 1, str([l for l in inmod if l == "huff_expand"]))
check("completion: a top-level function sees its own locals/params",
      "top_local" in intop and "args" in intop and "vgagraph" in intop, str(intop))
check("completion: a module-internal local does not leak to a top-level scope",
      "expanded_len" not in intop, str([l for l in intop if "expand" in (l or "")]))

# --- completion: scope walk survives the multi-file merge's namespace sentinels.
# analyze() injects a line-0 / NULL-filename DECL_NAMESPACE "reset sentinel"
# before every merged file that doesn't open with a namespace (src/analyze.c). In
# a multi-file unit, a sentinel for a file ordered AFTER the open one lands past
# the open file's module in the decl list; the scope walk must ignore foreign and
# synthetic decls (match filename + real line) or that sentinel wins the line race
# and no in-scope names are offered. `zz.fc` sorts after `data.fc` under src/*.fc,
# so its sentinel reproduces exactly the wolf-fc regression.
rsp_proj = tempfile.mkdtemp(prefix="fc_lsp_scope_rsp_")
rwrite(os.path.join(rsp_proj, "src", "data.fc"),
       "module vgagraph =\n"
       "    let huff_expand = (n: i32) ->\n"
       "        n\n"
       "    let decompress_at = (raw: i32, dict: i32) ->\n"
       "        let expanded_len = raw + 1\n"
       "        \n"                                     # line 5: cursor, inside decompress_at
       "    let decompress_chunk = (chunk: i32) ->\n"
       "        chunk\n")
rwrite(os.path.join(rsp_proj, "src", "zz.fc"),
       "module other =\n    let g = () ->\n        0\n")  # merged AFTER data.fc -> a sentinel follows vgagraph
rwrite(os.path.join(rsp_proj, "lsp.rsp"), "# unit\nsrc/*.fc\n")
dp = os.path.join(rsp_proj, "src", "data.fc")
rsp_sess = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": "file://" + dp,
         "languageId": "fc", "version": 1, "text": open(dp).read()}}),
    req(2, "textDocument/completion", {"textDocument": {"uri": "file://" + dp},
         "position": {"line": 5, "character": 8}}),
    req(9, "shutdown", None), note("exit", None),
]
rresp, _, _, _, _ = run_session(rsp_sess)
rres = rresp.get(2, {}).get("result") or {}
rits = rres.get("items") if isinstance(rres, dict) else rres
rlabels = [it.get("label") for it in (rits or [])]
check("completion: in-scope names survive merged-unit namespace sentinels (lsp.rsp)",
      all(x in rlabels for x in ("huff_expand", "decompress_chunk", "expanded_len", "raw", "dict")),
      str([l for l in rlabels if l and ("huff" in l or "decompress" in l or "expand" in l or l in ("raw","dict"))]))

# --- completion: synthetic / built-in members on non-struct objects -----------
# `.` after a slice -> len/ptr, after an option -> is_some/is_none, after a
# numeric type name -> min/max/bits (+ float nan/inf/neg_inf/epsilon); `.` on a
# pointer auto-derefs to its struct's fields. Each context returns EXACTLY its
# synthetic set (member completion replaces, not augments, the global list), with
# the right result type as `detail`.
SYN = (
    "module m =\n"                                 # 0
    "    struct pt =\n"                            # 1
    "        x: i32\n"                             # 2
    "        y: i32\n"                             # 3
    "    let f = (s: u8[], p: pt*, o: i32?) ->\n"  # 4
    "        let a = s.len\n"                      # 5  slice '.'
    "        let b = p.x\n"                        # 6  pointer '.' (auto-deref)
    "        let c = o.is_some\n"                  # 7  option '.'
    "        let d = i32.max\n"                    # 8  int type name '.'
    "        let e = f64.nan\n"                    # 9  float type name '.'
    "        0\n")                                 # 10
def synreq(i, ln, ch):
    return req(i, "textDocument/completion",
               {"textDocument": {"uri": URI}, "position": {"line": ln, "character": ch}})
syn = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, SYN),
    synreq(2, 5, 19),   # s.l|en
    synreq(3, 6, 19),   # p.x|
    synreq(4, 7, 21),   # o.is|_some
    synreq(5, 8, 20),   # i32.m|ax
    synreq(6, 9, 20),   # f64.n|an
    req(9, "shutdown", None), note("exit", None),
]
synresp, _, _, _, _ = run_session(syn)
def syn_items(rid):
    res = synresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return {it.get("label"): it.get("detail") for it in (its or [])}
sl, ar, op, it_, fl = (syn_items(i) for i in (2, 3, 4, 5, 6))
check("completion: slice '.' offers exactly len/ptr with len: i64",
      set(sl) == {"len", "ptr"} and sl.get("len") == "i64", str(sl))
check("completion: slice '.ptr' detail is a pointer to the element type",
      str(sl.get("ptr", "")).endswith("*"), str(sl))
check("completion: pointer '.' auto-derefs and offers the pointee struct's fields",
      set(ar) == {"x", "y"} and ar.get("x") == "i32", str(ar))
check("completion: option '.' offers exactly is_some/is_none as bool",
      set(op) == {"is_some", "is_none"} and op.get("is_some") == "bool", str(op))
check("completion: integer type name '.' offers min/max/bits typed correctly",
      set(it_) == {"min", "max", "bits"} and it_.get("max") == "i32"
      and it_.get("bits") == "i32", str(it_))
check("completion: float type name '.' adds nan/inf/neg_inf/epsilon typed f64",
      set(fl) == {"min", "max", "bits", "epsilon", "nan", "inf", "neg_inf"}
      and fl.get("nan") == "f64", str(fl))

# --- completion: a member operator never falls through to the global list -----
# `->` is overloaded (lambda body, fn-type, match arm — the deref role was
# retired in favor of `.`). At a NON-member `->` — e.g. a lambda's `(params) ->`
# — completion must be empty, NOT the global dump, regardless of triggerKind:
# once VSCode has a suggest session open it re-queries as Invoked (kind=1) even
# as you type through `->`, so suppressing only TriggerCharacter (kind=2) would
# still leak globals (the reported bug). A genuine `ptr.` still completes the
# pointee's fields (auto-deref) under either kind.
TRIG = (
    "module m =\n"                       # 0
    "    struct pt =\n"                  # 1
    "        x: i32\n"                   # 2
    "    let f = (p: pt*) ->\n"          # 3  ') ->' is a lambda body, not a deref
    "        let b = p.x\n"              # 4  real pointer dereference via '.'
    "        0\n")                       # 5
def trigreq(i, ln, ch, kind, tc=">"):
    p = {"textDocument": {"uri": URI}, "position": {"line": ln, "character": ch},
         "context": {"triggerKind": kind, "triggerCharacter": tc}}
    return req(i, "textDocument/completion", p)
lam_col = TRIG.split("\n")[3].index("->") + 2      # just past the lambda '->'
der_col = TRIG.split("\n")[4].index("p.") + 2      # just past 'p.'
trg = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, TRIG),
    trigreq(2, 3, lam_col, 2),        # lambda '->', TriggerCharacter -> empty
    trigreq(3, 3, lam_col, 1),        # lambda '->', Invoked (persisted session) -> empty
    trigreq(4, 4, der_col, 2, "."),   # real deref '.', TriggerCharacter -> pt fields
    req(9, "shutdown", None), note("exit", None),
]
trgresp, _, _, _, _ = run_session(trg)
def trg_labels(rid):
    res = trgresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return [it.get("label") for it in (its or [])]
check("completion: trigger-char '->' at a lambda body is suppressed (empty)",
      trg_labels(2) == [], str(trg_labels(2)))
check("completion: Invoked '->' at a lambda body also stays empty (no global leak)",
      trg_labels(3) == [], str(trg_labels(3)[:8]))
check("completion: a genuine pointer '.' completes pointee fields under a trigger char",
      set(trg_labels(4)) == {"x"}, str(trg_labels(4)))

# --- completion: member access on a composite object (index / call / nested) --
# The object before a '.' need not be a bare identifier. `dict[i].` ends in
# ']', `mk().` in ')', which no leaf node's position span covers — so the server
# captures the field node by its operator position and reads field.object.type.
# Index→struct, call→struct, and index→pointer '.' must all complete the fields.
CMP = (
    "module m =\n"                                       # 0
    "    struct pt =\n"                                  # 1
    "        bit0: i32\n"                                # 2
    "        bit1: i32\n"                                # 3
    "    let mk = (n: i32) ->\n"                         # 4
    "        pt { bit0 = n, bit1 = n }\n"                # 5
    "    let f = (arr: pt[], pp: pt*[]) ->\n"            # 6
    "        let a = arr[0].bit0\n"                      # 7  index  -> struct '.'
    "        let b = mk(3).bit1\n"                       # 8  call   -> struct '.'
    "        let c = pp[0].bit0\n"                       # 9  index  -> pointer '.' (auto-deref)
    "        0\n")                                       # 10
def cmpreq(i, ln, after):
    col = CMP.split("\n")[ln].index(after) + len(after) + 1   # one char into the member
    return req(i, "textDocument/completion",
               {"textDocument": {"uri": URI}, "position": {"line": ln, "character": col}})
cmps = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, CMP),
    cmpreq(2, 7, "arr[0]."),
    cmpreq(3, 8, "mk(3)."),
    cmpreq(4, 9, "pp[0]."),
    req(9, "shutdown", None), note("exit", None),
]
cmpresp, _, _, _, _ = run_session(cmps)
def cmp_labels(rid):
    res = cmpresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return set(it.get("label") for it in (its or []))
check("completion: a slice element 'arr[i].' completes the element struct's fields",
      cmp_labels(2) == {"bit0", "bit1"}, str(cmp_labels(2)))
check("completion: a call result 'f().' completes the returned struct's fields",
      cmp_labels(3) == {"bit0", "bit1"}, str(cmp_labels(3)))
check("completion: a slice-of-pointers element 'arr[i].' auto-derefs to pointee fields",
      cmp_labels(4) == {"bit0", "bit1"}, str(cmp_labels(4)))

# --- result type T!: hover docs for ok/err, member completion is_ok/is_err ----
RES = (
    "let parse = (n: i32) ->\n"            # 0
    "    if n < 0 then err(i32, 2)\n"      # 1  hover 'err'
    "    else ok(n * 2)\n"                 # 2  hover 'ok'
    "\n"                                   # 3
    "let main = (args: str[]) ->\n"        # 4
    "    let r = parse(21)\n"              # 5  hover 'r' -> i32!
    "    let f = r.is_ok\n"                # 6  member completion after 'r.'
    "    if f then 0 else 1\n"             # 7
)
rs = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, RES),
    hover(2, 1, RES.split("\n")[1].index("err") + 1),
    hover(3, 2, RES.split("\n")[2].index("ok(") + 1),
    hover(4, 5, 8),
    req(5, "textDocument/completion",
        {"textDocument": {"uri": URI},
         "position": {"line": 6, "character": RES.split("\n")[6].index("r.") + 2}}),
    req(9, "shutdown", None), note("exit", None),
]
rsresp, _, _, _, _ = run_session(rs)
def rs_hover(rid):
    res = rsresp.get(rid, {}).get("result") or {}
    return ((res.get("contents") or {}).get("value")) or ""
def rs_labels(rid):
    res = rsresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return set(it.get("label") for it in (its or []))
check("hover 'err' shows the err(T, code) builtin doc",
      "err(T, code: i32) -> T!" in rs_hover(2), rs_hover(2))
check("hover 'ok' shows the ok(x) builtin doc",
      "ok(x: T) -> T!" in rs_hover(3), rs_hover(3))
check("hover a result binding shows the T! type",
      "i32!" in rs_hover(4), rs_hover(4))
check("completion: 'r.' on a result offers is_ok/is_err",
      rs_labels(5) == {"is_ok", "is_err"}, str(rs_labels(5)))

# --- error groups: member hover shows `error`, member completion, error_name doc ----
ERR = (
    "error file_io =\n"                          # 0
    "    | not_found\n"                          # 1
    "    | invalid_path\n"                       # 2
    "\n"                                         # 3
    "let main = (args: str[]) ->\n"              # 4
    "    let c = file_io.not_found\n"            # 5  hover member; completion after 'file_io.'
    "    let n = error_name(c)\n"                # 6  hover 'error_name'
    "    if n.is_some then 0 else 1\n"           # 7
)
es = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, ERR),
    hover(2, 5, ERR.split("\n")[5].index("not_found") + 1),
    req(3, "textDocument/completion",
        {"textDocument": {"uri": URI},
         "position": {"line": 5, "character": ERR.split("\n")[5].index("file_io.") + 8}}),
    hover(4, 6, ERR.split("\n")[6].index("error_name") + 1),
    hover(5, 5, ERR.split("\n")[5].index("c =")),
    req(9, "shutdown", None), note("exit", None),
]
esresp, _, _, _, _ = run_session(es)
def es_hover(rid):
    res = esresp.get(rid, {}).get("result") or {}
    return ((res.get("contents") or {}).get("value")) or ""
def es_labels(rid):
    res = esresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return set(it.get("label") for it in (its or []))
check("hover an error-group member shows the `error` display alias",
      "error" in es_hover(2), es_hover(2))
check("completion: 'file_io.' offers the group's members",
      es_labels(3) == {"not_found", "invalid_path"}, str(es_labels(3)))
check("hover 'error_name' shows the builtin doc",
      "error_name(e: i32) -> str?" in es_hover(4), es_hover(4))
check("hover a binding initialized from an error constant shows type error",
      "error" in es_hover(5), es_hover(5))

# ---- enums: hover, member completion (variants + count), enum_of builtin ----
EN = (
    "enum color of u8 =\n"                       # 0
    "    | red        // stop signal\n"          # 1
    "    | green\n"                              # 2
    "    | blue\n"                               # 3
    "\n"                                         # 4
    "let main = (args: str[]) ->\n"              # 5
    "    let c = color.red\n"                    # 6  hover variant; completion after 'color.'
    "    let n = enum_of(color, 1)\n"            # 7  hover 'enum_of'
    "    match n with\n"                         # 8
    "    | some(x) -> (i32) x\n"                 # 9
    "    | none -> 0\n"                          # 10
)
en = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, EN),
    hover(2, 6, EN.split("\n")[6].index("red") + 1),
    req(3, "textDocument/completion",
        {"textDocument": {"uri": URI},
         "position": {"line": 6, "character": EN.split("\n")[6].index("color.") + 6}}),
    hover(4, 7, EN.split("\n")[7].index("enum_of") + 1),
    hover(5, 6, EN.split("\n")[6].index("c =")),
    req(9, "shutdown", None), note("exit", None),
]
enresp, _, _, _, _ = run_session(en)
def en_hover(rid):
    res = enresp.get(rid, {}).get("result") or {}
    return ((res.get("contents") or {}).get("value")) or ""
def en_labels(rid):
    res = enresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return set(it.get("label") for it in (its or []))
check("hover an enum variant shows the enum type and the variant's doc comment",
      "color" in en_hover(2) and "stop signal" in en_hover(2), en_hover(2))
check("completion: 'color.' offers variants plus count",
      en_labels(3) == {"red", "green", "blue", "count"}, str(en_labels(3)))
check("hover 'enum_of' shows the builtin doc",
      "enum_of(E, x) -> E?" in en_hover(4), en_hover(4))
check("hover a binding initialized from an enum variant shows the enum type",
      "color" in en_hover(5), en_hover(5))

# ---- declaration-form hovers + companion doc merge ----
CP = (
    "// Eight compass directions.\n"             # 0
    "enum dir of u8 =\n"                         # 1  decl-site enum name
    "    | east\n"                               # 2
    "    | nodir\n"                              # 3
    "\n"                                         # 4
    "// Companion tables for dir.\n"             # 5
    "module dir =\n"                             # 6  decl-site module name
    "    let dx = i32[2] { 1, 0 }\n"             # 7
    "\n"                                         # 8
    "// Sound helpers.\n"                        # 9
    "module sfx =\n"                             # 10
    "    let volume = 3\n"                       # 11
    "\n"                                         # 12
    "struct box =\n"                             # 13
    "    d: dir\n"                               # 14  field type annotation
    "\n"                                         # 15
    "let use = (k: dir) -> (i32) k\n"            # 16  param type annotation
    "\n"                                         # 17
    "let main = (args: str[]) ->\n"              # 18
    "    let d = dir.east\n"                     # 19  ref 'dir' -> merged hover
    "    let v = sfx.volume\n"                   # 20  ref 'sfx' -> module header
    "    (i32) d + v + use(dir.nodir)\n"         # 21
)
cp = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, CP),
    hover(2, 19, CP.split("\n")[19].index("dir") + 1),   # ref with companion
    hover(3, 1, CP.split("\n")[1].index("dir") + 1),     # enum decl site
    hover(4, 6, CP.split("\n")[6].index("dir") + 1),     # module decl site
    hover(5, 20, CP.split("\n")[20].index("sfx") + 1),   # pure module ref
    hover(6, 14, CP.split("\n")[14].index("dir") + 1),   # struct field annotation
    hover(7, 16, CP.split("\n")[16].index("dir") + 1),   # param annotation
    req(8, "textDocument/definition", {"textDocument": {"uri": URI},
        "position": {"line": 16, "character": CP.split("\n")[16].index("dir") + 1}}),
    req(9, "shutdown", None), note("exit", None),
]
cpresp, _, _, _, _ = run_session(cp)
def cp_hover(rid):
    res = cpresp.get(rid, {}).get("result") or {}
    return ((res.get("contents") or {}).get("value")) or ""
check("hover a companion-pair name merges both docs with labeled sections",
      "enum dir of u8" in cp_hover(2) and "module dir" in cp_hover(2)
      and "Companion tables for dir." in cp_hover(2)
      and "Eight compass directions." in cp_hover(2), cp_hover(2))
check("merged companion hover separates the sections with a visible rule",
      "\n────" in cp_hover(2), cp_hover(2))
check("hover the enum name at its declaration site shows the decl form + doc",
      "enum dir of u8" in cp_hover(3) and "Eight compass directions." in cp_hover(3),
      cp_hover(3))
check("declaration-site hovers do not merge the companion (references only)",
      "module dir" not in cp_hover(3) and "enum dir" not in cp_hover(4),
      cp_hover(3) + " ||| " + cp_hover(4))
check("hover the module name at its declaration site shows the decl form + doc",
      "module dir" in cp_hover(4) and "Companion tables for dir." in cp_hover(4),
      cp_hover(4))
check("hover a plain module reference shows `module m`, not `m: void`",
      "module sfx" in cp_hover(5) and "Sound helpers." in cp_hover(5)
      and "void" not in cp_hover(5), cp_hover(5))
check("hover a struct-field type annotation shows the type's merged decl hover",
      "enum dir of u8" in cp_hover(6) and "module dir" in cp_hover(6), cp_hover(6))
check("hover a param type annotation shows the type's decl-form hover",
      "enum dir of u8" in cp_hover(7)
      and "Eight compass directions." in cp_hover(7), cp_hover(7))
cp_def = (cpresp.get(8, {}).get("result") or {})
check("go-to-definition from a param type annotation lands on the enum decl",
      isinstance(cp_def, dict) and cp_def.get("range", {}).get("start", {}).get("line") == 1,
      str(cp_def))

# ---- companion members after '.' on a TYPE NAME + doc-less companion hover ----
# The std::wideint shape: struct + companion module pairs inside a module,
# pulled in by a wildcard import; the module half has only a blank-separated
# banner (no attached doc comment). Hover on the type name must still render
# the `module X` fence (the pairing is visible even without a doc), and
# completion after `TypeName.` must offer the companion module's members —
# not the struct's fields, which are invalid on a type name.
CM_LIB = (
    "namespace mystd::\n"                        # 0
    "\n"                                         # 1
    "module wide =\n"                            # 2
    "    // A 128-bit integer.\n"                # 3
    "    struct w128 =\n"                        # 4
    "        limbs: u32[4]\n"                    # 5
    "\n"                                         # 6
    "    // ============================\n"      # 7  banner, blank-separated:
    "\n"                                         # 8  must NOT attach as doc
    "    module w128 =\n"                        # 9  companion, no doc comment
    "        let zero = () ->\n"                 # 10
    "            default(w128)\n"                # 11
    "        let parse = (s: str) ->\n"          # 12
    "            default(w128)\n"                # 13
    "\n"                                         # 14
    "    union pkt =\n"                          # 15
    "        | ping(i32)\n"                      # 16
    "        | quiet\n"                          # 17
    "\n"                                         # 18
    "    module pkt =\n"                         # 19  union companion, no doc
    "        let mk = () ->\n"                   # 20
    "            pkt.quiet\n"                    # 21
)
CM_MAIN = (
    "import * from mystd::wide\n"                # 0
    "\n"                                         # 1
    "module app =\n"                             # 2
    "    let go = () ->\n"                       # 3
    "        let z = w128.zero()\n"              # 4  'w128' ref + member completion
    "        let k = pkt.mk()\n"                 # 5  union companion completion
    "        let f = z.limbs\n"                  # 6  VALUE '.': fields (control)
    "        f.len\n"                            # 7
    "\n"                                         # 8
    "let main = (args: str[]) ->\n"              # 9
    "    (i32) app.go()\n"                       # 10
)
cmdir = tempfile.mkdtemp(prefix="fc_lsp_comp_")
with open(os.path.join(cmdir, "wide.fc"), "w") as f: f.write(CM_LIB)
with open(os.path.join(cmdir, "main.fc"), "w") as f: f.write(CM_MAIN)
cmuri = "file://" + os.path.join(cmdir, "main.fc")
def cm_completion(i, l, c): return req(i, "textDocument/completion",
    {"textDocument": {"uri": cmuri}, "position": {"line": l, "character": c}})
cm = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    note("textDocument/didOpen", {"textDocument": {"uri": cmuri, "languageId": "fc",
         "version": 1, "text": CM_MAIN}}),
    req(2, "textDocument/hover", {"textDocument": {"uri": cmuri},
        "position": {"line": 4, "character": CM_MAIN.split("\n")[4].index("w128") + 1}}),
    cm_completion(3, 4, CM_MAIN.split("\n")[4].index("w128.") + 5),   # after 'w128.'
    cm_completion(4, 5, CM_MAIN.split("\n")[5].index("pkt.") + 4),    # after 'pkt.'
    cm_completion(5, 6, CM_MAIN.split("\n")[6].index("z.") + 2),      # after 'z.' (value)
    req(9, "shutdown", None), note("exit", None),
]
cmresp, _, cmbf, _, _ = run_session(cm)
cm_hov = ((cmresp.get(2, {}).get("result") or {}).get("contents") or {}).get("value") or ""
def cm_labels(rid):
    r = cmresp.get(rid, {}).get("result") or {}
    items = r.get("items") if isinstance(r, dict) else r
    return [it.get("label") for it in (items or [])]
check("companion setup: unit is clean",
      cmbf.get("main.fc", [["?"]])[-1] == [], str(cmbf.get("main.fc")))
check("hover a type ref merges a DOC-LESS companion module (module fence still shown)",
      "struct w128" in cm_hov and "module w128" in cm_hov and "\n────" in cm_hov,
      cm_hov)
check("completion after '.' on a struct TYPE NAME offers the companion module's members",
      set(cm_labels(3)) >= {"zero", "parse"}, str(cm_labels(3)))
check("completion on a struct type name does NOT offer the struct's fields",
      "limbs" not in cm_labels(3), str(cm_labels(3)))
check("completion after '.' on a union type name merges companion members and variants",
      set(cm_labels(4)) >= {"mk", "ping", "quiet"}, str(cm_labels(4)))
check("completion on a struct VALUE still offers its fields",
      "limbs" in cm_labels(5) and "zero" not in cm_labels(5), str(cm_labels(5)))

# --- import statements: hover + go-to-definition on their identifiers ---------
# Every identifier written in an import — the imported name, its `as` alias, and
# the module named in the `from` clause — resolves to the symbol pass1 bound it
# to. A namespace path segment (`acme`) names no declaration and is deliberately
# inert, as is the `import`/`from`/`as` keyword itself.
IMP_LIB = (
    "// Outer bag.\n"                            # 0
    "module outer =\n"                           # 1
    "    // Inner bag.\n"                        # 2
    "    module inner =\n"                       # 3
    "        // Adds one.\n"                     # 4
    "        let bump = (n: i32) ->\n"           # 5
    "            n + 1\n"                        # 6
    "\n"                                         # 7
    "    // Doubles n.\n"                        # 8
    "    let double = (n: i32) ->\n"             # 9
    "        n * 2\n"                            # 10
    "\n"                                         # 11
    "    // Triples n.\n"                        # 12
    "    let triple = (n: i32) ->\n"             # 13
    "        n * 3\n"                            # 14
    "\n"                                         # 15
    "    // Middle bag.\n"                       # 16
    "    module mid =\n"                         # 17
    "        // Deep bag.\n"                     # 18
    "        module deep =\n"                    # 19
    "            // Answers two.\n"              # 20
    "            let two = () ->\n"              # 21
    "                2\n"                        # 22
)
IMP_NSLIB = (
    "namespace acme::\n"                         # 0
    "\n"                                         # 1
    "// A colour.\n"                             # 2
    "struct color =\n"                           # 3
    "    r: u8\n"                                # 4
    "\n"                                         # 5
    "// Colour helpers.\n"                       # 6
    "module color =\n"                           # 7
    "    let black = () ->\n"                    # 8
    "        color { r = 0u8 }\n"                # 9
    "\n"                                         # 10
    "// Palette bag.\n"                          # 11
    "module palette =\n"                         # 12
    "    // The default shade.\n"                # 13
    "    let shade = () ->\n"                    # 14
    "        7\n"                                # 15
)
IMP_MAIN = (
    "import color from acme::\n"                 # 0  cross-ns companion pair
    "import shade from acme::palette\n"          # 1  member of a namespaced module
    "import double as dbl, triple from outer\n"  # 2  alias + multi-item
    "import * from outer\n"                      # 3  wildcard (also brings `inner`)
    "import two as t2 from outer.mid.deep\n"     # 4  dotted `from` route + alias
    "\n"                                         # 5
    "module app =\n"                             # 6
    "    import inner from outer\n"              # 7  module-body module import
    "    import bump from inner\n"               # 8  ...resolved through it
    "\n"                                         # 9
    "    let go = () ->\n"                       # 10
    "        bump(1)\n"                          # 11
    "\n"                                         # 12
    "let main = (args: str[]) ->\n"              # 13
    "    let c = color { r = 3u8 }\n"            # 14
    "    let a = dbl(1) + triple(2) + shade()\n" # 15
    "    let b = app.go() + inner.bump(0) + t2()\n"  # 16
    "    return (i32) c.r + a + b\n"             # 17
)
# Two imports that produce no symbol: one that never resolves, and one with a dot
# on the LEFT of `from` (rejected in the parser, so it yields no Decl at all).
# Hover/definition must stay silent on both — the diagnostic already says what is
# wrong — and neither may take the server down.
IMP_BAD = (
    "import nosuch from outer\n"
    "import outer.inner\n"
    "\n"
    "let main = (args: str[]) ->\n"
    "    return 0\n"
)
impdir = tempfile.mkdtemp(prefix="fc_lsp_imp_")
with open(os.path.join(impdir, "lib.fc"), "w") as f:   f.write(IMP_LIB)
with open(os.path.join(impdir, "nslib.fc"), "w") as f: f.write(IMP_NSLIB)
with open(os.path.join(impdir, "main.fc"), "w") as f:  f.write(IMP_MAIN)
impuri = "file://" + os.path.join(impdir, "main.fc")
IL = IMP_MAIN.split("\n")

def imp_at(line, token, occurrence=0):
    """Character offset of `token` on IMP_MAIN's `line`, +1 to land inside it."""
    col, s = -1, IL[line]
    for _ in range(occurrence + 1):
        col = s.index(token, col + 1)
    return col + 1

# (id, label, line, token) — each probed with both hover and definition.
IMP_PROBES = [
    (10, "ns type name",      0, "color"),
    (11, "ns member name",    1, "shade"),
    (12, "ns module in path", 1, "palette"),
    (13, "namespace segment", 1, "acme"),
    (14, "source name",       2, "double"),
    (15, "as alias",          2, "dbl"),
    (16, "2nd item of a list",2, "triple"),
    (17, "from module",       2, "outer"),
    (18, "wildcard module",   3, "outer"),
    (21, "module-body name",  8, "bump"),
    (22, "module-body module",8, "inner"),
    (23, "import keyword",    0, "import"),
    (24, "route head",        4, "outer"),
    (25, "route mid segment", 4, "mid"),
    (26, "route tail segment",4, "deep"),
    (27, "name over a route", 4, "two"),
    (28, "alias over a route",4, "t2"),
]
imp = [req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
       note("textDocument/didOpen", {"textDocument": {"uri": impuri, "languageId": "fc",
            "version": 1, "text": IMP_MAIN}})]
for rid, _, line, token in IMP_PROBES:
    pos = {"line": line, "character": imp_at(line, token)}
    imp.append(req(rid, "textDocument/hover", {"textDocument": {"uri": impuri}, "position": pos}))
    imp.append(req(rid + 100, "textDocument/definition",
                   {"textDocument": {"uri": impuri}, "position": pos}))
imp += [
    note("textDocument/didChange", {"textDocument": {"uri": impuri, "version": 2},
         "contentChanges": [{"text": IMP_BAD}]}),
    req(50, "textDocument/hover", {"textDocument": {"uri": impuri},
        "position": {"line": 0, "character": 8}}),      # inside `nosuch`
    req(51, "textDocument/definition", {"textDocument": {"uri": impuri},
        "position": {"line": 0, "character": 8}}),
    req(52, "textDocument/hover", {"textDocument": {"uri": impuri},
        "position": {"line": 1, "character": 15}}),     # inside dotted `inner`
    req(53, "textDocument/definition", {"textDocument": {"uri": impuri},
        "position": {"line": 1, "character": 15}}),
    req(9, "shutdown", None), note("exit", None),
]
impresp, _, impbf, imprc, _ = run_session(imp)

def imp_hov(rid):
    r = impresp.get(rid, {}).get("result")
    return ((r or {}).get("contents") or {}).get("value") or ""
def imp_def(rid):
    """(basename, line0, char0) of a definition response, or None."""
    r = impresp.get(rid + 100, {}).get("result")
    if not isinstance(r, dict): return None
    st = r["range"]["start"]
    return (r["uri"].split("/")[-1], st["line"], st["character"])

# The first publish is the well-formed IMP_MAIN; the last is IMP_BAD, edited in
# deliberately broken at the end of the session.
check("imports: the probe unit is clean", impbf.get("main.fc", [["?"]])[0] == [],
      str(impbf.get("main.fc")))
# A cross-namespace import of a companion pair brings in both halves under one
# name, so its hover merges both docs exactly as a use-site reference does.
check("imports: a cross-namespace type name hovers as the merged companion pair",
      "struct color" in imp_hov(10) and "module color" in imp_hov(10)
      and "A colour." in imp_hov(10) and "Colour helpers." in imp_hov(10), imp_hov(10))
check("imports: go-to-definition on it lands on the struct in the other file",
      imp_def(10) == ("nslib.fc", 3, 0), str(imp_def(10)))
check("imports: an imported function hovers with its type and doc comment",
      "shade: () -> i32" in imp_hov(11) and "The default shade." in imp_hov(11), imp_hov(11))
check("imports: go-to-definition on an imported function lands on its `let`",
      imp_def(11) == ("nslib.fc", 14, 4), str(imp_def(11)))
check("imports: the module in a `from ns::mod` path hovers as a module",
      "module palette" in imp_hov(12) and "Palette bag." in imp_hov(12), imp_hov(12))
check("imports: go-to-definition on that module lands on its declaration",
      imp_def(12) == ("nslib.fc", 12, 0), str(imp_def(12)))
# A namespace names no declaration — there is nothing to jump to, so its
# segments stay inert rather than resolving to something arbitrary.
check("imports: a namespace path segment offers nothing",
      imp_hov(13) == "" and imp_def(13) is None, repr((imp_hov(13), imp_def(13))))
check("imports: the source name of an aliased import hovers as that symbol",
      "double: (i32) -> i32" in imp_hov(14) and "Doubles n." in imp_hov(14), imp_hov(14))
# The alias is only another spelling of the same symbol, so it reports the same
# thing — under the SOURCE name, which is what the reader came to look up.
check("imports: the `as` alias hovers as the symbol it aliases",
      "double: (i32) -> i32" in imp_hov(15) and "Doubles n." in imp_hov(15), imp_hov(15))
check("imports: hovering the alias highlights the alias token, not the source name",
      (impresp.get(15, {}).get("result") or {}).get("range", {})
        == {"start": {"line": 2, "character": IL[2].index("dbl")},
            "end":   {"line": 2, "character": IL[2].index("dbl") + 3}},
      json.dumps((impresp.get(15, {}).get("result") or {}).get("range")))
check("imports: go-to-definition on the alias lands on the aliased definition",
      imp_def(15) == ("lib.fc", 9, 4), str(imp_def(15)))
# Each name in `a, b from m` is its own decl sharing one `from` clause: the
# second item must resolve to ITS symbol, not the first's.
check("imports: the 2nd name of a comma list resolves to its own symbol",
      "triple: (i32) -> i32" in imp_hov(16) and "Triples n." in imp_hov(16), imp_hov(16))
check("imports: go-to-definition on the 2nd name lands on its own `let`",
      imp_def(16) == ("lib.fc", 13, 4), str(imp_def(16)))
check("imports: the `from` module hovers as a module with its doc",
      "module outer" in imp_hov(17) and "Outer bag." in imp_hov(17), imp_hov(17))
check("imports: go-to-definition on the `from` module lands on its declaration",
      imp_def(17) == ("lib.fc", 1, 0), str(imp_def(17)))
check("imports: a wildcard import's module resolves (there is no name to hover)",
      "module outer" in imp_hov(18) and imp_def(18) == ("lib.fc", 1, 0),
      repr((imp_hov(18), imp_def(18))))
check("imports: an import inside a module body resolves its name",
      "bump: (i32) -> i32" in imp_hov(21) and "Adds one." in imp_hov(21)
      and imp_def(21) == ("lib.fc", 5, 8), repr((imp_hov(21), imp_def(21))))
check("imports: an import inside a module body resolves its `from` module",
      "module inner" in imp_hov(22) and imp_def(22) == ("lib.fc", 3, 4),
      repr((imp_hov(22), imp_def(22))))
check("imports: the `import` keyword itself offers nothing",
      imp_hov(23) == "" and imp_def(23) is None, repr((imp_hov(23), imp_def(23))))
# A dotted `from` route answers PER SEGMENT: each names its own module, so the
# reader can hover or jump anywhere along the path rather than only at its head.
check("imports: the head of a `from` route hovers as its module",
      "module outer" in imp_hov(24) and "Outer bag." in imp_hov(24)
      and imp_def(24) == ("lib.fc", 1, 0), repr((imp_hov(24), imp_def(24))))
check("imports: a middle route segment resolves to its own nested module",
      "module mid" in imp_hov(25) and "Middle bag." in imp_hov(25)
      and imp_def(25) == ("lib.fc", 17, 4), repr((imp_hov(25), imp_def(25))))
check("imports: the last route segment resolves to the module imported from",
      "module deep" in imp_hov(26) and "Deep bag." in imp_hov(26)
      and imp_def(26) == ("lib.fc", 19, 8), repr((imp_hov(26), imp_def(26))))
check("imports: a name imported over a route resolves through it",
      "two: () -> i32" in imp_hov(27) and "Answers two." in imp_hov(27)
      and imp_def(27) == ("lib.fc", 21, 12), repr((imp_hov(27), imp_def(27))))
check("imports: an `as` alias over a route reports the routed symbol",
      "two: () -> i32" in imp_hov(28) and imp_def(28) == ("lib.fc", 21, 12),
      repr((imp_hov(28), imp_def(28))))
check("imports: an unresolved import offers nothing (hover)",
      impresp.get(50, {}).get("result") is None, json.dumps(impresp.get(50)))
check("imports: an unresolved import offers nothing (definition)",
      impresp.get(51, {}).get("result") is None, json.dumps(impresp.get(51)))
# A dot on the LEFT of `from` is rejected in the parser, so there is no Decl to
# walk — the position lookup must simply find nothing rather than reading a
# half-built one.
check("imports: a left-dotted import offers nothing (hover)",
      impresp.get(52, {}).get("result") is None, json.dumps(impresp.get(52)))
check("imports: a left-dotted import offers nothing (definition)",
      impresp.get(53, {}).get("result") is None, json.dumps(impresp.get(53)))
check("imports: the left-dotted spelling is reported, once, as an error",
      [m for m in impbf.get("main.fc", [[]])[-1] if "not allowed on the left of 'from'" in m]
        and len(impbf.get("main.fc", [[]])[-1]) == 2, str(impbf.get("main.fc")))
check("imports: SERVER SURVIVED the malformed-import probes",
      all(i in impresp for i in (50, 51, 52, 53)) and imprc == 0, f"rc={imprc}")

# --- completion inside an import statement ------------------------------------
# An import resolves against its `from` clause, not lexical scope: the left of
# `from` offers what that clause exposes (a module's members, or a namespace's
# top-level symbols), the right offers the modules a route may continue with. An
# import line never falls through to the global keyword/scope list, and each
# probe is a half-typed statement — the state the parser recovers from — so the
# candidates come from the source text, not from a Decl that may not exist.
CIMP_LIB = (
    "module outer =\n"                           # 0
    "    module inner =\n"                       # 1
    "        let bump = (n: i32) ->\n"           # 2
    "            n + 1\n"                        # 3
    "    let double = (n: i32) ->\n"             # 4
    "        n * 2\n"                            # 5
    "    let triple = (n: i32) ->\n"             # 6
    "        n * 3\n"                            # 7
    "    private let hidden = (n: i32) ->\n"     # 8
    "        n\n"                                # 9
    "    module mid =\n"                         # 10
    "        module deep =\n"                    # 11
    "            let two = () ->\n"              # 12
    "                2\n"                        # 13
)
CIMP_NS = (
    "namespace acme::\n\n"
    "struct color =\n    r: u8\n\n"
    "module color =\n    let black = () ->\n        color { r = 0u8 }\n\n"
    "module palette =\n    let shade = () ->\n        7\n"
)
CIMP_NS2 = (
    "namespace acme::deeper::\n\n"
    "module gadget =\n    let go = () ->\n        1\n"
)
cimpdir = tempfile.mkdtemp(prefix="fc_lsp_cimp_")
for nm, txt in (("clib.fc", CIMP_LIB), ("cnslib.fc", CIMP_NS), ("cns2.fc", CIMP_NS2)):
    with open(os.path.join(cimpdir, nm), "w") as f:
        f.write(txt)
cimpuri = "file://" + os.path.join(cimpdir, "doc.fc")

def cdoc(*lines):
    """A probe document: the given lines, then an entry point."""
    return "".join(l + "\n" for l in lines) + "\nlet main = (args: str[]) ->\n    return 0\n"

# (id, label, doc lines, line index of the cursor, text left of the cursor).
# The cursor column is the length of that text, so each probe reads as the
# keystroke that produced it.
CIMP_PROBES = [
    (60, "name list from a module",     ("import d from outer",), 0, "import d"),
    (61, "second item of a list",       ("import double, t from outer",), 0, "import double, t"),
    (62, "the `as` alias position",     ("import double as x from outer",), 0, "import double as x"),
    (63, "the wildcard",                ("import * from outer",), 0, "import * "),
    (64, "name list from a namespace",  ("import c from acme::",), 0, "import c"),
    (65, "name list over a route",      ("import tw from outer.mid.deep",), 0, "import tw"),
    (66, "no `from` written yet",       ("import x",), 0, "import x"),
    (67, "the route head",              ("import x from o",), 0, "import x from o"),
    (68, "after a namespace `::`",      ("import x from acme::p",), 0, "import x from acme::p"),
    (69, "after a route `.`",           ("import x from outer.m",), 0, "import x from outer.m"),
    (70, "deeper in a route",           ("import x from outer.mid.d",), 0, "import x from outer.mid.d"),
    (71, "inside a module body",
         ("module app =", "    import inner from outer", "    import b from inner"), 2,
         "    import b"),
    (72, "inside the `import` keyword", ("import x from outer",), 0, "imp"),
    (73, "inside a trailing comment",   ("import double from outer // no",), 0,
         "import double from outer // n"),
]
cimp = [req(1, "initialize", {"capabilities": {}}), note("initialized", {})]
for n, (rid, _, lines, ln, prefix) in enumerate(CIMP_PROBES):
    text = cdoc(*lines)
    cimp.append(note("textDocument/didOpen" if n == 0 else "textDocument/didChange",
        {"textDocument": {"uri": cimpuri, "languageId": "fc", "version": n + 1,
                          "text": text}} if n == 0 else
        {"textDocument": {"uri": cimpuri, "version": n + 1},
         "contentChanges": [{"text": text}]}))
    cimp.append(req(rid, "textDocument/completion",
        {"textDocument": {"uri": cimpuri},
         "position": {"line": ln, "character": len(prefix)}}))
cimp += [req(9, "shutdown", None), note("exit", None)]
cimpresp, _, _, cimprc, _ = run_session(cimp)

def cimp_labels(rid):
    res = cimpresp.get(rid, {}).get("result") or {}
    its = res.get("items") if isinstance(res, dict) else res
    return [it.get("label") for it in (its or [])]

l60 = cimp_labels(60)
check("import completion: names offered are the module's public members",
      set(l60) >= {"double", "triple", "inner", "mid"} and "hidden" not in l60, str(l60))
check("import completion: a name list never offers keywords or globals",
      "let" not in l60 and "outer" not in l60, str(l60))
check("import completion: a name already in the list is not offered again",
      "triple" in cimp_labels(61) and "double" not in cimp_labels(61), str(cimp_labels(61)))
check("import completion: the `as` alias position offers nothing",
      cimp_labels(62) == [], str(cimp_labels(62)))
check("import completion: a wildcard import offers no names",
      cimp_labels(63) == [], str(cimp_labels(63)))
check("import completion: `from ns::` offers the namespace's top-level symbols",
      set(cimp_labels(64)) >= {"color", "palette"}
      and "shade" not in cimp_labels(64) and "gadget" not in cimp_labels(64),
      str(cimp_labels(64)))
check("import completion: a companion pair is offered once",
      cimp_labels(64).count("color") == 1, str(cimp_labels(64)))
check("import completion: a dotted route resolves to the module it reads from",
      cimp_labels(65) == ["two"], str(cimp_labels(65)))
check("import completion: with no `from` there is nothing to resolve against",
      cimp_labels(66) == [], str(cimp_labels(66)))
l67 = cimp_labels(67)
check("import completion: the route head offers visible modules and namespaces",
      "outer" in l67 and "acme" in l67 and "std" in l67, str(l67))
check("import completion: the head does not offer namespaced modules bare",
      "palette" not in l67 and "gadget" not in l67, str(l67))
check("import completion: the head is modules only, not the global list",
      "let" not in l67 and "main" not in l67, str(l67))
l68 = cimp_labels(68)
check("import completion: `ns::` offers that namespace's modules",
      set(l68) >= {"color", "palette"} and "outer" not in l68, str(l68))
check("import completion: `ns::` also offers the next namespace segment",
      "deeper" in l68 and "gadget" not in l68, str(l68))
check("import completion: a route `.` offers only nested modules",
      set(cimp_labels(69)) == {"inner", "mid"}, str(cimp_labels(69)))
check("import completion: a route continues past its second segment",
      cimp_labels(70) == ["deep"], str(cimp_labels(70)))
check("import completion: a module-body import resolves through its own imports",
      cimp_labels(71) == ["bump"], str(cimp_labels(71)))
check("import completion: the `import` keyword itself is not an import context",
      "let" in cimp_labels(72), str(cimp_labels(72))[:120])
# In the comment the cursor gets what any other comment gets — the ordinary
# global list — not the route's modules-and-namespaces (`acme` is a namespace,
# so it is offered only by the route head).
check("import completion: a trailing comment is not part of the statement",
      "let" in cimp_labels(73) and "acme" not in cimp_labels(73),
      str(cimp_labels(73))[:120])
check("import completion: SERVER SURVIVED every half-typed import",
      all(rid in cimpresp for rid, *_ in CIMP_PROBES) and cimprc == 0, f"rc={cimprc}")

print(f"\n{len(failures)} failure(s)" if failures else "\nall LSP tests passed")
sys.exit(1 if failures else 0)
