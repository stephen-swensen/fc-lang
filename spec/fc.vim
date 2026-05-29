" Vim syntax file
" Language: FC
" Maintainer: Stephen Swensen
" Latest Revision: 2026-05-29

if exists("b:current_syntax")
  finish
endif

" Keywords
syn keyword fcKeyword let mut struct union module namespace import from as
syn keyword fcKeyword extern private match with if then else for in loop
syn keyword fcKeyword break continue return define

" Built-in operators (reserved identifiers)
syn keyword fcBuiltin alloc free sizeof alignof default assert some

" Boolean and none literals
syn keyword fcBoolean true false
syn keyword fcConstant none void

" Built-in type names
syn keyword fcType int8 int16 int32 int64 uint8 uint16 uint32 uint64
syn keyword fcType isize usize float32 float64 bool char str cstr any
syn keyword fcType const

" Built-in globals
syn keyword fcBuiltinGlobal stdin stdout stderr

" Type variables: 'a, 'b, 'elem, etc.
syn match fcTypeVar "'\a\w*"

" Integer literals (with optional suffix and underscores).
" Shared integer suffix: optional i/u with optional bit width.
" The three prefixed forms (hex/binary/octal) are folded into one pattern gated
" behind "\<0" so the alternation is only walked after a leading zero; decimal
" stays separate. Folding cuts per-line attempt overhead without slowing the
" common case (an alternation gated behind a rare anchor char stays cheap).
syn match fcNumber "\<0\%([xX][0-9a-fA-F_]\+\|[bB][01_]\+\|[oO][0-7_]\+\)\%([iu]\%(8\|16\|32\|64\)\=\)\=\>"
syn match fcNumber "\<[0-9][0-9_]*\%([iu]\%(8\|16\|32\|64\)\=\)\=\>"

" Float literals
syn match fcFloat "\<[0-9][0-9_]*\.[0-9][0-9_]*\%(f32\|f64\)\=\>"

" Character literals (plain char, named escape, hex escape) in one '-anchored pattern
syn match fcCharLit "'\%([^'\\]\|\\[ntr\\'\"0]\|\\x[0-9a-fA-F]\{2}\)'"

" String literals
syn region fcString start=+"+ skip=+\\\\\|\\"+ end=+"+ contains=fcEscape,fcInterp,fcPercentEsc
" C-string literals
syn region fcCString start=+c"+ skip=+\\\\\|\\"+ end=+"+ contains=fcEscape,fcInterp,fcPercentEsc

" Escape sequences within strings
syn match fcEscape contained "\\[ntr\\'\"0]"
syn match fcEscape contained "\\x[0-9a-fA-F]\{2}"

" String interpolation: %d{expr}, %s{expr}, %04x{expr}, etc.
syn match fcInterp contained "%[-+ 0#]*\d*\%(\.\d*\)\=[diuxXofeEgGscpT]{"
syn match fcInterp contained "}"

" Percent escape
syn match fcPercentEsc contained "%%"

" Operators. Every operator links to the same highlight group, so a multi-char
" operator whose characters are already covered by the single-char class (==, ->,
" !=, <=, >=, <<, >>, &&, ||) needs no pattern of its own: two adjacent same-group
" matches render identically to one. Only '..' and '::' use characters ('.' and
" ':') absent from the class, so they get tiny dedicated patterns. A long
" \|-alternation here would be costly — it gets walked at every operator char.
syn match fcOperator "[-+*/%&|^~!<>=]"
syn match fcOperator "\.\."
syn match fcOperator "::"

" Conditional compilation directives. "#else if" is intentionally not its own
" pattern: the trailing "if" is the fcKeyword (which outranks a syn-match), so
" matching just "#else" here reproduces that and keeps this anchored on "^#".
syn match fcPreProc "^#\%(if\|else\|end\)\>"

" Line comments
syn match fcComment "//.*" contains=fcTodo

" Block comments (nestable)
syn region fcBlockComment start="/\*" end="\*/" contains=fcBlockComment,fcTodo

" TODO/FIXME/XXX/NOTE in comments
syn keyword fcTodo contained TODO FIXME XXX NOTE HACK BUG

" Synchronization: bound how far back Vim scans to recompute syntax state when
" scrolling/editing instead of re-parsing toward the start of the file. Block
" comments and (rare) multi-line strings can span a few lines, so look back far
" enough to re-establish region state cheaply.
syn sync minlines=50 maxlines=200

" Highlighting links
hi def link fcKeyword       Keyword
hi def link fcBuiltin       Function
hi def link fcBoolean       Boolean
hi def link fcConstant      Constant
hi def link fcType          Type
hi def link fcBuiltinGlobal Identifier
hi def link fcTypeVar       Special
hi def link fcNumber        Number
hi def link fcFloat         Float
hi def link fcCharLit       Character
hi def link fcString        String
hi def link fcCString       String
hi def link fcEscape        SpecialChar
hi def link fcInterp        SpecialChar
hi def link fcPercentEsc    SpecialChar
hi def link fcComment       Comment
hi def link fcBlockComment  Comment
hi def link fcTodo          Todo
hi def link fcOperator      Operator
hi def link fcPreProc       PreProc

let b:current_syntax = "fc"
