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
    f.write("let ok = (n: i32) ->\n    n + 1\n")
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
# numeric type name -> min/max/bits (+ float nan/inf/neg_inf/epsilon); `->`
# dereferences a pointer to its struct's fields. Each context returns EXACTLY its
# synthetic set (member completion replaces, not augments, the global list), with
# the right result type as `detail`.
SYN = (
    "module m =\n"                                 # 0
    "    struct pt =\n"                            # 1
    "        x: i32\n"                             # 2
    "        y: i32\n"                             # 3
    "    let f = (s: u8[], p: pt*, o: i32?) ->\n"  # 4
    "        let a = s.len\n"                      # 5  slice '.'
    "        let b = p->x\n"                       # 6  pointer '->'
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
    synreq(3, 6, 20),   # p->x|
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
check("completion: pointer '->' offers exactly the pointee struct's fields",
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
# `->` is overloaded (deref, lambda body, fn-type, match arm). At a NON-member
# `->` — e.g. a lambda's `(params) ->` — completion must be empty, NOT the global
# dump, regardless of triggerKind: once VSCode has a suggest session open it
# re-queries as Invoked (kind=1) even as you type through `->`, so suppressing
# only TriggerCharacter (kind=2) would still leak globals (the reported bug). A
# genuine `ptr->` still completes the pointee's fields under either kind.
TRIG = (
    "module m =\n"                       # 0
    "    struct pt =\n"                  # 1
    "        x: i32\n"                   # 2
    "    let f = (p: pt*) ->\n"          # 3  ') ->' is a lambda body, not a deref
    "        let b = p->x\n"             # 4  real pointer dereference
    "        0\n")                       # 5
def trigreq(i, ln, ch, kind):
    p = {"textDocument": {"uri": URI}, "position": {"line": ln, "character": ch},
         "context": {"triggerKind": kind, "triggerCharacter": ">"}}
    return req(i, "textDocument/completion", p)
lam_col = TRIG.split("\n")[3].index("->") + 2      # just past the lambda '->'
der_col = TRIG.split("\n")[4].index("p->") + 3     # just past 'p->'
trg = [
    req(1, "initialize", {"capabilities": {}}), note("initialized", {}),
    open_doc(1, TRIG),
    trigreq(2, 3, lam_col, 2),   # lambda '->', TriggerCharacter -> empty
    trigreq(3, 3, lam_col, 1),   # lambda '->', Invoked (persisted session) -> empty
    trigreq(4, 4, der_col, 2),   # real   '->', TriggerCharacter -> pt fields
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
check("completion: a genuine pointer '->' still completes fields under a trigger char",
      set(trg_labels(4)) == {"x"}, str(trg_labels(4)))

# --- completion: member access on a composite object (index / call / nested) --
# The object before a '.'/'->' need not be a bare identifier. `dict[i].` ends in
# ']', `mk().` in ')', which no leaf node's position span covers — so the server
# captures the field node by its operator position and reads field.object.type.
# Index→struct, call→struct, and index→pointer '->' must all complete the fields.
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
    "        let c = pp[0]->bit0\n"                      # 9  index  -> pointer '->'
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
    cmpreq(4, 9, "pp[0]->"),
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
check("completion: a slice-of-pointers element 'arr[i]->' completes pointee fields",
      cmp_labels(4) == {"bit0", "bit1"}, str(cmp_labels(4)))

print(f"\n{len(failures)} failure(s)" if failures else "\nall LSP tests passed")
sys.exit(1 if failures else 0)
