# AMD Radeon GPU Profiler (RGP) Integration Guide for Tracy Profiler

This document serves as an architectural guide and implementation roadmap for integrating **AMD Radeon GPU Profiler (RGP)** data into Tracy Profiler. It is designed to give future contributors and agents an immediate starting point, detailing where relevant subsystems live in Tracy, how RGP hardware data is structured, and how to adapt Tracy's views to handle **SIMD-style GPU execution with instruction-level timing**.

---

## 1. Executive Summary & Goals

### The Opportunity
Tracy already provides first-class CPU profiling (instrumented zones, sampling, callstacks, context switches, locks) and high-level GPU zone profiling across Vulkan, Direct3D 11/12, OpenGL, OpenCL, and WebGPU.

However, GPU profiling in Tracy currently models GPU work at the **coarse event / pass level** (`GpuEvent` with `GpuStart` and `GpuEnd`). It has no concept of:
1. **Wavefronts / Warps:** Massive hardware multi-threading across Compute Units (CUs) / Workgroup Processors (WGPs).
2. **SIMD Lane Execution & Divergence:** Masked execution over 32 or 64 lanes with branch divergence.
3. **Instruction-Level Timing & Stalls:** Detailed instruction issue counts, memory dependency latency, LDS barriers, and register allocation pressure.

**AMD's Radeon GPU Profiler (RGP)** captures hardware-level Sequencer Thread Trace (SQTT) data from Radeon GPUs (GCN, RDNA 1/2/3/3.5). Integrating RGP data into Tracy brings nanosecond-accurate CPU execution timelines together with deep GPU hardware instruction traces in a single, unified view.

---

## 2. Tracy Architecture Map: What Lives Where

Understanding Tracy requires knowing its four discrete layers:

```
┌────────────────────────────────────────────────────────┐
│                      Client Layer                      │
│ (Instrumentation linked into target app or tracy-monitor)│
│ public/tracy/                 public/client/           │
└───────────────────────────┬────────────────────────────┘
                            │ Lock-Free Queue (LFQ) + TCP
┌───────────────────────────▼────────────────────────────┐
│                    Protocol Layer                      │
│ common/TracyProtocol.hpp     common/TracyQueue.hpp     │
└───────────────────────────┬────────────────────────────┘
                            │ Network Stream or File
┌───────────────────────────▼────────────────────────────┐
│                     Server Layer                       │
│ server/TracyWorker.hpp/cpp   server/TracyEvent.hpp     │
└───────────────────────────┬────────────────────────────┘
                            │ In-Memory Data Model
┌───────────────────────────▼────────────────────────────┐
│                   Presentation Layer                   │
│ profiler/src/profiler/       profiler/src/main.cpp     │
└────────────────────────────────────────────────────────┘
```

### 2.1. Client Instrumentation (`public/`)
* **`public/tracy/`**: Public API headers included by user code:
  * `TracyVulkan.hpp`, `TracyD3D12.hpp`, `TracyD3D11.hpp`, `TracyOpenGL.hpp`, `TracyOpenCL.hpp`: API-specific GPU command buffer instrumentation wrappers (`TracyGpuZone`, `TracyGpuContext`).
  * `Tracy.hpp`, `TracyC.h`: Core CPU instrumentation APIs.
* **`public/client/`**: Client runtime engine:
  * `TracyProfiler.cpp`: Client worker thread, network transport, packet serialization, calibration handshake.
  * `TracySysTrace.cpp`: Kernel system tracing (ETW on Windows, `perf_event` on Linux).
  * `TracyCallstack.cpp`: Symbol resolution (`DbgHelp` on Windows, `libbacktrace` on Linux).
  * `TracyRocprof.cpp`: Existing AMD ROCm/HIP runtime telemetry integration.

### 2.2. Protocol & Wire Representation (`common/`)
* **`common/TracyProtocol.hpp`**: Defines protocol packet types (`QueueType` enum), protocol version, and handshake flags.
  * GPU packet types: `GpuZoneBegin`, `GpuZoneEnd`, `GpuTime`, `GpuCalibration`, `GpuContextName`, `GpuZoneBeginCallstack`, `GpuZoneEndCallstack`.
* **`common/TracyQueue.hpp`**: POD data structures transferred across the socket or serialized into files.

### 2.3. Server Ingestion & Storage (`server/`)
* **`server/TracyEvent.hpp`**: Core data structures stored in the server's slab allocator:
  * `GpuCtxData`: Models an independent GPU hardware queue / context (period, timeDiff, calibration, timeline, custom notes/counters).
  * `GpuEvent`: 32-byte packed struct for GPU zones (`CpuStart`, `CpuEnd`, `GpuStart`, `GpuEnd`, `SrcLoc`, `Child`, `callstack`, `query_id`).
  * `ThreadData`: CPU threads, sample arrays, and context switches.
  * `PlotData`: Numerical time-series curves (e.g. VRAM usage, GPU clock, FPS).
* **`server/TracyWorker.hpp` / `server/TracyWorker.cpp`**:
  * Ingests packet streams from live clients or deserializes `.tracy` files.
  * Manages the slab allocators (`m_slab`).
  * Performs symbol indexing, disassembly caching (`m_data.symbolCode`), and callstack tree generation.
  * Exposes query APIs used by the profiler UI (`GetGpuData()`, `GetSamplesForSymbol()`, `GetSymbolStats()`).
* **`server/TracyFileRead.hpp` / `server/TracyFileWrite.hpp`**: High-performance streaming serialization using Zstandard compression.

### 2.4. Profiler Desktop GUI (`profiler/src/profiler/`)
* **`TracyView.cpp`**: Master UI loop, timeline coordinator, zooming, panning, range selection.
* **`TracyTimelineItemGpu.cpp`**: Renders GPU timeline rows, nested GPU zones, and stage dependencies.
* **`TracyView_GpuTimeline.cpp`**: Handles GPU zone hover inspection, zone statistics, and CPU-GPU correlation.
* **`TracySourceView.cpp`**: Source code side-by-side with disassembly and hardware cost heatmaps.
* **`TracyDisassembly.cpp`**: Interactive assembly viewer using Capstone (`cs_disasm`).

### 2.5. Importers (`import/src/`)
* **`import/src/import-chrome.cpp`**: Converts Chrome / Perfetto JSON trace files into `.tracy` files.
* **`import/src/import-fuchsia.cpp`**: Converts Fuchsia binary trace files into `.tracy` files.
* Uses `Worker::Worker(name, program, timeline, messages, plots, threadNames)` to construct an in-memory trace and write it out with `worker.Write(*f, false)`.

---

## 3. The AMD RGP Architecture & File Format

### 3.1. What is an RGP File?
An RGP capture (`.rgp`) is a binary chunk container (magic `RDGP`) generated by the **Radeon Developer Tool Suite** (Radeon Developer Panel + AMD Driver PAL / Vulkan / D3D12 layer).

The AMD open-source specifications and parsing headers are available in the [GPUOpen-Tools/Radeon-GPU-Profiler](https://github.com/GPUOpen-Tools/Radeon-GPU-Profiler) repository.

### 3.2. Core Chunks in RGP

```
┌────────────────────────────────────────────────────────┐
│                   RDGP Header & Index                  │
├───────────────────┬────────────────────────────────────┤
│ Chunk Identifier  │ Contents                           │
├───────────────────┼────────────────────────────────────┤
│ SQTT              │ Hardware Sequencer Thread Trace    │
│                   │ (Per-CU raw instruction tokens)   │
├───────────────────┼────────────────────────────────────┤
│ OCCUPANCY         │ Wavefront birth, death, SIMD lane  │
│                   │ allocations over time              │
├───────────────────┼────────────────────────────────────┤
│ CODE_OBJECT       │ Driver-compiled AMDGPU ELF binary  │
│                   │ (ISA machine code, symbols, VGPRs) │
├───────────────────┼────────────────────────────────────┤
│ DISPATCH_DRAWS    │ Draw, Dispatch, Barrier, Copy list │
├───────────────────┼────────────────────────────────────┤
│ PERF_COUNTERS     │ Hardware PMU counters per event    │
├───────────────────┼────────────────────────────────────┤
│ QUEUE_TIMINGS     │ Hardware queue submission timestamps│
└───────────────────┴────────────────────────────────────┘
```

### 3.3. Deep Dive: Sequencer Thread Trace (SQTT)
The Sequencer (SQ) in AMD RDNA / GCN hardware writes execution tokens into a circular memory buffer per Shader Array (SA) when tracing is enabled. These tokens represent the exact hardware execution trace:

1. **`INST` Token:** Instruction issued at a specific Program Counter (`PC`) by wave $W$ on SIMD $S$.
2. **`VALU_INST` / `SALU_INST`:** Indicates whether Vector ALU or Scalar ALU executed.
3. **`VMEM_WAIT` / `SMEM_WAIT`:** Hardware stall waiting for vector memory (texture/buffer load) or scalar memory (constant buffer load).
4. **`LDS_WAIT`:** Hardware stall waiting for Local Data Share (group shared memory) or barrier synchronization.
5. **`BRANCH_DIVERGENT`:** Branch instruction where some lanes in the wave took the branch while others fell through (lane mask divergence).
6. **`WAVE_START` / `WAVE_END`:** Allocation and retirement of a wavefront on a SIMD unit.

---

## 4. SIMD GPU Execution Model vs. Tracy's CPU Model

Integrating GPU instruction traces into Tracy requires understanding why GPU SIMD execution cannot be directly shoehorned into Tracy's CPU thread model:

| Characteristic | Tracy CPU Model | AMD GPU SIMD Model (RGP) | Architectural Requirement |
| :--- | :--- | :--- | :--- |
| **Execution Entity** | **OS Thread:** 1 continuous program counter per core. | **Wavefront:** A group of 32 (RDNA) or 64 (GCN) work-items executing in lock-step under an `EXEC` mask. | Need a hierarchy: Queue $\to$ Event/Pass $\to$ Workgroup $\to$ Wavefront $\to$ SIMD Lane. |
| **Control Flow** | Scalar jumps, branch prediction, speculation. | **Predicated Execution / Masking:** Divergent branches execute both paths with masked lanes. | Need **Active Lane Ratio** metric (e.g. 24/32 active lanes = 75% efficiency). |
| **Latency Hiding** | Deep out-of-order execution, speculative execution, massive cache hierarchy. | **Hardware Multi-Threading:** Switching between up to 32 active wavefronts on the same CU while waiting for memory. | Execution is heavily interleaved. Must separate **Issue Cycles**, **Memory Stall Cycles**, and **ALU Cycles**. |
| **ALU Hardware Units** | Uniform ALU / FPU per core. | Asymmetric units: **SALU** (Scalar ALU, 1 per wave for uniform control), **VALU** (Vector ALU, 32 lanes for per-work-item math), **LDS** (Local Data Share). | Disassembly needs category classification: VALU, SALU, VMEM, SMEM, LDS, Export. |
| **Register Pressure** | Fixed architectural registers (16 GPRs on x86, 32 on ARM), register renaming. | Dynamic register allocation: Shaders allocate VGPRs and SGPRs. Allocating more VGPRs directly reduces maximum wavefront occupancy on the CU. | Need live register pressure visualization alongside disassembly to identify occupancy bottlenecks. |

---

## 5. Data Model Design for GPU SIMD Data in Tracy

To represent GPU instruction timing and execution traces without breaking existing Tracy traces, add dedicated data structures in `server/TracyEvent.hpp`:

```cpp
// server/TracyEvent.hpp

// Classification of GPU instruction operations
enum class GpuInstCategory : uint8_t
{
    VALU,           // Vector ALU (v_add_f32, v_fma_f32, etc.)
    SALU,           // Scalar ALU (s_add_u32, s_mov_b32, etc.)
    VMEM_Read,      // Vector Memory Read (buffer_load, image_sample)
    VMEM_Write,     // Vector Memory Write (buffer_store)
    SMEM,           // Scalar Memory Read (s_load_dword)
    LDS,            // Local Data Share (ds_read, ds_write)
    Branch,         // Control flow (s_cbranch, s_branch)
    Barrier,        // Synchronization (s_barrier, s_waitcnt)
    Export,         // Vertex / Pixel export (exp)
    Other
};

// Instruction-level timing statistics for a specific instruction in a shader
struct GpuInstructionStats
{
    uint64_t pc;                    // Relative program counter offset
    uint32_t issueCount;            // Total times this instruction was issued
    uint64_t totalActiveLanes;      // Sum of active SIMD lanes across issues (for divergence calc)
    uint32_t vmemStallCycles;       // Cycles wavefronts were stalled waiting for VMEM
    uint32_t smemStallCycles;       // Cycles stalled waiting for SMEM
    uint32_t ldsStallCycles;        // Cycles stalled waiting for LDS / barrier
    uint32_t aluStallCycles;        // Cycles stalled waiting for ALU dependency
    uint16_t avgVgprUsage;          // Live VGPR pressure at this instruction
    GpuInstCategory category;       // Instruction category
};

// Wavefront execution slice (for the CU spatial timeline)
struct GpuWavefrontSlice
{
    int64_t startGpuTime;           // Hardware start timestamp
    int64_t endGpuTime;             // Hardware end timestamp
    uint16_t cuId;                  // Compute Unit / WGP index
    uint8_t simdId;                 // SIMD unit within CU (0..3)
    uint8_t waveId;                 // Wavefront index on that SIMD
    uint32_t workgroupId[3];        // Workgroup coordinates (X, Y, Z)
    uint32_t eventId;               // Parent Draw / Dispatch event index
    uint32_t pipelineHash;          // Compiled shader pipeline hash
};

// Aggregated shader disassembly and timing container
struct GpuShaderData
{
    uint64_t pipelineHash;
    StringIdx shaderName;
    uint8_t shaderStage;            // Compute, Vertex, Pixel, etc.
    uint32_t vgprCount;             // VGPR allocation per wave
    uint32_t sgprCount;             // SGPR allocation per wave
    uint32_t ldsSize;               // LDS bytes per workgroup
    uint32_t maxOccupancy;          // Theoretical maximum waves per CU
    Vector<uint8_t> isaCode;        // Raw AMDGPU ISA machine code
    unordered_flat_map<uint64_t, GpuInstructionStats> instructionStats;
};
```

---

## 6. Profiler UI: Adapting & Creating Tracy Views

### 6.1. View 1: Shader Disassembly & Instruction Stall Heatmap
Currently, `profiler/src/profiler/TracyDisassembly.cpp` uses Capstone for CPU code. Because Capstone lacks full native AMDGPU RDNA3/RDNA2 ISA support, shader disassembly should be handled via one of two paths:
1. **Pre-Disassembled ISA in Import:** The `.rgp` file's `CODE_OBJECT` chunk can be disassembled during import using `llvm-objdump -d --arch=amdgcn` or AMD's open-source ISA parser, storing the text lines directly.
2. **Dedicated Shader Disassembly View (`TracyGpuDisassembly.cpp`):**
   * Side-by-side with HLSL/GLSL source code (if source cache is present).
   * **Custom Table Columns:**
     * **Address:** Hex offset in shader binary.
     * **Instruction:** Formatted ISA (`v_fma_f32 v0, v1, v2, v3`).
     * **Issue Count:** Heatmap bar showing execution frequency.
     * **Avg Active Lanes:** Shows control divergence (e.g., `24.2 / 32` highlighted in yellow/orange if $< 80\%$).
     * **Stall Breakdown:** Stacked bar chart per line:
       * 🟥 Red: VMEM stall (memory bound).
       * 🟨 Yellow: LDS / Barrier stall (synchronization bound).
       * 🟦 Blue: ALU dependency stall.
       * 🟩 Green: Active execution.
     * **Live VGPR Pressure:** Line graph showing register count. If the line crosses the CU occupancy threshold (e.g. $> 64$ VGPRs on RDNA), highlight in red!

```
┌─────────┬───────────────────────────────┬────────────┬─────────────┬──────────────────────────┬──────┐
│ Offset  │ Instruction                   │ Issue Cnt  │ Active Lanes│ Stall Heatmap            │ VGPR │
├─────────┼───────────────────────────────┼────────────┼─────────────┼──────────────────────────┼──────┤
│ 0x0040  │ s_load_dwordx4 s[4:7], s[0:1] │ 1,024      │ 32.0 / 32   │ [SMEM: 12 cycles]        │ 16   │
│ 0x0048  │ s_waitcnt lgkmcnt(0)          │ 1,024      │ 32.0 / 32   │ [STALL: 48 cycles] 🟨    │ 16   │
│ 0x004C  │ v_mad_f32 v4, v1, s4, v0      │ 1,024      │ 32.0 / 32   │ [ALU: 4 cycles] 🟩       │ 18   │
│ 0x0054  │ v_cmpx_gt_f32 v4, 0.0         │ 1,024      │ 32.0 / 32   │ [BRANCH] 🟦              │ 18   │
│ 0x005C  │ image_sample v[8:11], v[2:5]  │ 1,024      │ 18.4 / 32 ⚠️│ [VMEM STALL: 220 c] 🟥🟥 │ 24   │
└─────────┴───────────────────────────────┴────────────┴─────────────┴──────────────────────────┴──────┘
```

### 6.2. View 2: Wavefront Spatial Timeline (CU / SIMD View)
Extend the GPU timeline track (`TracyTimelineItemGpu.cpp`):
* Clicking any Dispatch or Draw zone expands the **Wavefront Occupancy View**:
  * **$Y$-Axis:** Compute Units (CU 0, CU 1, ..., CU 31) and their 4 SIMD lanes.
  * **$X$-Axis:** Time.
  * Wavefronts are drawn as horizontal bars.
  * Instantly exposes:
    * **Wave Tails (Stragglers):** A few long-running waves keeping a CU occupied while other CUs sit idle.
    * **Under-Occupancy:** Large empty gaps where memory stalls prevent wave issue.

---

## 7. Implementation Roadmap: 4 Phases

### Phase 1: Standalone Importer (`import-rgp`)
* **Goal:** Convert an `.rgp` file into a native `.tracy` trace file viewable in the standard `tracy-profiler`.
* **Files:** Create `import/src/import-rgp.cpp`, register in `import/CMakeLists.txt`.
* **Tasks:**
  1. Parse the RDGP container chunks (`DISPATCH_DRAWS`, `QUEUE_TIMINGS`).
  2. Populate `Worker::ImportEventTimeline` with draw calls and dispatches as GPU zones.
  3. Parse the `OCCUPANCY` chunk and generate a continuous `PlotData` curve of active wavefronts over time.
  4. Construct `tracy::Worker` and save to `output.tracy`.
* **Deliverable:** Users can profile with RGP tools and immediately visualize GPU queues, dispatches, and occupancy curves alongside CPU timelines in Tracy.

### Phase 2: RGP Chunk Parser & Code Object Extraction
* **Goal:** Extract AMDGPU ELF Code Objects and SQTT instruction tokens from the `.rgp` file.
* **Dependencies:** Add AMD's open-source RGP specification parser (header-only) to `import/` or `server/`.
* **Tasks:**
  1. Parse the `CODE_OBJECT` chunk to extract shader stages, VGPR/SGPR counts, and ISA byte buffers.
  2. Map pipeline hashes to shader names and source locations.
  3. Decode the `SQTT` token stream into instruction issue events and stall categories per Program Counter.

### Phase 3: GPU Shader Disassembly & Timing View
* **Goal:** Enable clicking a GPU dispatch to inspect its ISA instructions, issue counts, and stall heatmaps.
* **Files:**
  * `server/TracyEvent.hpp`: Add `GpuInstructionStats` and `GpuShaderData`.
  * `server/TracyWorker.hpp/cpp`: Add storage and serialization for shader data.
  * `profiler/src/profiler/TracyGpuDisassembly.cpp`: Implement the shader disassembly table with stall heatmaps and divergence ratios.
* **Deliverable:** Users can double-click a GPU dispatch and see line-by-line shader performance and bottlenecks.

### Phase 4: Wavefront Occupancy Timeline
* **Goal:** Interactive timeline view showing wavefront execution across all GPU Compute Units.
* **Files:** `profiler/src/profiler/TracyView_GpuWavefronts.cpp`, `profiler/src/profiler/TracyTimelineItemGpu.cpp`.
* **Tasks:**
  1. Render the multi-lane CU grid under the GPU timeline.
  2. Color-code wavefront bars by current execution state (VALU, VMEM stall, LDS wait).
  3. Support zooming down to individual wavefront lifetimes.

---

## 8. File & Symbol Reference Cheat Sheet

| Feature / Task | Relevant Files in Tracy | Key Structs / Functions |
| :--- | :--- | :--- |
| **GPU Context Storage** | `server/TracyEvent.hpp` | `struct GpuCtxData`, `struct GpuCtxThreadData` |
| **GPU Zone Representation** | `server/TracyEvent.hpp` | `struct GpuEvent` |
| **GPU Stream Processing** | `server/TracyWorker.cpp` | `Worker::ProcessGpuZoneBegin()`, `Worker::ProcessGpuZoneEnd()` |
| **GPU Clock Calibration** | `server/TracyWorker.cpp` | `Worker::ProcessGpuCalibration()` |
| **GPU Timeline Rendering** | `profiler/src/profiler/TracyTimelineItemGpu.cpp` | `TimelineItemGpu::Preprocess()`, `View::DrawGpu()` |
| **GPU Zone Details Window** | `profiler/src/profiler/TracyView_GpuTimeline.cpp` | `View::DrawGpuTimeline()` |
| **CPU Disassembly Reference** | `profiler/src/profiler/TracyDisassembly.cpp` | `View::DisassemblyWindow()`, `CountAsmIpStats()` |
| **CPU Cost Heatmap Reference**| `profiler/src/profiler/TracySourceView.cpp` | `SourceView::Render()`, `GatherIpHwStats()` |
| **Trace Importer Pattern** | `import/src/import-chrome.cpp` | `Worker::ImportEventTimeline`, `worker.Write(*f)` |
| **Trace Serialization** | `server/TracyFileRead.hpp`, `TracyFileWrite.hpp` | `FileRead::Open()`, `FileWrite::Open()` |
| **Existing AMD Integration** | `public/client/TracyRocprof.cpp` | `RocprofInit()`, `RocprofActivityCallback()` |
