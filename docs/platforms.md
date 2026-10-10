# platforms

what runs where, and what does not. a platform is *supported* when real
CI builds and tests it; anything else is best-effort, and this file says
which.

| platform | compiler | status |
|---|---|---|
| linux x86-64 | gcc, clang | supported: full CI, sanitizers, release artifacts |
| macOS (Apple silicon and Intel) | clang | probe: CI builds and runs the suites, signal-only |
| windows x86-64 | mingw-w64 gcc (MSYS2 MINGW64) | probe: CI builds and runs what can pass, signal-only |

MSVC is not targeted: the codebase is C11 with POSIX gaps closed by
`_WIN32` branches, and the Windows path is validated against mingw-w64
headers. the MSYS shell's emulated gcc is not a Windows result either;
CI names the mingw compiler explicitly so a PATH change cannot silently
substitute the emulated one.

## building

Linux and macOS need a C11 compiler and `make`:

```sh
make release
```

Windows needs MSYS2 with the MINGW64 toolchain:

```sh
pacman -S base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-make
make release CC=x86_64-w64-mingw32-gcc
```

## what works everywhere

the language, the bytecode verifier, the garbage collector, the
standard library in `lib/`, the package manager's path dependencies,
`flint fmt`, `flint test`, and the C embedding ABI. integer printing
uses `int64_t` throughout, because Windows `long` is 32 bits and the
`%ld` it used to print through is undefined behaviour there past 2^31.

paths in flint source use `/` on every system. paths the *platform*
hands flint (the script argument, the executable's own location, the
working directory) may use `\` on Windows, and the module resolver,
the project-root climb, and the stdlib locator all accept both.

## what windows does not do in 0.13.0

each of these is a documented runtime error, not a missing function or
a silent pretence:

- `process.run()` and `exec()`: no fork, no pipes, no child.
  `"process.run() needs a POSIX system in this release."`
- git dependencies: the git subprocess is the same fork family.
  `pkg add` of a URL fails; path dependencies work fully, lockfile
  content hashes included.
- `http`: plain sockets need Winsock and `https://` needs process
  spawning. requests return the usual `{ok: false, error}` table with
  `"http is not supported on Windows in this release."`
- `flint sync` downloads: the curl/wget subprocess it uses.
- `os.setenv` with an empty value: the system call deletes the
  variable instead of setting it, so the runtime returns false rather
  than reporting success and leaving `has()` disagreeing.

`process.fl` is therefore POSIX-only by design and does not pass on
Windows; every other language test does. the count, measured by
cross-compiling for x86_64-windows and running the suite under wine:
149 of 150, the one failure being the fork test above. wine is not
Windows, so that number is evidence, not support; support is what the
CI probe says once it runs on a real runner.

## 64-bit only

the VM uses nan-boxed values and requires a 64-bit host with pointers
that fit in 48 bits. that holds on x86-64 and ARM64 on all three
systems, and nowhere else is claimed.
