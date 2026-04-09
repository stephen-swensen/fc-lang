" Vim syntax file
" Language: FC
" Maintainer: Stephen Swensen
" Latest Revision: 2026-03-31

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

" Integer literals (with optional suffix and underscores)
" Hex
syn match fcNumber "\<0[xX][0-9a-fA-F_]\+\%(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\|[iu]\)\=\>"
" Binary
syn match fcNumber "\<0[bB][01_]\+\%(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\|[iu]\)\=\>"
" Octal
syn match fcNumber "\<0[oO][0-7_]\+\%(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\|[iu]\)\=\>"
" Decimal
syn match fcNumber "\<[0-9][0-9_]*\%(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\|[iu]\)\=\>"

" Float literals
syn match fcFloat "\<[0-9][0-9_]*\.[0-9][0-9_]*\%(f32\|f64\)\=\>"

" Character literals
syn match fcCharLit "'[^'\\]'"
syn match fcCharLit "'\\[ntr\\\\'\"0]'"
syn match fcCharLit "'\\x[0-9a-fA-F]\{2}'"

" String literals
syn region fcString start=+"+ skip=+\\\\\|\\"+ end=+"+ contains=fcEscape,fcInterp,fcPercentEsc
" C-string literals
syn region fcCString start=+c"+ skip=+\\\\\|\\"+ end=+"+ contains=fcEscape,fcInterp,fcPercentEsc

" Escape sequences within strings
syn match fcEscape contained "\\[ntr\\\\'\"0]"
syn match fcEscape contained "\\x[0-9a-fA-F]\{2}"

" String interpolation: %d{expr}, %s{expr}, %04x{expr}, etc.
syn match fcInterp contained "%[-+ 0#]*\d*\%(\.\d*\)\=[diuxXofeEgGscpT]{"
syn match fcInterp contained "}"

" Percent escape
syn match fcPercentEsc contained "%%"

" Operators
syn match fcOperator "->"
syn match fcOperator "\.\."
syn match fcOperator "::"
syn match fcOperator "&&"
syn match fcOperator "||"
syn match fcOperator "=="
syn match fcOperator "!="
syn match fcOperator "<="
syn match fcOperator ">="
syn match fcOperator "<<"
syn match fcOperator ">>"
syn match fcOperator "[+\-*/%&|^~!<>=]"

" Conditional compilation directives
syn match fcPreProc "^#if\>"
syn match fcPreProc "^#else\s\+if\>"
syn match fcPreProc "^#else\>"
syn match fcPreProc "^#end\>"

" Line comments
syn match fcComment "//.*$" contains=fcTodo

" Block comments (nestable)
syn region fcBlockComment start="/\*" end="\*/" contains=fcBlockComment,fcTodo

" TODO/FIXME/XXX/NOTE in comments
syn keyword fcTodo contained TODO FIXME XXX NOTE HACK BUG

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
