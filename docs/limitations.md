# Flint 0.8.0 Known Limitations

1. **Platform support**: tested primarily on Linux x86_64. The code is
   portable C11, but Windows and macOS are not covered by CI yet.
2. **Concurrency**: there is no thread, channel or mutex support yet.
   `docs/concurrency.md` describes the model that is intended, not one that
   is implemented.
3. **Networking**: HTTP is a blocking client. TCP, UDP, DNS, TLS, websocket
   and streaming are not implemented.
4. **Toolchain**: the `flint` executable supports running scripts, `-e`,
   stdin, the REPL, `sync` and the diagnostic flags. Subcommands such as
   `check`, `test`, `fmt`, `lint`, `build`, `bench`, `profile`, `debug`,
   `doc`, `package` and `lsp` are not implemented.
5. **Packages**: no package manager, registry or lockfile are implemented.
   `flint.toml` is not a supported source format yet.
6. **Unicode**: strings are byte-oriented with UTF-8 validation where
   documented. There is no grapheme-level segmentation.
7. **Secret data**: process arguments and environment are passed through
   exactly as given; there is no explicit shell, and no permission system.
8. **Sizes**: objects and tables are limited to `INT_MAX` elements and
   individual allocations to what `size_t` can express on the build platform.
