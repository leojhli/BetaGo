# Third-party dependencies

- `nlohmann/json.hpp`: [JSON for Modern C++ 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0), MIT license in `nlohmann/LICENSE.MIT`.
- `tcl/`: public [Tcl 8.6.14 headers](https://github.com/tcltk/tcl/tree/core-8-6-14/generic) for the stable Tk 8.6 C API. Tcl and Tk redistribution licenses are included. `build.ps1` bundles the Tk DLLs and scripts beside `play.exe`.
- The original Tk runtime imports [zlib 1.3.1](https://github.com/madler/zlib/tree/v1.3.1). Its DLL is bundled with Tk; see `ZLIB-LICENSE.txt`.
- The MT19937 generator in `src/random.cpp` follows the original Matsumoto/Nishimura algorithm and preserves existing integer-seed and sampling conventions. See `MT19937-LICENSE.txt`.
- The build script pins the portable [Zig 0.14.1 compiler](https://ziglang.org/download/0.14.1/) and verifies the official SHA-256 before extraction. The compiler and its bundled C++ standard library live under the ignored `.tools/` directory. Zig compiles C++ here; this project has no Zig-language source.

Distribute the complete `build/` folder with its `runtime/` and `licenses/`
folders. The standalone runner and rules tests do not use Tk. The graphical
executable loads only the Tk libraries in its own runtime folder.
