// FC language client — dependency-free.
//
// VSCode's extension host is Node/Electron, so we drive `fcc --lsp` directly
// with the built-in `child_process` + a tiny JSON-RPC transport. No npm install,
// no vscode-languageclient. `make install-vscode` is therefore a plain copy.

const vscode = require("vscode");
const cp = require("child_process");

let proc = null;
let diagColl = null;
let nextId = 1;
const pending = new Map();          // id -> {resolve, reject}
let buf = Buffer.alloc(0);
const versions = new Map();         // uri -> document version
let ready = false;

/* ---- transport ---- */

function send(msg) {
  if (!proc || proc.killed) return;
  const body = Buffer.from(JSON.stringify(msg), "utf8");
  proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  proc.stdin.write(body);
}

function request(method, params) {
  const id = nextId++;
  send({ jsonrpc: "2.0", id, method, params });
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
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
  if (doc.languageId !== "fc") return;
  const uri = doc.uri.toString();
  versions.set(uri, doc.version);
  notify("textDocument/didOpen", {
    textDocument: { uri, languageId: "fc", version: doc.version, text: doc.getText() },
  });
}

function didChange(doc) {
  if (doc.languageId !== "fc") return;
  const uri = doc.uri.toString();
  const version = (versions.get(uri) || 0) + 1;
  versions.set(uri, version);
  notify("textDocument/didChange", {
    textDocument: { uri, version },
    contentChanges: [{ text: doc.getText() }],
  });
}

function didClose(doc) {
  if (doc.languageId !== "fc") return;
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

async function activate(context) {
  const cfg = vscode.workspace.getConfiguration("fc");
  const command = cfg.get("serverPath") || "fcc";
  const stdlibPath = cfg.get("stdlibPath") || "";
  const env = Object.assign({}, process.env);
  if (stdlibPath) env.FCC_STDLIB_DIR = stdlibPath;

  try {
    proc = cp.spawn(command, ["--lsp"], { env });
  } catch (err) {
    vscode.window.showErrorMessage(`FC: cannot launch '${command} --lsp': ${err.message}`);
    return;
  }
  proc.on("error", (err) =>
    vscode.window.showErrorMessage(
      `FC: language server '${command}' failed to start (${err.message}). Set 'fc.serverPath'.`
    )
  );
  proc.stdout.on("data", handleData);
  // Server diagnostics never reach the client over stderr, but surface crashes.
  proc.stderr.on("data", (d) => console.error("fcc --lsp:", d.toString()));

  diagColl = vscode.languages.createDiagnosticCollection("fc");
  context.subscriptions.push(diagColl);

  await request("initialize", {
    processId: process.pid,
    rootUri: null,
    capabilities: {},
  });
  notify("initialized", {});
  ready = true;

  // Sync already-open FC documents, then watch for changes.
  vscode.workspace.textDocuments.forEach(didOpen);
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
        const r = await request("textDocument/hover", docPos(document, position));
        if (!r || !r.contents) return null;
        const md = new vscode.MarkdownString(
          typeof r.contents === "string" ? r.contents : r.contents.value
        );
        return new vscode.Hover(md, r.range ? toRange(r.range) : undefined);
      },
    }),

    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(document, position) {
        const r = await request("textDocument/definition", docPos(document, position));
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
          // auto-pop (e.g. after `->`) quiet when it isn't a real member access,
          // while still answering an explicit Ctrl+Space.
          const params = docPos(document, position);
          if (context) {
            params.context = {
              triggerKind: context.triggerKind,
              triggerCharacter: context.triggerCharacter,
            };
          }
          const r = await request("textDocument/completion", params);
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
      // these must be kept in sync with it. '>' completes `->` (member access on
      // a pointer); the server checks the preceding '-'.
      ".",
      ":",
      ">"
    ),

    vscode.languages.registerCodeLensProvider(selector, {
      onDidChangeCodeLenses: codeLensEmitter.event,
      async provideCodeLenses(document) {
        if (typeDisplayMode() !== "codelens") return [];
        const r = await request("textDocument/codeLens", {
          textDocument: { uri: document.uri.toString() },
        });
        return (r || []).map(
          (l) => new vscode.CodeLens(toRange(l.range), l.command || { title: "", command: "" })
        );
      },
    }),

    vscode.languages.registerInlayHintsProvider(selector, {
      onDidChangeInlayHints: inlayEmitter.event,
      async provideInlayHints(document, range) {
        if (typeDisplayMode() !== "inline") return [];
        const r = await request("textDocument/inlayHint", {
          textDocument: { uri: document.uri.toString() },
          range: {
            start: { line: range.start.line, character: range.start.character },
            end: { line: range.end.line, character: range.end.character },
          },
        });
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
  if (proc && !proc.killed) {
    try { notify("shutdown", null); notify("exit", null); } catch (_) {}
    proc.kill();
  }
}

module.exports = { activate, deactivate };
