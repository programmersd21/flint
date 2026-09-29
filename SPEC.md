# flint language spec

version 0.2. this is the grammar and the semantics. the implementation is not
always right; when they disagree, file a bug.

## lexical

### keywords

```
and      as       break    const    continue else
export   false    fn       for      if       import
in       let      nil      not      or       print
return   true     while
```

### literals

- numbers: decimal and exponent notation. `123`, `3.14159`, `1e-5`
- strings: double-quoted bytes, escapes `\n \t \r \" \\ \0`. utf-8 passes
  through untouched
- booleans: `true`, `false`
- nil: `nil`

### operators

```
(   )   {   }   [   ]
+   -   *   /   %   !
=   ==  !=  <   <=  >   >=
+=  -=  *=  /=
..  :   .   ,   ;
```

`#` starts a comment that runs to the end of the line.

## grammar

### statements

statements end at a `;` or a newline.

```
program        = declaration* EOF ;
declaration    = fnDecl | letDecl | constDecl | importDecl | exportDecl | statement ;

letDecl        = "let" IDENTIFIER ( "=" expression )? terminator ;
constDecl      = "const" IDENTIFIER "=" expression terminator ;
fnDecl         = "fn" IDENTIFIER "(" parameters? ")" "{" block "}" ;
importDecl     = "import" STRING terminator ;
exportDecl     = "export" ( fnDecl | letDecl | constDecl ) ;

statement      = exprStmt | printStmt | ifStmt | whileStmt | forStmt
               | breakStmt | continueStmt | returnStmt | "{" block "}" ;

printStmt      = "print" "(" arguments ")" terminator ;
ifStmt         = "if" expression block ( "else" ( ifStmt | block ) )? ;
whileStmt      = "while" expression block ;
forStmt        = "for" IDENTIFIER "in" expression block ;
returnStmt     = "return" expression? terminator ;
breakStmt      = "break" terminator ;
continueStmt   = "continue" terminator ;

terminator     = ";" | newline | "}" | EOF ;
```

`for x in expr` iterates a list, or a range when `expr` contains `..`.
ranges are half-open: `1..5` is 1, 2, 3, 4.

### expression precedence

loosest to tightest:

1. assignment: `=`, `+=`, `-=`, `*=`, `/=`
2. `or`
3. `and`
4. equality: `==`, `!=`
5. comparison: `<`, `<=`, `>`, `>=`
6. range: `..`
7. term: `+`, `-`
8. factor: `*`, `/`, `%`
9. type assertion: `as`
10. unary: `!`, `not`, `-`
11. call, subscript, field: `()`, `[]`, `.`
12. primary: literals, identifiers, grouping, list, table

`<=` is not `!(>)`. `NaN` is unordered, so the two forms differ on NaN and the
compiler emits a separate opcode for each.

### type assertions

`expr as T` is an infix operator. `T` is one of the seven names `type()`
returns: `number`, `string`, `bool`, `nil`, `list`, `table`, `function`. any
other name is a compile error.

`as` asserts; it does not convert. the value is unchanged on success. on
failure the program stops with a runtime error naming both types.

the assertion runs at run time, not at compile time, because the value may come
from a module that has not been read yet or from a function parameter.

## semantics

### truthiness

only `nil` and `false` are falsy. `0`, `0.0`, `NaN`, `""`, `[]` and `{}` are
all truthy.

### equality

- `nil == nil` is true
- numbers compare by IEEE 754 value, so `NaN == NaN` is false
- strings compare by interned pointer identity
- lists and tables compare by identity

### variables

- top-level names go in the global table
- block-scoped names are stack slots
- `const` rejects assignment. a local is caught when the function is
  compiled; a global is caught when the assignment runs, because the const
  may have come from an imported module that had not been read yet when this
  file was compiled
- `let` on a name that is already const is an error. an ordinary global can
  be redeclared, so this has to be checked separately, and silently
  overwriting would leave the name read-only anyway
- redeclaring a const with a different value is an error. re-running the
  same `const` statement with the same value is not, because importing a
  module executes its top level, and failing on the second import of any
  module that exports a const would be absurd
- promoting `let` to `const` is allowed; narrowing is not a contradiction
- a closure captures enclosing locals by reference. a write inside the closure
  is visible outside it
- a local cannot be read in its own initializer: `let a = a` is an error
- a name cannot be declared twice in the same scope

### strings

`+` concatenates when both operands are strings, adds when both are numbers,
and is an error otherwise. there is no implicit conversion.

### indexing

`list[i]` accepts negative indices, counting from the end. out of range is a
runtime error. `str[i]` yields a one-character string.

the index must be a finite whole number. a fractional index is not truncated:
`xs[1.5]` is an error rather than a silent read of element 1. a value beyond
the range of int, an infinity, a NaN, or anything that is not a number is
rejected with its own message before any conversion happens, because a
floating-to-integer conversion of any of those is undefined.

### string concatenation

`a + b` on strings whose combined length would overflow is a runtime error,
not undefined behaviour. doubling a string each iteration reaches the limit
in a dozen passes, so the check is not theoretical.

### tables

`{ key: value }` is a table literal. `t.key` reads it, `t.key = v` writes it.
reading a key that was never set gives `nil`. there is no delete syntax.

a table literal works in any expression position. it keeps itself on the
stack between pairs rather than in a hidden local, which is what lets
`type({a: 1})` work: a hidden local collides with the callee already on the
stack in a call argument.

### input

`input()` reads one line from stdin and returns it without the newline.
`input("prompt")` writes the prompt first with no newline added. the prompt
must be a string; more than one argument is an error.

an empty line returns `""`. end of file returns `nil`, so a script tells
"the user pressed enter" from "there is nothing left to read". a final line
with no trailing newline is still a line, not nothing.

CRLF is tolerated: a `\r` is skipped, so input from a windows terminal
arrives as plain text. a line can be any length; the buffer grows.

a non-string prompt and a non-zero-or-one argument count are runtime errors,
reported the ordinary way.
