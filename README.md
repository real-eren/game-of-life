[Conway's Game of Life](https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life), in C99.
Renders in 4-bit ASCII color codes to the terminal.

How to run:
1) build with "cc gol_color.c".
 where 'cc' is one of "cc", "gcc", "clang", "cl" (msvc), "tcc", "zig cc".
2) Run the executable with "./a.out --help", or "gol_color --help" on Windows.

You can choose to add the optimization flags for your compiler,
but even debug builds should be plenty fast for normal usage.
[Additional help for msvc](https://learn.microsoft.com/en-us/cpp/build/walkthrough-compile-a-c-program-on-the-command-line)
