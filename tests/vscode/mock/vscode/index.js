// The part of the VS Code API that editors/vscode/extension.js uses, mocked
// for tests/vscode/smoke.js.
const errors = [];
const providers = {};
let config = {};
const docs = [];
const commands = {};
const terminals = [];
let editor = null, folder = undefined;
class Position { constructor(l,c){ this.line=l; this.character=c; } }
class Range {
  constructor(a,b,c,d){
    this.a=[a,b,c,d];
    if (typeof a === "number") { this.start=new Position(a,b); this.end=new Position(c,d); }
    else { this.start=a; this.end=b; }
  }
}
class Selection extends Range {
  constructor(anchor, active){
    super(anchor, active);
    this.anchor=anchor; this.active=active;
    this.isEmpty = anchor.line===active.line && anchor.character===active.character;
  }
}
// A terminal records what it is sent.
function createTerminal(options){
  const t = { options, sent: [], shown: 0, exitStatus: undefined, processId: Promise.resolve(1),
              show(){ this.shown++; }, sendText(text){ this.sent.push(text); } };
  terminals.push(t);
  return t;
}
class MarkdownString { constructor(v){ this.value=v; } }
class Hover { constructor(md, r){ this.md=md; this.r=r; } }
class EventEmitter { constructor(){ this.event=()=>{}; } fire(){} dispose(){} }
module.exports = {
  _errors: errors, _providers: providers, _setConfig: (c)=>{ config=c; }, _docs: docs,
  _commands: commands, _terminals: terminals,
  _setEditor: (e)=>{ editor=e; }, _setFolder: (f)=>{ folder=f; },
  Range, Position, Selection, MarkdownString, Hover, EventEmitter,
  Location: class {}, CompletionItem: class { constructor(l){ this.label=l; } },
  CodeLens: class {}, InlayHint: class {}, InlayHintKind: { Type: 1 },
  Diagnostic: class { constructor(r,m){ this.m=m; } },
  Uri: { parse: (s)=>({ s, toString(){ return s; } }) },
  window: {
    showErrorMessage: (m)=>{ errors.push(m); },
    get activeTextEditor(){ return editor; },
    createTerminal, onDidCloseTerminal: ()=>({}),
  },
  commands: { registerCommand: (id, f)=>{ commands[id]=f; return {}; } },
  workspace: {
    getConfiguration: ()=>({ get: (k)=>config[k] }),
    getWorkspaceFolder: ()=>folder,
    textDocuments: docs,
    onDidOpenTextDocument: ()=>({}), onDidChangeTextDocument: ()=>({}),
    onDidCloseTextDocument: ()=>({}), onDidChangeConfiguration: ()=>({}),
  },
  languages: {
    createDiagnosticCollection: ()=>({ set(){}, dispose(){} }),
    registerHoverProvider: (s,p)=>{ providers.hover=p; return {}; },
    registerDefinitionProvider: (s,p)=>{ providers.def=p; return {}; },
    registerCompletionItemProvider: (s,p)=>{ providers.comp=p; return {}; },
    registerCodeLensProvider: (s,p)=>{ providers.lens=p; return {}; },
    registerInlayHintsProvider: (s,p)=>{ providers.inlay=p; return {}; },
  },
};
