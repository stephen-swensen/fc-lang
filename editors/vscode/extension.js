// FC language client — dependency-free.
//
// VSCode's extension host is Node/Electron, so we drive `fcc --lsp` directly
// with the built-in `child_process` + a tiny JSON-RPC transport. No npm install,
// no vscode-languageclient; `make install-vscode` packages these files as a .vsix.

const vscode = require("vscode");
const cp = require("child_process");
const path = require("path");

let proc = null;
let diagColl = null;
let nextId = 1;
const pending = new Map();          // id -> {resolve, reject}
let buf = Buffer.alloc(0);
const versions = new Map();         // uri -> document version
let ready = false;                  // initialize answered; documents may be synced
let stopping = false;               // deactivate() is shutting the server down
const restarts = [];                // times of recent automatic restarts

/* ---- transport ---- */

function send(msg) {
  if (!proc || proc.killed) return false;
  const body = Buffer.from(JSON.stringify(msg), "utf8");
  proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  proc.stdin.write(body);
  return true;
}

// A request to the server. Rejects at once when no server is running, and when
// the server exits before answering (see onExit), so a provider never waits on
// a dead process.
function request(method, params) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    if (!send({ jsonrpc: "2.0", id, method, params })) {
      pending.delete(id);
      reject(new Error("FC language server is not running"));
    }
  });
}

// The result of a request, or `fallback` when the server is unavailable.
async function query(method, params, fallback) {
  try { return await request(method, params); } catch (_) { return fallback; }
}

function notify(method, params) {
  send({ jsonrpc: "2.0", method, params });
}

function onMessage(msg) {
  if (msg.id !== undefined && (msg.result !== undefined || msg.error !== undefined)) {
    const p = pending.get(msg.id);
    if (p) {
      pending.delete(msg.id);
      msg.error ? p.reject(msg.error) : p.resolve(msg.result);
    }
  } else if (msg.method === "textDocument/publishDiagnostics") {
    applyDiagnostics(msg.params);
  }
  // The server issues no client-bound requests, so nothing else to handle.
}

function handleData(chunk) {
  buf = Buffer.concat([buf, chunk]);
  for (;;) {
    const headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    const header = buf.slice(0, headerEnd).toString("ascii");
    const m = /Content-Length:\s*(\d+)/i.exec(header);
    const start = headerEnd + 4;
    if (!m) { buf = buf.slice(start); continue; }
    const len = parseInt(m[1], 10);
    if (buf.length < start + len) return;          // wait for the rest
    const bodyStr = buf.slice(start, start + len).toString("utf8");
    buf = buf.slice(start + len);
    try { onMessage(JSON.parse(bodyStr)); } catch (_) { /* ignore */ }
  }
}

/* ---- LSP <-> VSCode conversions ---- */

function toRange(r) {
  return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function applyDiagnostics(params) {
  if (!diagColl) return;
  const uri = vscode.Uri.parse(params.uri);
  const items = (params.diagnostics || []).map((d) => {
    const diag = new vscode.Diagnostic(
      toRange(d.range),
      d.message,
      (d.severity || 1) - 1                      // LSP Error=1 -> VSCode Error=0
    );
    if (d.source) diag.source = d.source;
    return diag;
  });
  diagColl.set(uri, items);
}

/* ---- document sync (full text) ---- */

function didOpen(doc) {
  if (doc.languageId !== "fc" || !ready) return;
  const uri = doc.uri.toString();
  versions.set(uri, doc.version);
  notify("textDocument/didOpen", {
    textDocument: { uri, languageId: "fc", version: doc.version, text: doc.getText() },
  });
}

function didChange(doc) {
  if (doc.languageId !== "fc" || !ready) return;
  const uri = doc.uri.toString();
  const version = (versions.get(uri) || 0) + 1;
  versions.set(uri, version);
  notify("textDocument/didChange", {
    textDocument: { uri, version },
    contentChanges: [{ text: doc.getText() }],
  });
}

function didClose(doc) {
  if (doc.languageId !== "fc" || !ready) return;
  const uri = doc.uri.toString();
  versions.delete(uri);
  notify("textDocument/didClose", { textDocument: { uri } });
}

function docPos(document, position) {
  return {
    textDocument: { uri: document.uri.toString() },
    position: { line: position.line, character: position.character },
  };
}

// How inferred-type annotations are shown: "inline" (after the name, `let x: T`,
// default), "codelens" (above the line), or "off". The server always offers both
// CodeLens and inlay hints; this picks which the editor actually renders.
function typeDisplayMode() {
  return vscode.workspace.getConfiguration("fc").get("typeDisplay") || "inline";
}

/* ---- activation ---- */

/* ---- server lifecycle ---- */

// Start `fcc --lsp`, initialize it, and send it every open FC document. Returns
// false (after telling the user) when the server cannot be started. When it
// exits unexpectedly it is restarted, up to three times in three minutes.
async function startServer() {
  const cfg = vscode.workspace.getConfiguration("fc");
  const command = cfg.get("serverPath") || "fcc";
  const stdlibPath = cfg.get("stdlibPath") || "";
  const env = Object.assign({}, process.env);
  if (stdlibPath) env.FCC_STDLIB_DIR = stdlibPath;

  ready = false;
  buf = Buffer.alloc(0);
  let child;
  try {
    child = cp.spawn(command, ["--lsp"], { env });
  } catch (err) {
    vscode.window.showErrorMessage(`FC: cannot launch '${command} --lsp': ${err.message}`);
    return false;
  }
  proc = child;
  // A missing binary is reported asynchronously, as an 'error' event, and the
  // process then never answers initialize: race the two.
  const failed = new Promise((resolve) => {
    child.on("error", (err) => resolve(err));
    child.on("exit", (code) => resolve(new Error(`exited with code ${code}`)));
  });
  child.on("exit", () => onExit(child));
  child.stdout.on("data", handleData);
  // Server diagnostics never reach the client over stderr, but surface crashes.
  child.stderr.on("data", (d) => console.error("fcc --lsp:", d.toString()));

  const init = request("initialize", { processId: process.pid, rootUri: null, capabilities: {} })
    .then(() => null, (err) => err);
  const err = await Promise.race([init, failed]);
  if (err) {
    vscode.window.showErrorMessage(
      `FC: language server '${command}' failed to start (${err.message}). Set 'fc.serverPath'.`
    );
    if (proc === child) proc = null;
    return false;
  }
  notify("initialized", {});
  ready = true;
  versions.clear();
  vscode.workspace.textDocuments.forEach(didOpen);
  return true;
}

// The server process ended: fail its outstanding requests and, unless the
// extension is shutting down, start a new one.
function onExit(child) {
  if (proc !== child) return;
  proc = null;
  ready = false;
  for (const p of pending.values()) p.reject(new Error("FC language server exited"));
  pending.clear();
  if (stopping) return;
  const now = Date.now();
  while (restarts.length && now - restarts[0] > 180000) restarts.shift();
  if (restarts.length >= 3) {
    vscode.window.showErrorMessage(
      "FC: the language server keeps exiting; not restarting it. Reload the window to try again."
    );
    return;
  }
  restarts.push(now);
  startServer();
}

/* ---- interactive session (fcc --interactive) ---- */

let session = null;                 // the terminal running the session
let sessionCwd = undefined;         // its working directory
let sessionReady = null;            // resolves once it can be sent input

// The session's terminal, started if there is none. Its first input waits a
// moment: the session reads what arrives early at its first prompt, but the
// terminal echoes whatever it is sent before the session has started.
function interactiveSession(document) {
  if (session && session.exitStatus === undefined) return session;
  const cfg = vscode.workspace.getConfiguration("fc");
  const stdlibPath = cfg.get("stdlibPath") || "";
  const folder = document && vscode.workspace.getWorkspaceFolder(document.uri);
  sessionCwd = folder ? folder.uri.fsPath
    : document && document.uri.scheme === "file" ? path.dirname(document.uri.fsPath)
    : undefined;
  session = vscode.window.createTerminal({
    name: "FC Interactive",
    shellPath: cfg.get("serverPath") || "fcc",
    shellArgs: ["--interactive"],
    cwd: sessionCwd,
    env: stdlibPath ? { FCC_STDLIB_DIR: stdlibPath } : undefined,
  });
  sessionReady = Promise.resolve(session.processId)
    .then(() => new Promise((resolve) => setTimeout(resolve, 500)));
  return session;
}

// A line that begins a top-level item: one at the left margin that does not
// go on with the item above it (a match arm, `else`, a closing bracket).
function startsItem(document, line) {
  const text = document.lineAt(line).text;
  return /^\S/.test(text) && !/^([|)\]}]|(else|then|with|do|in)\b)/.test(text);
}

function isComment(document, line) {
  return /^\s*(\/\/|$)/.test(document.lineAt(line).text);
}

// The top-level item at `line`, from the line that begins it to the line
// before the next, and with the item below them for a comment at the left
// margin. { first, last }, or null on a blank line.
function itemAt(document, line) {
  if (document.lineAt(line).isEmptyOrWhitespace) return null;
  let first = line;
  while (first > 0 && !startsItem(document, first)) first--;
  let last = line;
  for (;;) {
    while (last + 1 < document.lineCount && !startsItem(document, last + 1)) last++;
    let onlyComments = true;
    for (let l = first; l <= last && onlyComments; l++) onlyComments = isComment(document, l);
    if (!onlyComments || last + 1 >= document.lineCount) break;
    last++;
  }
  while (last > first && document.lineAt(last).isEmptyOrWhitespace) last--;
  return { first, last };
}

// What Shift+Enter sends: { text, line } (the 0-based line it starts on)
// and, with no selection, `next`, the line to move to afterwards.
//   - A selection, whole lines from its first: a selection that begins in a
//     line's indentation takes the indentation too, and one inside a line is
//     put in its column, so the session reads it as written there. A
//     selection that ends at the start of a line ends with the line before.
//   - With no selection, the top-level item at the cursor.
function textToSend(editor) {
  const document = editor.document;
  const sel = editor.selection;
  if (sel.isEmpty) {
    const item = itemAt(document, sel.active.line);
    let next = item ? item.last + 1 : sel.active.line + 1;
    while (next < document.lineCount && document.lineAt(next).isEmptyOrWhitespace) next++;
    if (next >= document.lineCount) next = document.lineCount - 1;
    if (!item) return { next };
    const range = new vscode.Range(item.first, 0, item.last, document.lineAt(item.last).text.length);
    return { text: document.getText(range), line: item.first, next };
  }
  let start = sel.start, end = sel.end;
  if (end.character === 0 && end.line > start.line)
    end = new vscode.Position(end.line - 1, document.lineAt(end.line - 1).text.length);
  if (/^\s*$/.test(document.lineAt(start.line).text.slice(0, start.character)))
    start = new vscode.Position(start.line, 0);
  let text = document.getText(new vscode.Range(start, end));
  if (start.line === end.line) text = " ".repeat(start.character) + text;
  return { text, line: start.line };
}

// The file as the session is told of it: relative to its working directory
// when inside it, so diagnostics stay short and still open on a click.
function sessionFileName(document) {
  if (document.uri.scheme !== "file") return document.fileName;
  const file = document.uri.fsPath;
  const rel = sessionCwd ? path.relative(sessionCwd, file) : "";
  return rel && !rel.startsWith("..") && !path.isAbsolute(rel) ? rel : file;
}

// An input for the session: a paste (so it is one input, blank lines and
// all), headed by `#line`, so its diagnostics name the file and its lines.
function sessionInput(text, line, document) {
  const file = sessionFileName(document).replace(/[\\"]/g, "\\$&");
  return `\x1b[200~#line ${line + 1} "${file}"\n${text.replace(/\r\n/g, "\n")}\n\x1b[201~`;
}

async function sendToInteractive() {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  const document = editor.document;
  const part = textToSend(editor);
  if (part.text !== undefined) {
    const terminal = interactiveSession(document);
    terminal.show(true);
    await sessionReady;
    terminal.sendText(sessionInput(part.text, part.line, document), false);
  }
  if (part.next !== undefined) {
    const at = new vscode.Position(part.next, 0);
    editor.selection = new vscode.Selection(at, at);
    editor.revealRange(new vscode.Range(at, at));
  }
}

/* ---- activation ---- */

async function activate(context) {
  diagColl = vscode.languages.createDiagnosticCollection("fc");
  context.subscriptions.push(diagColl);

  // The session needs no language server, so it is set up first.
  context.subscriptions.push(
    vscode.commands.registerCommand("fc.sendToInteractive", sendToInteractive),
    vscode.commands.registerCommand("fc.startInteractive", () => {
      const editor = vscode.window.activeTextEditor;
      interactiveSession(editor && editor.document).show();
    }),
    vscode.window.onDidCloseTerminal((t) => { if (t === session) session = null; })
  );

  // startServer sends the already-open FC documents; then watch for changes.
  if (!(await startServer())) return;

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(didOpen),
    vscode.workspace.onDidChangeTextDocument((e) => didChange(e.document)),
    vscode.workspace.onDidCloseTextDocument(didClose)
  );

  const selector = { scheme: "file", language: "fc" };

  // Fired on `fc.typeDisplay` change so VSCode re-queries the providers below.
  const codeLensEmitter = new vscode.EventEmitter();
  const inlayEmitter = new vscode.EventEmitter();
  context.subscriptions.push(codeLensEmitter, inlayEmitter);

  context.subscriptions.push(
    vscode.languages.registerHoverProvider(selector, {
      async provideHover(document, position) {
        const r = await query("textDocument/hover", docPos(document, position), null);
        if (!r || !r.contents) return null;
        const md = new vscode.MarkdownString(
          typeof r.contents === "string" ? r.contents : r.contents.value
        );
        return new vscode.Hover(md, r.range ? toRange(r.range) : undefined);
      },
    }),

    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(document, position) {
        const r = await query("textDocument/definition", docPos(document, position), null);
        if (!r) return null;
        const locs = Array.isArray(r) ? r : [r];
        return locs.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), toRange(l.range)));
      },
    }),

    vscode.languages.registerCompletionItemProvider(
      selector,
      {
        async provideCompletionItems(document, position, _token, context) {
          // Forward the trigger context so the server can keep a trigger-char
          // auto-pop quiet when it isn't a real member access (a lone `:`),
          // while still answering an explicit Ctrl+Space.
          const params = docPos(document, position);
          if (context) {
            params.context = {
              triggerKind: context.triggerKind,
              triggerCharacter: context.triggerCharacter,
            };
          }
          const r = await query("textDocument/completion", params, []);
          const items = Array.isArray(r) ? r : (r && r.items) || [];
          return items.map((it) => {
            const ci = new vscode.CompletionItem(
              it.label,
              it.kind ? it.kind - 1 : undefined   // LSP kind -> VSCode kind
            );
            if (it.detail) ci.detail = it.detail;
            return ci;
          });
        },
      },
      // Trigger characters. This manual provider gates auto-completion itself —
      // the server's completionProvider.triggerCharacters capability is NOT
      // consulted (we don't route completion through vscode-languageclient), so
      // these must be kept in sync with it.
      ".",
      ":"
    ),

    vscode.languages.registerCodeLensProvider(selector, {
      onDidChangeCodeLenses: codeLensEmitter.event,
      async provideCodeLenses(document) {
        if (typeDisplayMode() !== "codelens") return [];
        const r = await query("textDocument/codeLens", {
          textDocument: { uri: document.uri.toString() },
        }, []);
        return (r || []).map(
          (l) => new vscode.CodeLens(toRange(l.range), l.command || { title: "", command: "" })
        );
      },
    }),

    vscode.languages.registerInlayHintsProvider(selector, {
      onDidChangeInlayHints: inlayEmitter.event,
      async provideInlayHints(document, range) {
        if (typeDisplayMode() !== "inline") return [];
        const r = await query("textDocument/inlayHint", {
          textDocument: { uri: document.uri.toString() },
          range: {
            start: { line: range.start.line, character: range.start.character },
            end: { line: range.end.line, character: range.end.character },
          },
        }, []);
        return (r || []).map(
          (h) =>
            new vscode.InlayHint(
              new vscode.Position(h.position.line, h.position.character),
              h.label,
              vscode.InlayHintKind.Type
            )
        );
      },
    })
  );

  // Flipping fc.typeDisplay changes which provider yields results; re-query both.
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("fc.typeDisplay")) {
        codeLensEmitter.fire();
        inlayEmitter.fire();
      }
    })
  );
}

function deactivate() {
  stopping = true;
  if (proc && !proc.killed) {
    try { notify("shutdown", null); notify("exit", null); } catch (_) {}
    proc.kill();
  }
}

module.exports = { activate, deactivate };
