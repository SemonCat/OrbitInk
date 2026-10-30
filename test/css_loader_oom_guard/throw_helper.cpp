// Compiled WITH exceptions enabled (default flags -- no -fno-exceptions here). This stands in for
// the precompiled C++ runtime library (libstdc++/libc++), which is always built with exceptions
// regardless of how app code is compiled. On the real device, operator new's failure path lives
// in exactly such a precompiled library and throws std::bad_alloc via __cxa_throw even though the
// app TUs that call it were built with -fno-exceptions (verified independently in this
// investigation by disassembling the toolchain's own libstdc++.a for rv32imac/ilp32). This file
// reproduces that boundary on the host: it is the only TU in this harness allowed to `throw`.
#include <new>

void throwBadAllocFromRuntime() { throw std::bad_alloc(); }
