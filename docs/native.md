# native extensions and embedding

there is no stable way to extend flint from c yet. this is the honest
version of that sentence, and the rest of the file is what exists today
plus what a future version would have to provide before the claim could
change.

## what does not exist

* no public c header. `vm.h`, `object.h` and friends are the interpreter's
  own internals, not an api: they expose the raw structs, they change
  between commits, and nothing checks compatibility.
* no extension loading. there is no `dlopen` anywhere in the source, no
  `native.load`, and no init-function convention. a `.so` cannot be taught
  to flint at run time.
* no ownership contract for values crossing a c boundary, and no handle
  scopes, pinning, or root registration for native-held objects.

until those three exist, any c code that touches the vm is reaching into
the implementation at its own risk. the test suite does not cover that
use, and a future commit may break it without warning.

## what does exist

the interpreter is plain c11 with no c++ and no runtime beyond libc and
libm, so linking it into another program is a build-system question rather
than a language question. the entry points a host would use are:

```c
VM vm;
vm_init(&vm);
vm_interpret(&vm, "print(\"hello\")");
vm_free(&vm);
```

`vm_interpret` compiles and runs one source string, and returns
`INTERPRET_OK`, `INTERPRET_COMPILE_ERROR` or `INTERPRET_RUNTIME_ERROR`.
there is no supported way to read a script's values back out, register a
c function, or run two scripts that share state beyond what one `VM` value
already shares. a host that needs any of that is writing against
internals, with the caveat from the previous section.
