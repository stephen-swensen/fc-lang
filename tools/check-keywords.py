#!/usr/bin/env python3
"""Check every hand-kept list of FC keywords against the lexer's table.

The lexer's KEYWORDS table (src/lexer.c) decides what is a keyword. These lists
repeat it and must agree:

  - token_kind_name in src/token.c spells each keyword token as 'word';
  - the spec's "reserved words" and "reserved identifiers" lists;
  - the VS Code TextMate grammar, the Vim syntax file, and the highlight.js
    grammar embedded in the spec, which highlight keywords (and must not
    highlight words that are not keywords);
  - src/builtin_docs.inc, the language server's hover text: every reserved
    identifier (a built-in operator) has an entry, and every entry names a
    keyword or a built-in global.

Run from the repository root; `make check` runs it. Exits 1 on a mismatch.
"""
import json
import re
import sys

failures = []


def fail(msg):
    failures.append(msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


# The lexer's table: KW("word", TOK_KIND).
lexer = read("src/lexer.c")
table = dict(re.findall(r'KW\("([a-z_]+)",\s*(TOK_[A-Z_]+)\)', lexer))
keywords = set(table)
if not keywords:
    sys.exit("check-keywords: no KW(...) entries found in src/lexer.c")

# token_kind_name: case TOK_X: return "'word'";
token_c = read("src/token.c")
names = dict(re.findall(r'case (TOK_[A-Z_]+):\s*return "\'([a-z_]+)\'";', token_c))
for word, kind in sorted(table.items()):
    if names.get(kind) != word:
        fail(f"src/token.c: token_kind_name({kind}) should be \"'{word}'\", "
             f"is {names.get(kind)!r}")

# The spec: the backquoted words in the paragraph after each heading line.
spec = read("spec/fc-spec.html")
spec_words = set()
spec_lists = {}
for heading in ("reserved words", "reserved identifiers"):
    m = re.search(r"\*\*" + heading + r"\*\*[^\n]*\n\n([^\n]*)", spec)
    if not m:
        fail(f"spec/fc-spec.html: no **{heading}** list found")
        continue
    spec_lists[heading] = set(re.findall(r"`([a-z_]+)`", m.group(1)))
    spec_words |= spec_lists[heading]
for w in sorted(keywords - spec_words):
    fail(f"spec/fc-spec.html: keyword '{w}' is missing from the reserved lists")
for w in sorted(spec_words - keywords):
    fail(f"spec/fc-spec.html: '{w}' is listed as reserved but is not a keyword")

# Words that editors highlight as something other than a keyword: built-in
# globals, and contextual words that are keywords only in one position
# (`define` in a module header, `of` in an enum header).
NOT_KEYWORDS = {"stdin", "stdout", "stderr", "define", "of"}
TYPE_NAMES = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize",
              "usize", "f32", "f64", "bool", "char", "str", "cstr", "any"}


def check_editor(path, words):
    words -= NOT_KEYWORDS | TYPE_NAMES
    for w in sorted(keywords - words):
        fail(f"{path}: keyword '{w}' is not highlighted")
    for w in sorted(words - keywords):
        fail(f"{path}: '{w}' is highlighted as a keyword but is not one")


# VS Code: every \b(a|b|c)\b alternation in the grammar's match patterns.
grammar = json.loads(read("editors/vscode/syntaxes/fc.tmLanguage.json"))
gwords = set()


def walk(node):
    if isinstance(node, dict):
        pat = node.get("match")
        if isinstance(pat, str):
            for group in re.findall(r"\\b\(([a-z_|0-9]+)\)\\b", pat):
                gwords.update(group.split("|"))
        for v in node.values():
            walk(v)
    elif isinstance(node, list):
        for v in node:
            walk(v)


walk(grammar)
check_editor("editors/vscode/syntaxes/fc.tmLanguage.json", gwords)

# Vim: `syn keyword Group word word ...` lines.
vwords = set()
for line in read("editors/vim/fc.vim").splitlines():
    m = re.match(r"\s*syn keyword \w+ (.*)", line)
    if m and "contained" not in m.group(1).split():   # not the TODO group
        vwords.update(m.group(1).split())
check_editor("editors/vim/fc.vim", vwords)

# The spec's highlight.js grammar: every '\\b(a|b|c)\\b' alternation (written
# with doubled backslashes inside a JavaScript string) in the registered block.
m = re.search(r"hljs\.registerLanguage\('fc'.*?</script>", spec, re.S)
if not m:
    fail("spec/fc-spec.html: no hljs.registerLanguage('fc', ...) block found")
else:
    hwords = set()
    for group in re.findall(r"\\\\b\(([a-z_|0-9]+)\)\\\\b", m.group(0)):
        hwords.update(group.split("|"))
    check_editor("spec/fc-spec.html (highlight.js grammar)", hwords)

# Hover docs for built-ins: BUILTIN_DOCS entries are `{ "name", ...`.
docs = set(re.findall(r'^\s*\{ "([a-z_]+)"', read("src/builtin_docs.inc"), re.M))
for w in sorted(spec_lists.get("reserved identifiers", set()) - docs):
    fail(f"src/builtin_docs.inc: built-in '{w}' has no hover entry")
for w in sorted(docs - keywords - NOT_KEYWORDS):
    fail(f"src/builtin_docs.inc: entry '{w}' is neither a keyword nor a built-in global")

if failures:
    for f in failures:
        print(f"check-keywords: {f}")
    sys.exit(1)
