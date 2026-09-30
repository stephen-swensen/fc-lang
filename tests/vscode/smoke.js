// Smoke test for the VS Code extension (editors/vscode/extension.js), run
// against a real `fcc --lsp` with the VS Code API mocked (mock/vscode): a
// missing server binary is reported instead of hanging activation, each open
// document is opened on the server once, hover works, and a server that dies is
// restarted. `make test-vscode` runs it (needs node).
const vscode = require("vscode");
const cp = require("child_process");
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
function check(name, ok) { console.log(`  ${ok ? "PASS" : "FAIL"}  ${name}`); if (!ok) failed++; }
function timeout(p, ms, what){ return Promise.race([p, new Promise((_,rej)=>setTimeout(()=>rej(new Error("TIMEOUT "+what)), ms))]); }
(async () => {
  // 1. missing binary: activation returns and reports, no hang
  vscode._setConfig({ serverPath: "/nonexistent/fcc" });
  await timeout(ext.activate({ subscriptions: [] }), 5000, "activate(missing)");
  check("missing binary reported", vscode._errors.length === 1 && /failed to start/.test(vscode._errors[0]));
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
  ext.deactivate();
  setTimeout(() => process.exit(failed ? 1 : 0), 300);
})().catch(e => { console.log("ERROR", e.message); process.exit(1); });
