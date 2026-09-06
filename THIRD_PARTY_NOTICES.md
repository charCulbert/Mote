# Third-party notices

Built with these projects. Thanks to their authors and contributors:

- [CLAP](https://github.com/free-audio/clap) and [clap-helpers](https://github.com/free-audio/clap-helpers) — Alexandre Bique and contributors.
- [clap-wrapper](https://github.com/free-audio/clap-wrapper) — defiantnerd and contributors; this project uses my fork.
- [CHOC](https://github.com/Tracktion/choc) — Tracktion Corporation and contributors.
- [Compost](https://github.com/charCulbert/compost) and [char-clap-utils](https://github.com/charCulbert/char-clap-utils) — Charlie Culbert.
- [WASI SDK](https://github.com/WebAssembly/wasi-sdk), [LLVM](https://github.com/llvm/llvm-project), and [wasi-libc](https://github.com/WebAssembly/wasi-libc) — their contributors, including the musl and dlmalloc authors.

The full copyright notices and license texts are in `THIRD_PARTY_LICENSES/`.
The dependency versions are pinned in the Git submodules. The runtime notices
cover WASI SDK 33.0 (LLVM 22.1.0 and wasi-libc revision `161b3195fc25`);
update them if you build with a different toolchain.

The plug-in's own code is ISC licensed. See `LICENSE`.
