# Third-party notices

Built with these projects. Credit goes to their maintainers and contributors:

- [CLAP](https://github.com/free-audio/clap) and [clap-helpers](https://github.com/free-audio/clap-helpers) — the project maintainers and contributors.
- [clap-wrapper](https://github.com/free-audio/clap-wrapper) — the project maintainers and contributors; this project uses my fork.
- [CHOC](https://github.com/Tracktion/choc) — the project maintainers and contributors.
- [Compost](https://github.com/charCulbert/compost) and [char-clap-utils](https://github.com/charCulbert/char-clap-utils) — the project maintainers and contributors.
- [WASI SDK](https://github.com/WebAssembly/wasi-sdk), [LLVM](https://github.com/llvm/llvm-project), and [wasi-libc](https://github.com/WebAssembly/wasi-libc) — their maintainers and contributors, including the wider upstream projects they incorporate.

The full copyright notices and license texts are in `THIRD_PARTY_LICENSES/`.
The dependency versions are pinned in the Git submodules. The runtime notices
cover WASI SDK 33.0 (LLVM 22.1.0 and wasi-libc revision `161b3195fc25`);
update them if you build with a different toolchain.

The plug-in's own code is ISC licensed. See `LICENSE`.
