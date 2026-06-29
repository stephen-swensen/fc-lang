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

msgs = [
    req(1, "initialize", {"capabilities": {}}),
    note("initialized", {}),
    open_doc(1, CLEAN),                                                  # diag[0] = clean
    hover(2, 3, 8),                                                      # x -> i32
    hover(3, 3, 13),                                                     # helper -> func
    req(4, "textDocument/definition", {"textDocument": {"uri": URI}, "position": {"line": 3, "character": 13}}),
    req(5, "textDocument/codeLens", {"textDocument": {"uri": URI}}),
    req(6, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 4, "character": 4}}),
    req(10, "textDocument/inlayHint", {"textDocument": {"uri": URI},     # inline type hints
        "range": {"start": {"line": 0, "character": 0}, "end": {"line": 5, "character": 0}}}),
    change(2, TYPEERR),                                                  # diag[1] = type error
    change(3, BROKEN),                                                   # diag[2] = unterminated
    hover(7, 0, 4),                                                      # SURVIVAL: must still reply
    change(4, UNICODE),                                                  # diag[3]
    hover(8, 2, 12),                                                     # greeting after a multibyte line
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

print(f"\n{len(failures)} failure(s)" if failures else "\nall LSP tests passed")
sys.exit(1 if failures else 0)
