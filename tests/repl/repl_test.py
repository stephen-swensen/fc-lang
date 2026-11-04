#!/usr/bin/env python3
"""Tests for `fcc --interactive`, the interactive session.

Two kinds of scenario:

- script(): a session whose stdin is not a terminal, so all of it is one
  input. The session's whole output (stdout and stderr together, in the order
  written) and its exit status must match exactly.
- Term: a session driven through a pseudo-terminal, as a person types into
  it: each input is sent when the prompt is up, and the lines the session
  shows must include the expected ones, in order.

Run by `make test-repl` (tests/repl/run_repl_tests.sh). Every scenario also
checks the session ended as expected; a timeout is a failure.
"""
import os, pty, re, select, shutil, subprocess, sys, tempfile, textwrap, time

BIN = sys.argv[1]
TMP = tempfile.mkdtemp(prefix="fc_repl_")
passes = 0
failures = []


def check(name, cond, detail=""):
    global passes
    if cond:
        passes += 1
        print("  PASS  " + name)
    else:
        failures.append(name)
        print("  FAIL  " + name)
        if detail:
            print(textwrap.indent(detail, "        "))


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w") as f:
        f.write(textwrap.dedent(text))
    return path


def script(name, text, expect, status=0, args=()):
    """Enter `text` as one input; the output must be `expect` exactly."""
    try:
        p = subprocess.run([BIN, "--interactive", *args], input=textwrap.dedent(text).encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        out, code = p.stdout.decode(errors="replace"), p.returncode
    except subprocess.TimeoutExpired:
        out, code = "<timed out>", None
    want = textwrap.dedent(expect)
    check(name, out == want and code == status,
          "expected (exit %s):\n%s\ngot (exit %s):\n%s" % (status, want, code, out))


class Term:
    """A session in a pseudo-terminal."""

    PROMPT = b"\x1b[?2004h"     # the editor turns bracketed paste on at each prompt

    def __init__(self, args=()):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(BIN, [BIN, "--interactive", *args])
        self.out = b""
        self.prompts = 0
        self.status = None

    def _read(self, until, timeout=30):
        end = time.time() + timeout
        while not until() and time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return False
            if not data:
                return False
            self.out += data
        return until()

    def prompt(self):
        """Wait for the next prompt."""
        self.prompts += 1
        n = self.prompts
        return self._read(lambda: self.out.count(self.PROMPT) >= n)

    def send(self, keys):
        """Wait for the prompt, then type `keys` (\\x03 is Ctrl+C, \\x04 Ctrl+D)."""
        self.prompt()
        os.write(self.fd, keys.encode())

    def paste(self, text):
        self.prompt()
        os.write(self.fd, b"\x1b[200~" + text.encode() + b"\x1b[201~")

    def interrupt_after(self, seconds):
        """Press Ctrl+C while a statement runs."""
        time.sleep(seconds)
        os.write(self.fd, b"\x03")

    def finish(self):
        """Read to the end of the session; its exit status. A session still
        running after that is killed, with its holder (one process group),
        and has status None."""
        self._read(lambda: False, timeout=10)
        for _ in range(50):
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = os.waitstatus_to_exitcode(st)
                break
            time.sleep(0.1)
        else:
            os.killpg(self.pid, 9)
            os.waitpid(self.pid, 0)
            self.status = None
        os.close(self.fd)
        return self.status

    def lines(self):
        """The lines as the terminal shows them: escapes dropped, and a line
        rewritten after a carriage return keeps its last version."""
        text = self.out.decode(errors="replace")
        text = re.sub(r"\x1b\[[0-9?;]*[A-Za-z~]", "", text)
        return [l.split("\r")[-1] if not l.endswith("\r") else l.rstrip("\r").split("\r")[-1]
                for l in text.split("\n")]

    def expect(self, name, wanted, status=0):
        """The session showed the `wanted` lines, in order, and ended with
        `status`."""
        shown = [l.rstrip() for l in self.lines()]
        pos, in_order = 0, True
        for w in wanted:
            try:
                pos = shown.index(w, pos) + 1
            except ValueError:
                in_order = False
                break
        check(name, in_order and self.status == status,
              "wanted, in order (exit %s):\n%s\nshown (exit %s):\n%s"
              % (status, "\n".join(wanted), self.status, "\n".join(shown)))


# ---------------------------------------------------------------------------
# Values, functions and statements
# ---------------------------------------------------------------------------

script("values, functions and statements", """\
    let x = 1 + 2
    let f = (n: i32) -> n * x
    f(5)
    it + 1
    let b = true
    let fl = 2.0
    let third = 1.0f32 / 3.0f32
    let big = 18446744073709551615u64
    let neg = -5i64
    let s = "a\\tb\\"c"
    let cs = c"hey"
    """, """\
    val x: i32 = 3
    val f: (i32) -> i32
    val it: i32 = 15
    val it: i32 = 16
    val b: bool = true
    val fl: f64 = 2.0
    val third: f32 = 0.33333334
    val big: u64 = 18446744073709551615
    val neg: i64 = -5
    val s: const str = "a\\tb\\"c"
    val cs: const cstr = c"hey"
    """)

script("a statement that yields no value binds nothing", """\
    5
    let mut c = 0
    c = c + 1
    it
    c
    """, """\
    val it: i32 = 5
    val c: i32 = 0
    val it: i32 = 5
    val it: i32 = 1
    """)

script("a name declared again shadows; what used the old one keeps it", """\
    let x = 10
    let getx = () -> x
    let x = 20
    getx()
    x
    let x = x + 1
    let sq = (n: i32) -> n * n
    let use_sq = () -> sq(3)
    let sq = (n: i32) -> n + 1000
    use_sq()
    sq(3)
    """, """\
    val x: i32 = 10
    val getx: () -> i32
    val x: i32 = 20
    val it: i32 = 10
    val it: i32 = 20
    val x: i32 = 21
    val sq: (i32) -> i32
    val use_sq: () -> i32
    val sq: (i32) -> i32
    val it: i32 = 9
    val it: i32 = 1003
    """)

script("a function sees a let mut's assignments", """\
    let mut count = 0
    let bump = () -> count = count + 1
    bump()
    bump()
    count
    let fact = (n: i64) -> if n <= 1 then 1i64 else n * fact(n - 1)
    fact(20)
    """, """\
    val count: i32 = 0
    val bump: () -> void
    val it: i32 = 2
    val fact: (i64) -> i64
    val it: i64 = 2432902008176640000
    """)

script("statements: loops, match, blocks, imports", """\
    import io from std::
    for i in 0..3 do io.write("i=%d{i}\\n", stdout)
    match some(3) with
    | some(v) -> v * 2
    | none -> 0
    let total = (v: i32[]) ->
        let mut s = 0
        for x in v do s = s + x
        s
    total(i32[] { 1, 2, 3 })
    io.write("one\\n", stdout); io.write("two\\n", stdout)
    """, """\
    i=0
    i=1
    i=2
    val it: i32 = 6
    val total: (i32[]) -> i32
    val it: i32 = 6
    one
    val it: i64 = 4
    two
    val it: i64 = 4
    """)

script("a destructuring let binds each of its names", """\
    let { q, r } = { 17 / 5, 17 % 5 }
    q * 5 + r
    struct point =
        x: i32
        y: i32
    let { x = px } = point { x = 3, y = 4 }
    let { x = a, y = b } = point { x = 5, y = 6 }
    let { { m, n }, _, o } = { { 1, 2 }, 3, 4 }
    let { _, _ } = { 1, 2 }
    let { q, r } = { r, q }
    let mut { s, t } = { 1, 2 }
    s = 10
    s + t
    """, """\
    val q: i32 = 3
    val r: i32 = 2
    val it: i32 = 17
    struct point
    val px: i32 = 3
    val a: i32 = 5
    val b: i32 = 6
    val m: i32 = 1
    val n: i32 = 2
    val o: i32 = 4
    val q: i32 = 2
    val r: i32 = 3
    val s: i32 = 1
    val t: i32 = 2
    val it: i32 = 12
    """)

script("destructured names are typed as in a block", """\
    struct holder =
        xs: i32[]
        n: i32
    let buf = i32[] { 1, 2 }
    let r = (const holder[]) holder[] { holder { xs = buf, n = 7 } }
    let { xs = t, n = k } = r[0]
    let { f, g } = { (v: i32) -> v + 1, (v: i32) -> v * 2 }
    f(g(3))
    """, """\
    struct holder
    val buf: i32[] = { 1, 2 }
    val r: const holder[] = { holder { xs = { 1, 2 }, n = 7 } }
    val t: const i32[] = { 1, 2 }
    val k: i32 = 7
    val f: (i32) -> i32
    val g: (i32) -> i32
    val it: i32 = 7
    """)

# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

script("structs, unions, enums, tuples, options, results", """\
    struct point =
        x: i32
        y: i32
    let p = point { x = 1, y = 2 }
    union shape =
        | circle(i32)
        | rect(point)
        | empty
    let c = shape.circle(7)
    let r = shape.rect(p)
    let e = shape.empty
    enum color =
        | red
        | green = 5
    let g = color.green
    let t = { 1u8, -2i8, "three" }
    let o = some(p)
    let n = none(i32)
    let np = none(i32*)
    let okv = ok(5)
    let errv = err(i32, 7)
    """, """\
    struct point
    val p: point = point { x = 1, y = 2 }
    union shape
    val c: shape = shape.circle(7)
    val r: shape = shape.rect(point { x = 1, y = 2 })
    val e: shape = shape.empty
    enum color
    val g: color = color.green
    val t: {u8, i8, const str} = { 1, -2, "three" }
    val o: point? = some(point { x = 1, y = 2 })
    val n: i32? = none
    val np: i32*? = none
    val okv: i32! = ok(5)
    val errv: i32! = err(7)
    """)

script("slices and strings", """\
    let xs = i32[] { 4, 5, 6 }
    xs[0] = 9
    xs
    let words = (const str)[] { "a", "bc" }
    struct item =
        name: const str
        tags: (const str)[]
    item { name = "x", tags = words }
    let zeros = i32[] { len = 30 }
    let heap = alloc(i32[] { len = 2 })!
    heap[1] = 7
    heap
    """, """\
    val xs: i32[] = { 4, 5, 6 }
    val it: i32[] = { 9, 5, 6 }
    val words: (const str)[] = { "a", "bc" }
    struct item
    val it: item = item { name = "x", tags = { "a", "bc" } }
    val zeros: i32[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ... (30 in all) }
    val heap: i32[] = { 0, 0 }
    val it: i32[] = { 0, 7 }
    """)

script("what a statement builds on its stack lives on", """\
    let n = 42
    let msg = "n is %d{n}"
    let xs = i32[] { 1, 2, 3 }
    let first = () -> xs[0]
    msg
    first()
    """, """\
    val n: i32 = 42
    val msg: str = "n is 42"
    val xs: i32[] = { 1, 2, 3 }
    val first: () -> i32
    val it: str = "n is 42"
    val it: i32 = 1
    """)

script("a type declared again: old values keep the old layout", """\
    struct pt =
        x: i32
        y: i32
    let a = pt { x = 1, y = 2 }
    struct pt =
        name: const str
    let b = pt { name = "new" }
    a
    a.y
    b
    """, """\
    struct pt
    val a: pt = pt { x = 1, y = 2 }
    struct pt
    val b: pt = pt { name = "new" }
    val it: pt = pt { x = 1, y = 2 }
    val it: i32 = 2
    val it: pt = pt { name = "new" }
    """)

script("generics: an instance made by one input is used by later ones", """\
    let id = (v: 'a) -> v
    let f1 = () -> id(41)
    let f2 = () -> id(1) + f1()
    f2()
    struct box =
        v: 'a
    let bx = box { v = 5 }
    let get = (b: box<'a>) -> b.v
    get(bx)
    get(box { v = "s" })
    """, """\
    val id: ('a) -> 'a
    val f1: () -> i32
    val f2: () -> i32
    val it: i32 = 42
    struct box
    val bx: box<i32> = box<i32> { v = 5 }
    val get: (box<'a>) -> 'a
    val it: i32 = 5
    val it: const str = "s"
    """)

script("modules: members in any order, using session values", """\
    let base = 100
    module m =
        let even = (n: i32) -> if n == 0 then true else odd(n - 1)
        let odd = (n: i32) -> if n == 0 then false else even(n - 1)
        let plus = (n: i32) -> n + base
    m.even(10)
    m.plus(1)
    """, """\
    val base: i32 = 100
    module m
    val it: bool = true
    val it: i32 = 101
    """)

script("error groups are numbered as they are entered", """\
    error file_err =
        | not_found
        | denied
    error net_err =
        | refused
    error_name(net_err.refused)
    error_name(file_err.denied)
    net_err.refused - file_err.not_found
    """, """\
    error file_err
    error net_err
    val it: const str? = some("net_err.refused")
    val it: const str? = some("file_err.denied")
    val it: error = 1
    """)

script("C externs", """\
    module c_stdlib from "stdlib.h" =
        extern abs: (i32) -> i32
        extern EXIT_FAILURE: i32
    c_stdlib.abs(-5) + c_stdlib.EXIT_FAILURE
    module c_math from "math.h" =
        extern sqrt: (f64) -> f64
    c_math.sqrt(2.0)
    """, """\
    module c_stdlib
    val it: i32 = 6
    module c_math
    val it: f64 = 1.4142135623730951
    """)

script("a value that points at no memory is shown as unreadable", """\
    let s = u8[] { ptr = (u8*) 16usize, len = 3 }
    1
    """, """\
    val s: str = <unreadable: it points at memory the program does not have>
    val it: i32 = 1
    """)

# ---------------------------------------------------------------------------
# Errors and the end of the session
# ---------------------------------------------------------------------------

script("an input that does not compile runs none of its statements", """\
    let a = 1
    let b = undefined_thing + 1
    """, """\
    stdin:2:9: error: undefined name 'undefined_thing'
    """, status=1)

script("a statement that aborts ends a script with the abort's status", """\
    let xs = i32[] { 1, 2 }
    xs[5]
    let b = 2
    """, """\
    val xs: i32[] = { 1, 2 }
    stdin:2: slice index out of range: index=5 len=2
    """, status=134)

script("return ends the session with its exit code", """\
    let a = 1
    return a + 6
    let b = 2
    """, """\
    val a: i32 = 1
    """, status=7)

script("return inside a statement ends the session", """\
    if true then return 4
    5
    """, "", status=4)

script("return inside a function returns from the function", """\
    let g = (n: i32) -> if n > 3 then return 1 else n
    g(5)
    """, """\
    val g: (i32) -> i32
    val it: i32 = 1
    """)

script("the program's exit ends the session", """\
    import sys from std::
    sys.exit(3)
    """, "", status=3)

script("a return without an exit code", "return\n", """\
    stdin:1:1: error: a return at the prompt ends the session and needs its exit code: return 0
    """, status=1)

script("a return whose exit code is not an i32", "return 3u8\n", """\
    stdin:1:8: error: a return at the prompt ends the session with an i32 exit code, not u8
    """, status=1)

script("defer is not allowed at the prompt", "defer 1\n", """\
    stdin:1:1: error: defer is not allowed at the prompt: the session has no end for it to run at
    """, status=1)

script("a namespace is not declared at the prompt", "namespace acme\n", """\
    stdin:1:1: error: a namespace declaration is not allowed at the prompt; load a file that declares one
    """, status=1)

script("a let with in is a statement", """\
    let x = 1 in x + 1
    let mut y = 2 in y = y + 1; y
    let f = (n: i32) -> n * 2 in f(21)
    """, """\
    val it: i32 = 2
    val it: i32 = 3
    val it: i32 = 42
    """)

script("a let with in binds its name in the body only", "let x = 1 in x\nx\n", """\
    stdin:2:1: error: undefined name 'x'
    """, status=1)

script("a pattern that does not fit is reported once, not at each use", """\
    let f = (ts: {i32, i32}[], t: {i32, i32}) ->
        let mut s = 0
        for { c, d, e } in ts do s = c + e
        match t with
        | { a, b, c } -> a + c + s
    """, """\
    stdin:3:5: error: tuple {i32, i32} has 2 elements but the pattern binds 3
    stdin:5:7: error: tuple {i32, i32} has 2 elements but the pattern matches 3
    """, status=1)

script("a destructuring let that does not fit is reported once", "let { x, y, z } = { 1, 2 }\n", """\
    stdin:1:1: error: tuple {i32, i32} has 2 elements but the pattern binds 3
    """, status=1)

script("a destructuring let with in binds its names in the body only", """\
    let { u, v } = { 1, 2 } in u + v
    u
    """, """\
    stdin:2:1: error: undefined name 'u'
    """, status=1)

script("a destructuring let that does not check binds nothing", "let { a, a } = { 1, 2 }\na\n", """\
    stdin:1:10: error: duplicate binding 'a' in this pattern; a name may be bound only once
    """, status=1)

script("what a destructuring let holds cannot be named", "destructured__value\n", """\
    stdin:1:1: error: identifier 'destructured__value' contains '__' (double underscore), which is reserved
    """, status=1)

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

PROG = write("prog.fc", """\
    import io from std::

    let mut counter = 100
    let table = alloc(i32[] { len = 3 })!

    module geo =
        struct vec =
            x: f64
            y: f64
        let len2 = (v: vec) -> v.x * v.x + v.y * v.y

    let main = (args: str[]) ->
        io.write("main ran\\n", stdout)
        counter = counter + 1
        0
    """)
SETUP = write("setup.fcx", """\
    let v = geo.vec { x = 3.0, y = 4.0 }
    geo.len2(v)
    """)

script("a loaded file's initializers run, and its main does not", """\
    counter
    table
    main(str[] {})
    counter
    """, """\
    val it: i32 = 100
    val it: i32[] = { 0, 0, 0 }
    main ran
    val it: i32 = 0
    val it: i32 = 101
    """, args=(PROG,))

script(".fcx inputs are entered before stdin", "v.x\n", """\
    val v: geo.vec = geo.vec { x = 3.0, y = 4.0 }
    val it: f64 = 25.0
    val it: f64 = 3.0
    """, args=(PROG, SETUP))

BAD = write("bad.fc", "let main = (args: str[]) -> nope\n")
script("a loaded file that does not compile ends the session", "1\n", """\
    %s:1:29: error: undefined name 'nope'
    """ % BAD, status=1, args=(BAD,))

# ---------------------------------------------------------------------------
# At a terminal
# ---------------------------------------------------------------------------

t = Term()
t.send("let mut c = 1\n")
t.send("let xs = i32[] { 1, 2 }\n")
t.send("c = 5; xs[9]; c = 7\n")
t.send("c\n")
t.send("\x04")
t.finish()
t.expect("a statement that aborts is undone; the session goes on", [
    "val c: i32 = 1",
    "val xs: i32[] = { 1, 2 }",
    "stdin:1: slice index out of range: index=9 len=2",
    "(1 later statement of this input did not run.)",
    "val it: i32 = 5",
])

t = Term()
t.send("let xs = i32[] { 1 }\n")
t.send("let a = 0\n")
t.send("let { a, b } = { xs[0], xs[5] }; a\n")
t.send("a\n")
t.send("\x04")
t.finish()
t.expect("a destructuring let that aborts binds none of its names", [
    "val xs: i32[] = { 1 }",
    "val a: i32 = 0",
    "stdin:1: slice index out of range: index=5 len=1",
    "(1 later statement of this input did not run.)",
    "val it: i32 = 0",
])

t = Term()
t.send("let mut x = 5\n")
t.send("let { p, n } = { &x, 1 }\n")
t.send("p[0] = 9; x\n")
t.send("\x04")
t.finish()
t.expect("a destructured pointer points where it was taken from", [
    "val x: i32 = 5",
    "val n: i32 = 1",
    "val it: i32 = 9",
])

t = Term()
t.send("let mut i = 0\n")
t.send("loop i = i + 1\n")
t.interrupt_after(1.0)
t.send("i\n")
t.send("\x04")
t.finish()
t.expect("Ctrl+C interrupts a statement, which is undone", [
    "val i: i32 = 0",
    "Interrupted.",
    "val it: i32 = 0",
])

t = Term()
t.send("let f = (n: i32) ->\n")
t.send("    n * 2\n")
t.send("\n")
t.send("f(4)\n")
t.send("struct p =\n")
t.send("    x: i32\n")
t.send("\x03")
t.send("p\n")
t.send("\x04")
t.finish()
t.expect("a line that opens a block reads to a blank line; Ctrl+C discards it", [
    "val f: (i32) -> i32",
    "val it: i32 = 8",
    "stdin:1:1: error: undefined name 'p'",
])

t = Term()
t.paste("let a = 1\n\nlet b = a + 1\n")
t.send("b\n")
t.send("\x04")
t.finish()
t.expect("a paste is one input, blank lines and all", [
    "val a: i32 = 1",
    "val b: i32 = 2",
    "val it: i32 = 2",
])

t = Term()
t.send("let a = 1 \\\n")
t.send("let b = a + 1\n")
t.send("let c = 5 \\\n")
t.send("nope\n")
t.send("c\n")
t.send("\x04")
t.finish()
t.expect("a line ending with \\ goes on to the next, in the same input", [
    "val a: i32 = 1",
    "val b: i32 = 2",
    "stdin:2:1: error: undefined name 'nope'",
    "stdin:1:1: error: undefined name 'c'",
])

t = Term()
t.send("let a = 1;\n")
t.send("let b = a + 1\n")
t.send("let c = 5;\n")
t.send("nope\n")
t.send("c\n")
t.send("\x04")
t.finish()
t.expect("a line ending with ; goes on to the next, in the same input", [
    "val a: i32 = 1",
    "val b: i32 = 2",
    "stdin:2:1: error: undefined name 'nope'",
    "stdin:1:1: error: undefined name 'c'",
])

t = Term()
t.send("let a = 1\x1b\r")
t.send("let b = a + 1\r")
t.send("let c = 5\x1b\r")
t.send("\x1b\r")
t.send("nope\r")
t.send("c\r")
t.send("\x04")
t.finish()
t.expect("Alt+Enter goes on to the next line, a blank one too", [
    "val a: i32 = 1",
    "val b: i32 = 2",
    "stdin:3:1: error: undefined name 'nope'",
    "stdin:1:1: error: undefined name 'c'",
])

t = Term()
t.send("module m =\n")
t.send("    let f = () -> 1\n")
t.send("\\\n")
t.send("    let g = () -> f() + 1\n")
t.send("\n")
t.send("m.g()\n")
t.send('let s = "x\\\\"\n')
t.send("1 + 1 // not continued \\\n")
t.send("\x04")
t.finish()
t.expect("in a block a line of \\ alone is a blank line; a \\ in a literal or comment is not one", [
    "module m",
    "val it: i32 = 2",
    'val s: const str = "x\\\\"',
    "val it: i32 = 2",
])

t = Term()
t.send('let s = "unterminated\n')
t.send("1 + 1\n")
t.send("\x04")
t.finish()
t.expect("an input the lexer rejects does not end the session", [
    "val it: i32 = 2",
])

t = Term((PROG, SETUP))
t.send("v.y\n")
t.send("return 9\n")
t.finish()
t.expect("an .fcx input is shown as typed; return ends the session", [
    "> let v = geo.vec { x = 3.0, y = 4.0 }",
    ". geo.len2(v)",
    "val v: geo.vec = geo.vec { x = 3.0, y = 4.0 }",
    "val it: f64 = 25.0",
    "val it: f64 = 4.0",
], status=9)

t = Term()
t.send("\x04")
t.finish()
t.expect("Ctrl+D on an empty line ends the session", [], status=0)

shutil.rmtree(TMP, ignore_errors=True)
print()
if failures:
    print("%d REPL test(s) failed:" % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("all %d REPL tests passed" % passes)
