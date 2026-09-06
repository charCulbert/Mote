# Mote

Hold a chord. Mote plays its notes one at a time, in sync with the host.
Place it **before a synth**, then start the transport.

- **Rate** — note division, including triplets; defaults to 1/8.
- **Octaves** — repeat the held chord over one to four octaves.
- **Gate** — note length, from 5–95% of the interval.
- **Up / down / up-down** — note order. Up-down turns around without repeating the end notes.
- **Hold** — keep the chord after releasing the keys; the next chord replaces it.
- **Clear** — release the chord.

The display lists the notes in playback order and underlines the sounding note.
Velocity and MIDI channel follow the input. Sustain holds the input chord;
other MIDI controllers pass through.

Available as CLAP, AUv3, and WCLAP. CLAP and WCLAP expose note ports without
audio ports. AUv3 provides a silent stereo output so the host can drive MIDI
processing through its audio render cycle.

## Build

Requires CMake 3.24+, a C++17 compiler, and Ninja or Xcode. Dependencies are
pinned submodules under `external/`.

```sh
git clone --recurse-submodules https://github.com/charCulbert/Mote.git
cd Mote
cmake --preset native
cmake --build --preset native
ctest --preset native
```

For WCLAP, install WASI SDK 33.0 with pthread support:

```sh
export WASI_SDK_ROOT=/path/to/wasi-sdk
cmake --preset wclap
cmake --build --preset wclap
```

For macOS AUv3:

```sh
cmake --preset xcode
cmake --build --preset xcode --config Release
```

Outputs are written to `build-*/artifacts/`. The WCLAP archive is
`build-wclap/artifacts/Mote.wclap.tar.gz`.

## Credits

Built with these projects. Thanks to their authors and contributors:

- [CLAP](https://github.com/free-audio/clap) and [clap-helpers](https://github.com/free-audio/clap-helpers) — Alexandre Bique and contributors.
- [clap-wrapper](https://github.com/free-audio/clap-wrapper) — defiantnerd and contributors; this project uses my fork.
- [CHOC](https://github.com/Tracktion/choc) — Tracktion Corporation and contributors.
- [Compost](https://github.com/charCulbert/compost) and [char-clap-utils](https://github.com/charCulbert/char-clap-utils) — Charlie Culbert.
- [WASI SDK](https://github.com/WebAssembly/wasi-sdk), [LLVM](https://github.com/llvm/llvm-project), and [wasi-libc](https://github.com/WebAssembly/wasi-libc) — their contributors, including the musl and dlmalloc authors.

## License

ISC. See [LICENSE](LICENSE). Each WCLAP archive contains this license and
[third-party notices](THIRD_PARTY_NOTICES.md), with the full dependency license
texts in `THIRD_PARTY_LICENSES/`.
