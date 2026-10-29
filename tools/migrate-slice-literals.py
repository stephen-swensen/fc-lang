#!/usr/bin/env python3
"""Rewrite slice literals from the length-in-brackets form to the T[] form.

    T[N] { }            ->  T[] { len = N }       (zero-filled)
    T[0] { }            ->  T[] { }               (empty)
    T[3] { a, b, c }    ->  T[] { a, b, c }       (length counted)
    T[k] { a, b, c }    ->  T[] { len = k, a, b, c }
    const T[N] { ... }  ->  (const T)[] { ... }
    const T[] { ptr = p, len = n }  ->  (const T)[] { ptr = p, len = n }

A raw-parts literal whose ptr is read-only (`str { ptr = c"hi", len = 2 }`)
now needs a read-only head (`const str { ... }`); the compiler reports each
one.

The length stays as a `len = N` assertion when it is not an integer literal
(an enum count, a named constant, a const parameter), when the literal spans
several lines (a table whose entries can't be counted at a glance), or when
it doesn't match the element count (so a literal that failed to compile
still fails).

Strings, character literals and comments are left alone; check comments by
hand afterwards. Markdown and HTML files are rewritten only inside ```fc
fences and inline `code` spans.

usage: tools/migrate-slice-literals.py FILE...   (rewrites in place, prints
the files it changed)
"""
import re
import sys

IDENT = re.compile(r"[A-Za-z0-9_]")


def skip_string(s, i):
    """i is at an opening '"'; return the index just past the closing one."""
    i += 1
    while i < len(s) and s[i] != '"':
        i += 2 if s[i] == "\\" else 1
    return i + 1


def char_lit_end(s, i):
    """i is at a "'"; return the end of a character literal there, or -1 for
    a type variable ('a)."""
    m = re.match(r"'(\\x[0-9A-Fa-f]{2}|\\.|[^\\'\n])'", s[i:])
    return i + m.end() if m else -1


def code_mask(s):
    """A list of booleans: True where s[i] is code (not in a string, char
    literal or comment)."""
    mask = [True] * len(s)
    i = 0
    while i < len(s):
        c = s[i]
        if s.startswith("//", i):
            j = s.find("\n", i)
            j = len(s) if j < 0 else j
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = len(s) if j < 0 else j + 2
        elif c == '"':
            j = skip_string(s, i)
        elif c == "'":
            j = char_lit_end(s, i)
            if j < 0:
                i += 1
                continue
        else:
            i += 1
            continue
        for k in range(i, min(j, len(s))):
            mask[k] = False
        i = j
    return mask


def match_close(s, mask, i, open_c, close_c):
    """s[i] is open_c; return the index of its matching close_c, or -1."""
    depth = 0
    for j in range(i, len(s)):
        if not mask[j]:
            continue
        if s[j] == open_c:
            depth += 1
        elif s[j] == close_c:
            depth -= 1
            if depth == 0:
                return j
    return -1


def match_open(s, mask, i, open_c, close_c):
    """s[i] is close_c; return the index of its matching open_c, or -1."""
    depth = 0
    for j in range(i, -1, -1):
        if not mask[j]:
            continue
        if s[j] == close_c:
            depth += 1
        elif s[j] == open_c:
            depth -= 1
            if depth == 0:
                return j
    return -1


def count_elems(s, mask, lo, hi):
    """Top-level comma-separated items in s[lo:hi]."""
    depth, items, cur = 0, 0, False
    for j in range(lo, hi):
        c = s[j]
        if mask[j] and c in "([{":
            depth += 1
        elif mask[j] and c in ")]}":
            depth -= 1
        if mask[j] and depth == 0 and c == ",":
            if cur:
                items += 1
            cur = False
        elif not c.isspace():
            cur = True
    return items + (1 if cur else 0)


def head_start(s, mask, i):
    """s[i-1] ends a type head that s[i] ('[') follows. Return the index where
    a plain head (name, dotted path or type variable, generic arguments,
    ?/*/! suffixes) starts, or -1 for a head that is not plain (a group, a
    tuple type, a nested slice)."""
    j = i - 1
    while j >= 0 and s[j] in "?*!":
        j -= 1
    if j >= 0 and s[j] == ">":
        j = match_open(s, mask, j, "<", ">") - 1
        if j < 0:
            return -1
    if j < 0 or not IDENT.match(s[j]):
        return -1
    while j >= 0 and (IDENT.match(s[j]) or s[j] == "."):
        j -= 1
    if j >= 0 and s[j] == "'":
        j -= 1
    return j + 1


def rewrite(s):
    mask = code_mask(s)
    edits = []   # (start, end, replacement), applied back to front
    i = 0
    while i < len(s):
        if s[i] != "[" or not mask[i] or i == 0:
            i += 1
            continue
        prev = s[i - 1]
        if not (IDENT.match(prev) or prev in ")?*!>}]"):
            i += 1
            continue
        close = match_close(s, mask, i, "[", "]")
        if close < 0:
            i += 1
            continue
        k = close + 1
        while k < len(s) and s[k] in " \t":
            k += 1
        size = s[i + 1:close].strip()
        if k >= len(s) or s[k] != "{":
            i += 1
            continue
        brace_close = match_close(s, mask, k, "{", "}")
        if brace_close < 0:
            i += 1
            continue
        body = s[k + 1:brace_close]
        if not size:
            pass   # T[] { ptr = .., len = .. }: only a const head changes
        elif not body.strip():
            new_body = " }" if size == "0" else " len = %s }" % size
            edits.append((i, brace_close + 1, "[] {" + new_body))
        else:
            n = count_elems(s, mask, k + 1, brace_close)
            literal = re.fullmatch(r"[0-9][0-9_]*", size) is not None
            one_line = "\n" not in body
            if literal and one_line and int(size.replace("_", "")) == n:
                edits.append((i, close + 1, "[]"))
            else:
                edits.append((i, k + 1, "[] { len = %s," % size))
        # const T[...] {  ->  (const T)[] {
        h = head_start(s, mask, i) if prev not in ")}]" else -1
        if h >= 0:
            m = re.search(r"((?:\bconst\s+)+)$", s[:h])
            if m and mask[m.start()]:
                edits.append((m.start(), m.start(), "("))
                edits.append((m.start(), h, "const "))
                edits.append((i, i, ")"))
        i = k + 1
    out = s
    for start, end, rep in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        out = out[:start] + rep + out[end:]
    return out


def rewrite_markdown(s):
    def fence(m):
        return m.group(1) + rewrite(m.group(2)) + m.group(3)
    s = re.sub(r"(```fc[^\n]*\n)(.*?)(\n```)", fence, s, flags=re.S)
    parts = re.split(r"(```.*?```)", s, flags=re.S)
    for idx in range(0, len(parts), 2):
        parts[idx] = re.sub(r"`([^`\n]+)`",
                            lambda m: "`" + rewrite(m.group(1)) + "`", parts[idx])
    return "".join(parts)


def main():
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as f:
            src = f.read()
        if path.endswith((".md", ".html")):
            out = rewrite_markdown(src)
        else:
            out = rewrite(src)
        if out != src:
            with open(path, "w", encoding="utf-8") as f:
                f.write(out)
            print(path)


if __name__ == "__main__":
    main()
