# diagnostics

The default output keeps the older line-only format for scripts that compare
stderr. Select structured output at the command line:

```sh
flint --error-format=human script.fl
flint --error-format=short script.fl
flint --error-format=json script.fl
```

Human output includes a source excerpt and underline. `short` prints one
location line. `json` writes one JSON object per diagnostic, without ANSI
codes. `--color=auto`, `--color=always`, and `--color=never` control color in
human output. Automatic color is limited to a terminal on stderr.

These formats cover the diagnostics currently emitted by the compiler and
VM. The parser reports one error before synchronization. Secondary source
labels, multiline underlines, warning control, and warning emission are not
implemented. Runtime diagnostics map to the executing source line; only
undefined-name errors narrow that span to the identifier.

Codes are stable identifiers for the current implementation, grouped by
origin: `E00xx` scanner, `E01xx` parser/compiler, `E02xx` name and scope,
`E03xx` operators and values, `E04xx` calls, `E05xx` modules, and `E06xx`
runtime. The grouping is not a compatibility promise yet. Some compiler
errors still share a general code while call sites are migrated.

Spans use zero-based half-open UTF-8 byte offsets. Reported columns count
Unicode codepoints as one column and expand tabs to four columns. They do not
calculate terminal display width for wide or combining characters.
Runtime locations use the line table attached to bytecode and underline that
source line; compile errors use token spans.

Flint's own options are recognised before *and* after the script path, so
`flint bad.fl --error-format=human` and `flint --error-format=human bad.fl`
are the same command. The short flags `-h`, `-v`, `-e` and `-` stay
positional, because those are the ones a script is likely to want for itself:
`flint t.fl -v` passes `-v` to the script rather than printing a version.
`--` ends flint's option scanning, so a script that really does want
`--error-format=...` as an argument can have it. An unrecognised value for
`--error-format=` or `--color=` is refused with the accepted values, not
ignored.

A suggestion renders its replacement on the help line, where the reader can
act on it:

```
error[E0102]: Expect ')' after arguments.
 --> bad.fl:2:1
  |
2 |
  | ^ expected here
  |
  = help: add the missing delimiter: )
```

Applicability does not appear in human output. It is metadata, and the only
two things that read it are `--fix` and tooling, both of which get it from the
JSON. A suggestion that is *not* machine-applicable says so, because that is
the case where `--fix` will leave it alone and the reader would otherwise
wonder why:

```
  = note: --fix will not apply this automatically
```

Missing closing delimiters at end of file carry structured replacement text
and applicability in JSON. `flint --fix file.fl` applies only those
machine-applicable insertions. It checks edit ranges for overlap, validates
the resulting source, and replaces a regular file through a same-directory
temporary file. It does not follow a symlink. There is no warning control.

`flint --explain E0102` prints the short explanation for a known diagnostic
code. Explanations exist only for the codes currently emitted.

### more than one error

The compiler recovers where it safely can and reports more than one problem,
because a reader who has fixed the first error and re-run deserves to know
whether that was the whole job:

```
error: could not compile due to 2 errors
```

That summary is the point. A file that is wrong from the first byte can produce
thousands of diagnostics, and thousands is not information, it is a wall, so
reporting stops after 20 and says so.

### two locations for one error

A redeclaration used to say "already a variable with this name" and stop, which
sent the reader off to find the other one. Both spans are shown now, the first
in a muted `-` so the eye can go straight between them:

```
error[E0201]: variable `x` is already defined in this scope
 --> dup.fl:3:9
  |
3 |     let x = 2
  |         ^ defined again here
  |
  |
2 |     let x = 1
  |         - first defined here
  |
  = error: could not compile due to 1 error
```

### where a diagnostic points

A missing delimiter is reported at the end of the token before it, which is
where the delimiter belongs, rather than at whatever token the parser had
reached:

```
error[E0102]: Expect ')' after arguments.
 --> bad.fl:1:10
  |
1 | print("x"
  |          ^ expected here
  |
  = help: add the missing delimiter: )
```

Runtime errors underline the expression that failed rather than the line it
sits on. Each bytecode byte records the source offset it was compiled from,
and because the end of one instruction is the start of the next, the span of
the failing expression is two array reads:

```
error[E0601]: List index 10 out of bounds (len 3).
 --> idx.fl:2:1
  |
2 | print(xs[10])
  | ^^^^^^^^^~~ invalid index
  |
```

The offset table is only consulted while an error is being reported. A
successful run does not read it, so it costs nothing on the path that matters.

Run `make diagnostic-test` for format and compatibility checks. `make test`
continues to cover the language behavior and the default output.
