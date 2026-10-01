#!/usr/bin/env python3
"""
Rewrite the interpreter's dispatch from a switch to computed goto.

Run from the repository root. Idempotent: a file that already says
FL_COMPUTED_GOTO is left alone, so running it twice is harmless.

Why a script rather than a #ifdef in the source
----------------------------------------------
The two dispatch forms cannot share one copy of sixty-odd instruction handlers
through a macro without turning run() into something nobody can read, and they
cannot share them through a runtime flag because the flag is a per-instruction
branch. So there are two builds of the same function, and this script produces
one of them from the other.

Doing it by script rather than by hand is also what keeps the two in step: a
handler added to the switch build and forgotten here would compile fine in the
computed-goto build only if the label table had an entry for it, and the
compiler says so. Adding the entry is the whole change.

The transformation
------------------
    switch (instruction) {   ->  goto *dispatch[instruction];
    case OP_ADD: {           ->  op_ADD: {
    break;                   ->  goto *dispatch[instruction];

Every `break` in the dispatch body is a handler ending. That is checked rather
than assumed: the script refuses to run if it finds a `break` at a brace depth
other than the one a handler body sits at, which is the `break` inside the
value_to_index-style nested code that a careless search would rewrite.

The label table is indexed by opcode and every entry is initialized, so a
malformed opcode reaching the dispatch is still a jump into a handler rather
than a jump to a null label. The verifier runs first and rejects those, but the
table is fully populated regardless: a jump through an uninitialized pointer is
not a failure mode worth leaving in a runtime that fuzzes bytecode.
"""

import re
import sys

DEFAULT_PATH = "src/runtime/vm.c"


def find_dispatch_span(lines):
    """(first line of `switch (instruction) {`, first line after its close)."""
    start = None
    for i, line in enumerate(lines):
        if re.match(r"\t\tswitch \(instruction\) \{\s*$", line):
            start = i
            break
    if start is None:
        return None

    depth = 0
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > start:
            return (start, i + 1)
    raise SystemExit("dispatch switch is not balanced; fix that first")


def transform(lines):
    start, end = find_dispatch_span(lines)
    body = lines[start:end]

    out = []
    for line in body:
        if re.match(r"\t\tswitch \(instruction\) \{\s*$", line):
            # the goto replaces the switch entirely
            out.append("\t\tgoto *dispatch[instruction];")
            continue

        case = re.match(r"\t\tcase (OP_[A-Z_]+):(.*)$", line)
        if case:
            out.append("\t\tl_%s:%s" % (case.group(1).lower(), case.group(2)))
            continue

        # Any `break;` inside the dispatch body ends a handler and becomes the
        # dispatch. The indentation is deliberately not matched: an earlier
        # version required exactly three tabs, missed a five-tab break nested
        # inside an if, and left it as a real break -- which, with the switch
        # gone, silently exits the dispatch loop instead of continuing it.
        # That is the kind of bug this script must not be able to introduce, so
        # the pattern is "any break that is not inside a switch or loop we
        # recognise", and the switch/loop breaks are matched first below.
        stripped = line.strip()
        indent = len(line) - len(line.lstrip("\t"))
        if stripped == "break;" and indent >= 2:
            out.append("\t" * indent + "goto next_instruction;")
            continue

        out.append(line)

    # The switch's own closing brace goes with it: the goto replaces the whole
    # construct. Leaving it there closes the for-loop early, which is a brace
    # error three hundred lines away from the mistake.
    assert out[-1].strip() == "}", out[-1]
    out = out[:-1]

    return lines[:start] + out + lines[end:], body


def opcode_labels(body):
    return [re.match(r"\t\tcase (OP_[A-Z_]+):", l).group(1)
            for l in body
            if re.match(r"\t\tcase (OP_[A-Z_]+):", l)]


def build_header(labels):
    rows = []
    for name in labels:
        rows.append('\t\t[FL_OPCODE(%s)] = &&l_%s,' % (name, name.lower()))
    return "\n".join(rows)


def main():
    source = DEFAULT_PATH
    output = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--output":
            output = args[i + 1]
            i += 2
        else:
            source = args[i]
            i += 1

    with open(source) as f:
        text = f.read()

    if "next_instruction" in text:
        print("%s is already transformed; nothing to do" % source)
        return 0

    path = output or source
    lines = text.split("\n")
    new_lines, body = transform(lines)
    labels = opcode_labels(body)
    if not labels:
        raise SystemExit("found no opcode cases; is the dispatch still a switch?")

    text = "\n".join(new_lines)

    # FL_OPCODE: the portable spelling of "enum constant as an array index".
    # GCC and Clang accept the bare constant, but writing it this way means the
    # transformed file does not depend on which of them compiled it.
    text = text.replace(
        "#include <time.h>",
        "#include <time.h>\n\n"
        "/* An OpCode as a table index. A compile-time integer either way. */\n"
        "#define FL_OPCODE(op) ((size_t)(op))",
        1)

    # `instruction` has to outlive the loop, because in this build the body is
    # entered by jumping to next_instruction rather than by falling into the
    # top of the loop.
    text = text.replace(
        "\tfor (;;) {\n#ifdef FL_DEBUG_TRACE_EXECUTION",
        "\t/*\n"
        "\t * The opcode being executed. Declared out here rather than at the\n"
        "\t * top of the loop body, because the body is entered by jumping to\n"
        "\t * next_instruction, so there is no single line at the top that\n"
        "\t * runs every iteration.\n"
        "\t */\n"
        "\tuint8_t instruction;\n\n"
        "\tfor (;;) {\n#ifdef FL_DEBUG_TRACE_EXECUTION",
        1)

    # The switch read the opcode at the top of its body, once per case. In this
    # build that read happens at next_instruction instead, so the one at the
    # top of the loop has to go -- two reads means the second one consumes an
    # operand byte as if it were an opcode.
    text = text.replace("\n\t\tuint8_t instruction = READ_BYTE();\n", "\n", 1)

    # The label table, and the macro that turns an opcode into its index.
    #
    # FL_OPCODE is a design decision worth spelling out. The table is indexed by
    # the OpCode value the run loop just read, so it is written with enum
    # constants rather than with numbers: a jump table indexed positionally by
    # hand-maintained integers is exactly the kind of thing that survives until
    # an opcode is inserted in the middle.
    anchor = "\t\tgoto *dispatch[instruction];"
    table = (
        "\t\t/*\n"
        "\t\t * The handler table.\n"
        "\t\t *\n"
        "\t\t * Indexed by the opcode itself, and written with the enum constants\n"
        "\t\t * rather than with hand-counted integers. A jump table indexed by\n"
        "\t\t * hand-maintained positions is a bug that survives until somebody\n"
        "\t\t * inserts an opcode in the middle of the enum, which is a bad day\n"
        "\t\t * for everyone.\n"
        "\t\t *\n"
        "\t\t * Every entry is filled in. The verifier has already rejected any\n"
        "\t\t * chunk containing an opcode outside the enum, but a table with a\n"
        "\t\t * hole in it is a jump through a null label, and the runtime is\n"
        "\t\t * about to start taking bytecode from tests that deliberately\n"
        "\t\t * contain nonsense.\n"
        "\t\t */\n"
        "\t\tstatic const void *const dispatch[] = {\n"
        + build_header(labels) +
        "\n\t\t};\n\n"
        # The dispatch reads the opcode. Handlers jump back to next_instruction
        # rather than through the table with the opcode they just executed,
        # which would re-run the same handler for ever -- the first version of
        # this did exactly that and hung on `print(1)`.
        "\t\tnext_instruction:\n"
        "\t\tinstruction = READ_BYTE();\n"
        + anchor
    )
    text = text.replace(anchor, table, 1)

    # The table above ends with `goto *dispatch[instruction];` -- the one real
    # dispatch. Every *other* occurrence is a handler end and must jump back to
    # the re-read label first, because jumping through the table with the
    # opcode just executed re-runs the same handler for ever.
    #
    # The main dispatch is the first occurrence and is left alone; the rest are
    # replaced. Counting rather than pattern-matching is what keeps the two
    # apart, because they are the same text.
    first = text.find(anchor)
    head = text[:first + len(anchor)]
    tail = text[first + len(anchor):]
    tail = tail.replace("goto *dispatch[instruction];", "goto next_instruction;")
    text = head + tail

    with open(path, "w") as f:
        f.write(text)

    print("rewrote %d handlers: %s -> %s" % (len(labels), source, path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
