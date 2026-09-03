# Plan: Supporting Windows in `tracy-monitor`

## 1. Executive Summary & Design Decision

### Sampling Engine: ETW (Tracy Native) vs. `SuspendThread` (VerySleepy)
**Decision: The Windows port of `tracy-monitor` will use Tracy's existing ETW (Event Tracing for Windows) kernel sampling pipeline, NOT VerySleepy's user-space `SuspendThread` approach.**

| Capability | Tracy ETW (`TracySysTrace.cpp`) | VerySleepy (`SuspendThread` / `StackWalk64`) |
| :--- | :--- | :--- |
| **Sampling Mechanism** | Kernel timer DPC interrupts CPU; kernel unwinds stack (`StackWalkEvent`). | User-space `SuspendThread` $\to$ `GetThreadContext` $\to$ `StackWalk64` $\to$ `ResumeThread`. |
| **Sampling Overhead** | Negligible ($\sim 1\text{--}2\%$). | High; severely disrupts application timing and scheduling. |
| **Deadlock Risk** | **None** (kernel handles unwinding). | **High** (suspending threads holding loader locks / heap locks causes deadlocks). |
| **Context Switches** | **Yes** (`CSwitch`, wait reasons, target CPU core). | **No**. |
| **Thread Wakeups** | **Yes** (`ReadyThread`). | **No**. |
| **Hardware VSync** | **Yes** (`DxgKrnl` VSync DPC). | **No**. |
| **Privilege Requirement** | Administrator (elevated token). | Standard user. |
| **Architecture Fit** | Natural: already integrated with Tracy's lock-free queues (`QueueType::CallstackSample`, etc.). | Mismatched: requires rewriting the sampling event loop. |

Running as Administrator on Windows is completely consistent with `tracy-monitor` on Linux, which requires `root` or `CAP_PERFMON + CAP_SYS_PTRACE`.

### Role of VerySleepy in this Port
VerySleepy (`./verysleepy`) serves as a reference for:
1. **DbgHelp External Target Setup (`symbolinfo.cpp`)**: Initializing DbgHelp on an external process handle (`SymInitialize(hProcess, ...)`), setting symbol paths (`_NT_SYMBOL_PATH`) to include the target exe directory, and refreshing module lists (`SymRefreshModuleList`).
2. **Process Discovery (`processinfo.cpp` / `debugger.cpp`)**: Using Toolhelp32 snapshots (`CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`) for attaching by executable name (`-n NAME`).
3. **Cross-Architecture Path Querying**: Using `QueryFullProcessImageName` to reliably retrieve target binary paths.

---

## 2. Component Breakdown & Required Changes

```mermaid
flowchart TD
    subgraph Core Client [public/client/]
        A[TracyCallstack.hpp] -->|Define TRACY_HAS_EXTERNAL_TARGET on Windows| B[TracyCallstack.cpp]
        B -->|Implement Windows InitExternalTarget & DbgHelp process routing| B
        C[TracySysTrace.cpp] -->|Filter ETW stack walks by GetExternalTargetPid| D[windows/TracyETW.cpp]
    end

    subgraph Monitor [monitor/]
        E[monitor/src/monitor.cpp] -->|Portable CLI parser| E
        E -->|CreateProcess CREATE_SUSPENDED & Toolhelp32 attach| E
        E -->|SetConsoleCtrlHandler & WinSock probing| E
        F[monitor/CMakeLists.txt] -->|Enable Windows, link ws2_32 dbghelp advapi32| F
    end

    subgraph Build & Toolchain
        G[MSVC v143+ / clang-cl / GCC] --> H[Build tracy-monitor.exe]
    end

    Core Client --> Monitor --> Build & Toolchain
```

### 2.1. Tracy Client Header (`public/client/TracyCallstack.hpp`)
* **Macro Guard:** Extend `TRACY_HAS_EXTERNAL_TARGET` to Windows desktop builds:
  ```cpp
  #if (defined(__linux__) && !defined(__ANDROID__) && defined(TRACY_HAS_CALLSTACK)) || \
      (defined(_WIN32) && !defined(TRACY_WIN32_NO_DESKTOP) && defined(TRACY_HAS_CALLSTACK))
  #  define TRACY_HAS_EXTERNAL_TARGET
  #endif
  ```
* **API Signature:** `InitExternalTarget(uint32_t targetPid)` (or `DWORD` on Windows) to avoid dependency on non-standard `pid_t`.

### 2.2. External Target & DbgHelp in `public/client/TracyCallstack.cpp`
* **Target State on Windows:**
  * `static HANDLE s_externalTargetProcess = NULL;`
  * `static uint32_t s_externalTargetPid = 0;`
  * `static char s_externalTargetName[64] = {};`
  * `static uint64_t s_externalTargetExeMtime = 0;`
* **`InitExternalTarget(uint32_t targetPid)`:**
  * Open process with `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE`.
  * Retrieve binary path via `QueryFullProcessImageNameA`.
  * Extract base filename into `s_externalTargetName`.
  * Get executable modification time via `GetFileTime` or `_stat64`.
* **DbgHelp Process Routing:**
  * Define helper `HANDLE GetDbgHelpProcess()` returning `s_externalTargetProcess ? s_externalTargetProcess : GetCurrentProcess()`.
  * In `DbgHelpInit()`:
    * Initialize DbgHelp on `GetDbgHelpProcess()`.
    * Append the directory containing the target executable to `_NT_SYMBOL_PATH` so local `.pdb` files are found.
  * In `CacheProcessModules()`:
    * Query `EnumProcessModules` and `GetModuleInformation` on `GetDbgHelpProcess()`.
    * Populate `s_imageCache` entries so fast module lookup succeeds.
  * In `GetModuleNameAndPrepareSymbols(uint64_t addr)`:
    * If address is not found in `s_imageCache` and an external process is active, call `SymRefreshModuleList(s_externalTargetProcess)` to pick up newly loaded DLLs.
  * In `DecodeCallstackPtr()`, `DecodeCallstackPtrFast()`, and `DecodeSymbolAddress()`:
    * Pass `GetDbgHelpProcess()` to `SymFromAddr`, `SymGetLineFromAddr64`, `SymFromInlineContext`, `SymGetLineFromInlineContext`, `SymAddrIncludeInlineTrace`, and `SymQueryInlineTrace`.
* **Disassembly Memory Query (`ReadExternalTargetMemory`)**:
  * Implement via `ReadProcessMemory(s_externalTargetProcess, (LPCVOID)addr, buf, size, &bytesRead)`.
* **Cleanup (`EndCallstack`)**:
  * If `s_externalTargetProcess != NULL`, call `SymCleanup(s_externalTargetProcess)` and `CloseHandle(s_externalTargetProcess)`.

### 2.3. ETW Stack Walk Filtering (`public/client/TracySysTrace.cpp`)
* In `SysTraceStart(int64_t& samplingPeriod)`:
  ```cpp
  #ifdef TRACY_HAS_EXTERNAL_TARGET
      s_pid = GetExternalTargetPid() != 0 ? GetExternalTargetPid() : GetCurrentProcessId();
  #else
      s_pid = GetCurrentProcessId();
  #endif
  ```
* When `EventRecordCallback` receives `etw::StackWalkGuid`:
  ```cpp
  if( sw->stackProcess == s_pid )
  ```
  The ETW kernel session will now extract and enqueue the target process's callstacks into `QueueType::CallstackSample`.

### 2.4. Monitor Driver (`monitor/src/monitor.cpp`)
* **CLI Option Parser:**
  * Replace `<getopt.h>` with a portable argument parser handling `-p/--pid`, `-n/--name`, `--hz`, `--port`, `-h/--help`, and positional program arguments.
* **Launch Mode (`RunForked` $\to$ Windows implementation):**
  * Format command line string with argument quoting.
  * Launch via `CreateProcessA(..., CREATE_SUSPENDED, ...)`.
  * Call `tracy::InitExternalTarget(pi.dwProcessId)`.
  * Verify ETW/Admin privileges (`etw::CheckAdminPrivilege()`).
  * Call `tracy::StartupProfiler()`.
  * Resume target: `ResumeThread(pi.hThread)`.
  * Monitor loop: `WaitForSingleObject(pi.hProcess, 100)`.
  * On exit, retrieve status via `GetExitCodeProcess`.
  * On Ctrl+C (`s_shouldQuit`), terminate target via `TerminateProcess(pi.hProcess, 1)`.
  * Cleanup handles and call `tracy::ShutdownProfiler()`.
* **Attach Mode (`RunAttached` $\to$ Windows implementation):**
  * PID attach: `OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid)`.
  * Name attach: Enumerate processes via `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)` and match `pe.szExeFile` (case-insensitive, optional `.exe`). If multiple match, list PIDs; if one matches, attach.
  * Liveness check: `WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT`.
  * On Ctrl+C or target exit, shut down profiler without terminating the attached process.
* **Signals & Sockets:**
  * Console events: `SetConsoleCtrlHandler` for `CTRL_C_EVENT` and `CTRL_BREAK_EVENT`.
  * Port probing: Initialize WinSock (`WSAStartup`), use `closesocket()`, handle non-blocking configuration.
  * Environment variables: Use `_putenv_s`.

### 2.5. Build Configuration (`monitor/CMakeLists.txt`)
* Remove `if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")`.
* Allow `Linux` and `Windows`.
* On Windows, link required system libraries:
  ```cmake
  if(WIN32)
      target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 dbghelp advapi32)
  endif()
  ```

---

## 3. Toolchain & Environment Details

* **MSVC BuildTools Location:**
  * Root: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`
  * `vcvarsall.bat`: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat`
  * Native 64-bit compiler: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64\cl.exe`
* **Alternative compilers present in environment:**
  * `clang++` / `clang-cl` (LLVM 21.1.0)
  * `g++` (MSYS2 UCRT64 / MinGW64)
  * `ninja` (MSYS2 and Scoop)

---
## 4. Implementation Status & Verification Results

### Phase 1: Core Client Modifications [COMPLETED]
- [x] **`public/client/TracyCallstack.hpp`**: Enabled `TRACY_HAS_EXTERNAL_TARGET` on Windows desktop (`_WIN32 && !TRACY_WIN32_NO_DESKTOP && TRACY_HAS_CALLSTACK`). Updated `InitExternalTarget` parameter to `uint32_t targetPid`.
- [x] **`public/client/TracyCallstack.cpp`**:
  - Implemented Windows external target state (`s_externalTargetProcess`, `s_externalTargetPid`, `s_externalTargetName`, `s_externalTargetExeMtime`).
  - Implemented `InitExternalTarget`, `GetExternalTargetPid`, `GetExternalTargetName`, `GetExternalTargetExeTime`, and `ReadExternalTargetMemory`.
  - Added `GetDbgHelpProcess()` routing all DbgHelp calls to `s_externalTargetProcess` when active or `GetCurrentProcess()` otherwise.
  - Configured `DbgHelpInit` to append the target executable's directory to `_NT_SYMBOL_PATH`.
  - **Key Finding & Fix**: Passed `fInvProcess = FALSE` to `SymInitialize` for external targets to prevent `0x8000000d` failures when initializing against `CREATE_SUSPENDED` processes before their PEB loader data is initialized. Module discovery is performed subsequently via `EnumProcessModules` and `SymRefreshModuleList`.
  - Handled target cleanup in `EndCallstack`.
- [x] **`public/client/TracySysTrace.cpp`**: Set `s_pid = GetExternalTargetPid() != 0 ? GetExternalTargetPid() : GetCurrentProcessId();` under `#ifdef TRACY_HAS_EXTERNAL_TARGET`.
- [x] **Compilation Verification**: Verified clean compilation of `TracyClient.cpp` under both MSVC (`cl.exe` v19.50) and LLVM `clang++` with zero errors and no regressions.

### Phase 2: `tracy-monitor` Porting [COMPLETED]
- [x] **`monitor/CMakeLists.txt`**: Added `Windows` to allowed platforms, added case-insensitive `CMAKE_SYSTEM_PROCESSOR` check for `AMD64`/`ARM64`, and linked `ws2_32 dbghelp advapi32` on Windows.
- [x] **`monitor/src/monitor.cpp`**:
  - Implemented portable CLI argument parser supporting `-p/--pid`, `-n/--name`, `--hz`, `--port`, `-h/--help`, and positional arguments without `<getopt.h>`.
  - Implemented Windows `RunForked` using `CreateProcessA(..., CREATE_SUSPENDED, ...)` with command line escaping and `ResumeThread`.
  - Implemented Windows `FindPidsByName` using `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`.
  - Implemented `ConsoleCtrlHandler` for clean shutdown on `CTRL_C_EVENT` and `CTRL_BREAK_EVENT`.
  - Implemented WinSock port probing and socket management (`closesocket`, `ioctlsocket`).
  - Added administrator privilege check and informative warning message.
- [x] **Build Verification**: Successfully compiled and linked `tracy-monitor.exe` using CMake and Ninja with MSVC BuildTools (`cl.exe`) with zero warnings.

### Phase 3: Testing & Verification [COMPLETED]
- [x] **CLI & Help**: `./monitor/build/tracy-monitor.exe --help` displays correct Windows-specific permissions and usage.
- [x] **Test Workload**: Created `monitor/build/test_workload.exe` with debug symbols (`/Zi`, `.pdb`).
- [x] **Launch Mode Test**:
  - Command: `./monitor/build/tracy-monitor.exe monitor/build/test_workload.exe`
  - Successfully created suspended process, extracted process name `'test_workload.exe'`, bound listen port, initialized DbgHelp without error, and reported ETW privilege status.
- [x] **Attach Mode Test**:
  - Non-existent PID: `./monitor/build/tracy-monitor.exe -p 999999` $\to$ correctly reports `Process 999999 does not exist or is not accessible.`
  - Non-existent Name: `./monitor/build/tracy-monitor.exe -n non_existent_process_123` $\to$ correctly reports `No process named 'non_existent_process_123'.`
  - Name Match: Ran `test_workload.exe` in background and attached via `tracy-monitor.exe -n test_workload` $\to$ successfully matched PID, opened target, extracted name, and initialized profiler.
