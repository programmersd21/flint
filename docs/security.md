# security

what flint promises, what it does not, and where the boundary sits.
everything here was checked against the source; if the source changes,
this file has to change with it.

## process execution

`process.run` takes an argument vector, never a shell string, and the
runtime never invokes a shell implicitly. argument boundaries are
preserved exactly as the list gives them. there is no shell-execution
api, explicit or otherwise.

this is the one deliberate security property of the language, and it is
why `process.run(["rm", "-rf", dir])` cannot become a shell injection no
matter what `dir` contains.

## the filesystem

there is no permission system and no sandbox. a script that can run can
read and write every file its user can, through `read_file`,
`write_file` and the `fs` module. do not run untrusted flint programs.

## bytecode

all bytecode is produced by the compiler in the same process that runs
it. there is no bytecode file format, no loader, and no serialization, so
there is no untrusted-bytecode boundary to defend. the in-process
verifier (`src/runtime/verify.c`) checks opcodes, operands, constants,
locals and jump targets before execution, and it exists to catch compiler
bugs, not attackers.

## packages

there is no package manager, no registry and no lockfile, so there is no
package-installation attack surface either. anything that arrives as a
`.fl` file is source you can read.

## network

the `http` module is a blocking client. requests go out over plain
sockets; there is no tls, no certificate validation, and no hostname
verification, because there is no tls at all. do not send secrets or
credentials through it, and do not fetch code you intend to run without
reading it first.

there is no cryptography in the tree: no hashes, no hmac, no password
hashing, no random-bytes source beyond the non-cryptographic `random`
module, which its own documentation marks as unsuitable for security.
