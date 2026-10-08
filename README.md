<div align="center">

# Tracy Profiler

</div>

### A real time, nanosecond resolution, remote telemetry, hybrid frame and sampling profiler for games and other applications.

Tracy supports profiling CPU (Direct support is provided for C, C++, Lua, Python and Fortran integration. At the same time, third-party bindings to many other languages exist on the internet, such as [Rust](https://github.com/nagisa/rust_tracy_client), [Zig](https://github.com/tealsnow/zig-tracy), [C#](https://github.com/clibequilibrium/Tracy-CSharp), [OCaml](https://github.com/imandra-ai/ocaml-tracy), [Odin](https://github.com/oskarnp/odin-tracy), etc.), GPU (All major graphics/compute APIs: OpenGL, Vulkan, Direct3D 11/12, Metal, OpenCL, CUDA, WebGPU.), memory allocations, locks, context switches, automatically attribute screenshots to captured frames, and much more.

- [Documentation](https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf) for usage and build process instructions
- [Releases](https://github.com/wolfpld/tracy/releases) containing the documentation (`tracy.pdf`) and compiled Windows x64 binaries (`Tracy-<version>.7z`) as assets
- [Changelog](NEWS)
- [Interactive demo](https://tracy.nereid.pl/)

![](doc/profiler.png)

![](doc/profiler2.png)

![](doc/profiler3.png)

[An Introduction to Tracy Profiler in C++ - Marcos Slomp - CppCon 2023](https://youtu.be/ghXk3Bk5F2U?t=37)

[Introduction to Tracy Profiler v0.2](https://www.youtube.com/watch?v=fB5B46lbapc)  
[New features in Tracy Profiler v0.3](https://www.youtube.com/watch?v=3SXpDpDh2Uo)  
[New features in Tracy Profiler v0.4](https://www.youtube.com/watch?v=eAkgkaO8B9o)  
[New features in Tracy Profiler v0.5](https://www.youtube.com/watch?v=P6E7qLMmzTQ)  
[New features in Tracy Profiler v0.6](https://www.youtube.com/watch?v=uJkrFgriuOo)  
[New features in Tracy Profiler v0.7](https://www.youtube.com/watch?v=_hU7vw00MZ4)  
[New features in Tracy Profiler v0.8](https://www.youtube.com/watch?v=30wpRpHTTag)

## Standalone Sample Profiling (`tracy-monitor`)

`tracy-monitor` samples applications without requiring source-level instrumentation or recompilation.

> **Windows Note:** Profiling via ETW sampling and context switch monitoring requires running your command prompt or terminal as **Administrator**.

### Usage

* **Profile directly to a `.tracy` file** (headless capture):
  ```sh
  tracy-monitor -u -o my_trace.tracy ./my_app [args...]
  ```
  * `-u`, `--user-only`: Filters out kernel-mode callstack frames so samples and exclusive time are attributed directly to user-space functions without kernel interruptions (e.g. `KeSaveStateForHibernate`).
  * `-o FILE`, `--output FILE`: Captures in-process and writes the trace directly to disk when the application exits or when interrupted (<kbd>Ctrl</kbd>+<kbd>C</kbd>).
  * Open `my_trace.tracy` in `tracy-profiler` to inspect the results.

* **Profile with live GUI connection**:
  Launch the application under `tracy-monitor`:
  ```sh
  tracy-monitor -u ./my_app [args...]
  ```
  Then connect the GUI profiler (`tracy-profiler -a 127.0.0.1`).

* **Attach to an existing process**:
  ```sh
  tracy-monitor -u -o my_trace.tracy -p <PID>
  tracy-monitor -u -o my_trace.tracy -n my_app.exe
  ```

* **Inspect an existing trace in user-space only mode**:
  Set the `TRACY_USER_ONLY_SAMPLES=1` environment variable before opening the trace:
  ```sh
  TRACY_USER_ONLY_SAMPLES=1 tracy-profiler my_trace.tracy
  ```
