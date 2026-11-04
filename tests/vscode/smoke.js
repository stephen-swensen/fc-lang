// Smoke test for the VS Code extension (editors/vscode/extension.js), run
// against a real `fcc --lsp` with the VS Code API mocked (mock/vscode): a
// missing server binary is reported instead of hanging activation, each open
// document is opened on the server once, hover works, and a server that dies is
// restarted. Then Shift+Enter: what it sends the interactive session, and what
// a real `fcc --interactive` makes of it. `make test-vscode` runs it (needs
// node).
const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
// Count the didOpen notifications the extension writes to the server.
let opens = 0;
const realSpawn = cp.spawn;
cp.spawn = (...args) => {
  const child = realSpawn(...args);
  const write = child.stdin.write.bind(child.stdin);
  child.stdin.write = (data, ...rest) => {
    if (String(data).includes('"textDocument/didOpen"')) opens++;
    return write(data, ...rest);
  };
  return child;
};
const ext = require("../../editors/vscode/extension.js");
const FCC = process.argv[2];
const doc = { languageId: "fc", version: 1, uri: { toString(){ return "file:///tmp/smoke_doc.fc"; } },
              getText(){ return "let main = (args: str[]) ->\n    let x = 41\n    return x\n"; } };
let failed = 0;

// A document in the editor, of the file `file` holding `text`.
function editorDoc(text, file) {
  const lines = text.split("\n");
  return {
    languageId: "fc", version: 1, fileName: file, lineCount: lines.length,
    uri: { scheme: "file", fsPath: file, toString(){ return "file://" + file; } },
    lineAt(i){ return { text: lines[i], isEmptyOrWhitespace: /^\s*$/.test(lines[i]) }; },
    getText(r){
      const out = lines.slice(r.start.line, r.end.line + 1);
      out[out.length - 1] = out[out.length - 1].slice(0, r.end.character);
      out[0] = out[0].slice(r.start.character);
      return out.join("\n");
    },
  };
}

// The session's output for one input, as the extension sends it: through a
// pipe, so without the paste markers a terminal's editor reads.
function session(input, cwd) {
  const text = input.replace(/^\x1b\[200~/, "").replace(/\x1b\[201~$/, "");
  const r = cp.spawnSync(FCC, ["--interactive"], { input: text, cwd });
  return r.stdout.toString() + r.stderr.toString();
}

async function shiftEnter() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "fc_smoke_"));
  const src = [
    "import io from std::",                 // 0
    "",
    "let twice = (n: i32) -> n * 2",        // 2
    "",
    "let main = (args: str[]) ->",          // 4
    "    let x = 20",
    "    let y = twice(x)",                 // 6
    "",
    "    y + 1",
    "",
    "// sums",                              // 10
    "let add = (a: i32, b: i32) ->",
    "    a + b",                            // 12
    "",
  ].join("\n");
  const file = path.join(dir, "prog.fc");
  fs.writeFileSync(file, src);
  const sdoc = editorDoc(src, file);
  vscode._setFolder({ uri: { fsPath: dir } });
  const P = (l, c) => new vscode.Position(l, c);
  const editor = { document: sdoc, selection: new vscode.Selection(P(6, 4), P(6, 4)), revealRange(){} };
  vscode._setEditor(editor);
  const send = vscode._commands["fc.sendToInteractive"];
  const sent = () => vscode._terminals.length ? vscode._terminals[0].sent : [];

  await send();
  const t = vscode._terminals[0];
  check("Shift+Enter starts the session in a terminal",
        vscode._terminals.length === 1 && t.options.shellPath === FCC &&
        t.options.shellArgs.join(" ") === "--interactive" && t.options.cwd === dir && t.shown === 1);
  check("with no selection it sends the top-level item at the cursor",
        sent()[0] === '\x1b[200~#line 5 "prog.fc"\nlet main = (args: str[]) ->\n    let x = 20\n' +
                      '    let y = twice(x)\n\n    y + 1\n\x1b[201~');
  check("and moves to the next item", editor.selection.active.line === 10);
  await send();
  check("a comment goes with the item below it", sent()[1] ===
        '\x1b[200~#line 11 "prog.fc"\n// sums\nlet add = (a: i32, b: i32) ->\n    a + b\n\x1b[201~');
  check("the session is started once", vscode._terminals.length === 1);

  editor.selection = new vscode.Selection(P(6, 12), P(6, 20));
  await send();
  check("a selection inside a line is sent in its column",
        sent()[2] === '\x1b[200~#line 7 "prog.fc"\n            twice(x)\n\x1b[201~');
  editor.selection = new vscode.Selection(P(5, 4), P(7, 0));
  await send();
  check("a selection of whole lines is sent with their indentation",
        sent()[3] === '\x1b[200~#line 6 "prog.fc"\n    let x = 20\n    let y = twice(x)\n\x1b[201~');
  check("a selection is sent where it is", editor.selection.start.line === 5);

  check("the session reads a selection as written in the file",
        session(sent()[3], dir) === "prog.fc:7:13: error: 'twice' is not defined in this session " +
                                    "yet (prog.fc declares it at line 3)\n");
  check("and an item as the file declares it",
        session('\x1b[200~#line 3 "prog.fc"\nlet twice = (n: i32) -> n * 2\nlet x = twice(4)\n\x1b[201~', dir)
        === "val twice: (i32) -> i32\nval x: i32 = 8\n");
  fs.rmSync(dir, { recursive: true });
}
function check(name, ok) { console.log(`  ${ok ? "PASS" : "FAIL"}  ${name}`); if (!ok) failed++; }
function timeout(p, ms, what){ return Promise.race([p, new Promise((_,rej)=>setTimeout(()=>rej(new Error("TIMEOUT "+what)), ms))]); }
(async () => {
  // 1. missing binary: activation returns and reports, no hang
  vscode._setConfig({ serverPath: "/nonexistent/fcc" });
  await timeout(ext.activate({ subscriptions: [] }), 5000, "activate(missing)");
  check("missing binary reported", vscode._errors.length === 1 && /failed to start/.test(vscode._errors[0]));
  check("the session commands are there without a server",
        !!vscode._commands["fc.sendToInteractive"] && !!vscode._commands["fc.startInteractive"]);
  // 2. real server: hover works
  vscode._setConfig({ serverPath: FCC });
  vscode._docs.push(doc);
  await timeout(ext.activate({ subscriptions: [] }), 10000, "activate(real)");
  check("the open document is sent once", opens === 1);
  const pos = { line: 1, character: 8 };
  let h = await timeout(vscode._providers.hover.provideHover(doc, pos), 10000, "hover1");
  check("hover works", !!h && /i32/.test(h.md.value));
  // 3. kill the server: restarted, hover works again
  cp.execSync(`pkill -f '${FCC.replace(/^(.*\/)?(.)/, "$1[$2]")} --lsp'; true`);
  await new Promise(r => setTimeout(r, 1500));
  h = await timeout(vscode._providers.hover.provideHover(doc, pos), 10000, "hover2");
  check("hover works after the server is restarted", !!h && /i32/.test(h.md.value));
  check("the restarted server is sent the open document once", opens === 2);
  await shiftEnter();
  ext.deactivate();
  setTimeout(() => process.exit(failed ? 1 : 0), 300);
})().catch(e => { console.log("ERROR", e.message); process.exit(1); });
