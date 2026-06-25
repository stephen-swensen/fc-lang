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
    "let helper = (n: int32) ->\n"      # line 0
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
    hover(2, 3, 8),                                                      # x -> int32
    hover(3, 3, 13),                                                     # helper -> func
    req(4, "textDocument/definition", {"textDocument": {"uri": URI}, "position": {"line": 3, "character": 13}}),
    req(5, "textDocument/codeLens", {"textDocument": {"uri": URI}}),
    req(6, "textDocument/completion", {"textDocument": {"uri": URI}, "position": {"line": 4, "character": 4}}),
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
      and caps.get("textDocumentSync") == 1, str(caps))

check("clean document has no diagnostics",
      len(diags) >= 1 and diags[0] == [], str(diags[0] if diags else None))

hx = responses.get(2, {}).get("result") or {}
check("hover 'x' shows int32",
      "int32" in json.dumps(hx), json.dumps(hx))

hh = responses.get(3, {}).get("result") or {}
check("hover 'helper' shows a function type",
      "->" in json.dumps(hh), json.dumps(hh))

dfn = responses.get(4, {}).get("result")
check("definition of 'helper' returns line 0",
      isinstance(dfn, dict) and dfn.get("range", {}).get("start", {}).get("line") == 0,
      json.dumps(dfn))

cl = responses.get(5, {}).get("result") or []
check("codeLens returns type lenses",
      isinstance(cl, list) and len(cl) > 0
      and any(": " in (x.get("command", {}).get("title", "")) for x in cl),
      json.dumps(cl)[:200])

comp = responses.get(6, {}).get("result") or {}
labels = [it.get("label") for it in (comp.get("items") if isinstance(comp, dict) else comp) or []]
check("completion includes keywords", "let" in labels and "match" in labels,
      str(labels[:10]))

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
    f.write("module prelude =\n    let double = (n: int32) ->\n        n * 2\n")
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

print(f"\n{len(failures)} failure(s)" if failures else "\nall LSP tests passed")
sys.exit(1 if failures else 0)
