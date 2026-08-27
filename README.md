# tbindiff

`tbindiff` is a colorful, side-by-side binary file comparison tool. It finds byte insertions, deletions, and substitutions using a dynamic-programming edit-distance algorithm.

```sh
make
./tbindiff old.bin new.bin
./tbindiff --ascii --show-matches old.bin new.bin
./tbindiff --hex --max-mem 10 old.bin new.bin
./tbindiff --color --width 160 old.bin new.bin | less -R
./tbindiff firmware.bin
./tbindiff --anchor-size 32 old.bin new.bin
./tbindiff --fine-diff old.bin new.bin
```

With one file argument, `tbindiff` acts as a simple hexadecimal or ASCII file viewer. With two, it shows their differences side by side. The default mode is detected from the inputs: when each file has less than 10% binary bytes, ASCII mode is used; otherwise hexadecimal mode is used. Binary bytes for this test are bytes above `0x7f` and ASCII controls other than `TAB`, `LF`, `VT`, `FF`, `CR`, and `ESC`. `--ascii` and `--hex` override detection. ASCII mode gives those six controls explicit names and renders other non-printable bytes as `<xXX>`. A rendered `<LF>` always ends the output row to preserve the input's line structure. `--annotate-spaces` displays spaces as `<SP>` in ASCII mode. ASCII rows are packed according to rendered width, including the extra columns used by annotations. Differences are red (deletions), green (insertions), or yellow (substitutions); matches are gray. Colors are enabled when standard output is a terminal; `--color` forces them through a pipe. `--width` overrides the detected terminal width, and `--progress` reports comparison progress on standard error.

The exit status follows the usual diff convention: 0 for identical files, 1 for differences, and 2 for an error. `--max-mem` is a rough MiB limit for dynamic-programming work rows. File contents, binary-anchor indexes, and the resulting edit script are not counted against it.

The comparison first finds unique, content-defined anchors and selects a non-crossing chain of them. Anchors are 16 bytes by default and can be configured with `-s`/`--anchor-size`. Every anchor is expanded through all adjacent matching bytes. Gaps between these substantial matches are shown directly as positional substitutions followed by remaining insertions or deletions; isolated equal bytes in a gap are intentionally not treated as meaningful matches. `--fine-diff` enables bounded dynamic programming inside the gaps when byte-level alignment is wanted. Work in large divergent gaps remains bounded, keeping comparisons of multi-megabyte files fast even when little content is shared.

## Building and testing

Requires a C++23 compiler.

```sh
make
make test
```
