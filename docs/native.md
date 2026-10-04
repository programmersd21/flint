# Flint Native ABI & Embedding

## Native Extension ABI
Flint provides a C-facing ABI for native extension modules (`.so`, `.dylib`, `.dll`).

```c
#include "native.h"

FLINT_EXPORT Value flint_extension_init(VM *vm) {
    // Register native module functions
    return NIL_VAL;
}
```

## Embedding API
Applications can embed the Flint VM directly:

```c
#include "vm.h"

int main() {
    VM vm;
    vm_init(&vm);
    vm_interpret(&vm, "print('Hello from embedded Flint!')", "main");
    vm_free(&vm);
    return 0;
}
```
