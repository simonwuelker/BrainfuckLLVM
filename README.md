This is a [brainfuck](https://en.wikipedia.org/wiki/Brainfuck) frontend for [LLVM](https://llvm.org/).

# Running it
Place the brainfuck program of your choice in `program.bf`,
then run
```
clang++ -std=c++14 -g -O3 codegen.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o codegen
```
to build the compiler and
```
./codegen | clang -x ir -
```
to compile your program (the `codegen` binary emits optimized LLVM IR which is then compiled by clang).

That's it, really (:
I made this as a weekend project, so please excuse the interface being a bit
clunky.
