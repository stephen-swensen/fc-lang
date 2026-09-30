// The part of the VS Code API that editors/vscode/extension.js uses, mocked
// for tests/vscode/smoke.js.
const errors = [];
const providers = {};
let config = {};
const docs = [];
class Range { constructor(a,b,c,d){ this.a=[a,b,c,d]; } }
class Position { constructor(l,c){ this.line=l; this.character=c; } }
class MarkdownString { constructor(v){ this.value=v; } }
class Hover { constructor(md, r){ this.md=md; this.r=r; } }
class EventEmitter { constructor(){ this.event=()=>{}; } fire(){} dispose(){} }
module.exports = {
  _errors: errors, _providers: providers, _setConfig: (c)=>{ config=c; }, _docs: docs,
  Range, Position, MarkdownString, Hover, EventEmitter,
  Location: class {}, CompletionItem: class { constructor(l){ this.label=l; } },
  CodeLens: class {}, InlayHint: class {}, InlayHintKind: { Type: 1 },
  Diagnostic: class { constructor(r,m){ this.m=m; } },
  Uri: { parse: (s)=>({ s, toString(){ return s; } }) },
  window: { showErrorMessage: (m)=>{ errors.push(m); } },
  workspace: {
    getConfiguration: ()=>({ get: (k)=>config[k] }),
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
