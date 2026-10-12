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
An RGP capture (`.rgp`) is a binary chunk container produced either by AMD's **Radeon Developer Tool Suite** (using the `AMD_RDF` / `libamdrdf` container) or by the open-source **Mesa / RADV** driver (using the `B00P` chunk container, magic `0x50303042`).

### 3.2. Core Chunks in RGP (B00P Specification)

The B00P container opens with a 56-byte header (`magic_number`, `version_major`, `version_minor`, `flags`, `chunk_offset`, and calendar timestamp), followed by sequential chunks identified by `sqtt_file_chunk_header`:

```
┌────────────────────────────────────────────────────────┐
│                 B00P Header (56 bytes)                 │
├───────────────────┬────────────────────────────────────┤
│ Chunk Type (ID)   │ Contents                           │
├───────────────────┼────────────────────────────────────┤
│ ASIC_INFO (0)     │ GPU family, CU counts, SIMDs/CU,   │
│                   │ VRAM size, timestamp frequency     │
├───────────────────┼────────────────────────────────────┤
│ CPU_INFO (7)      │ CPU brand, logical/physical cores  │
├───────────────────┼────────────────────────────────────┤
│ CLOCK_CALIB (6)   │ CPU vs GPU timestamp calibration   │
├───────────────────┼────────────────────────────────────┤
│ QUEUE_TIMINGS (5) │ Hardware queues (Graphics, Compute,│
│                   │ DMA) and submit/present timestamps │
├───────────────────┼────────────────────────────────────┤
│ SPM_DB (8 / 128)  │ Streaming Performance Monitor      │
│                   │ counter samples over time          │
├───────────────────┼────────────────────────────────────┤
│ CODE_OBJECT (9)   │ Compiled AMDGPU ELF shader binaries│
├───────────────────┼────────────────────────────────────┤
│ LOADER_EVENTS (10)│ Base addresses and load timestamps │
├───────────────────┼────────────────────────────────────┤
│ PSO_CORRELATION   │ Pipeline hashes to debug labels    │
├───────────────────┼────────────────────────────────────┤
│ SQTT_DATA (2)     │ Sequencer Thread Trace tokens &    │
│                   │ hardware markers (draws, barriers) │
└───────────────────┴────────────────────────────────────┘
```

### 3.3. Shader Disassembly

Shader disassembly uses an external LLVM `llvm-objdump` with AMDGPU support (official LLVM Windows releases lack `amdgcn`; build from source with `LLVM_TARGETS_TO_BUILD=AMDGPU`). Pass `--llvm-objdump <path>` or set `TRACY_LLVM_OBJDUMP`. Target is selected from ASIC info (`NAVI22` → `gfx1031`, `GFX1100` → `gfx1100`); decoding is limited to function symbols and stops at `s_code_end`. Without it, instructions show as unknown raw dwords.

### 3.4. Deep Dive: Sequencer Thread Trace (SQTT)
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

The current work is a prototype, not a verified implementation. The importer can produce a `.tracy` file and the profiler has wave-slot and shader views, but important parts of the capture are approximated, incomplete, or not validated against an independent decoder. See the known limitations in §9 before relying on any displayed counts or timings.

### Phase 1: Standalone Importer (`import-rgp`) [Prototype]
* **Goal:** Convert an `.rgp` file into a native `.tracy` trace file that can be opened in `tracy-profiler`.
* **Files:** `import/src/import-rgp.cpp`, registered in `import/CMakeLists.txt`.
* **Current scope:** Parses B00P chunks and writes Tracy traces. Queue submissions are represented as pseudo-thread tracks, not native Tracy GPU contexts. SPM timestamps are read, but no plots are created; the SPM counter names are not resolved.
* **Status:** Prototype output exists, but successful file generation does not establish correct event, clock, or counter interpretation.

### Phase 2: RGP Chunk Parser & Code Object Extraction [Partial]
* **Goal:** Decode SQTT and correlate its events with code objects and workload markers.
* **Files:** `import/src/import-rgp.cpp`.
* **Current scope:** SQTT `REG` writes to the `SQ_THREAD_TRACE_USERDATA_2/3` registers are decoded into command-buffer, API event, barrier, user-event, and layout-transition markers. CB start/end, barrier, and event IDs are normalized from the encoded global/per-frame union to the CB index; bind-pipeline marker IDs and chunk 5 `sqtt_cb_id` values are plain indices. The NAVI22 capture confirms this distinction: bind IDs match normalized CB indices, while all queue submit `sqtt_cb_id` values are zero and therefore use time-based submit matching. Markers are associated with queue submit spans to create pseudo-thread timeline zones. Chunk 11 PSO correlations are joined to chunk 9 AMDGPU ELF pipeline hashes, and chunk 10 loader records are joined to the full 128-bit internal pipeline hash. On the NAVI22 GFX10.3 validation capture, all 4 draw/dispatch zones resolved to a shader and 126,192 of 170,873 waves were attributed. The GFX11.0 Bioshock capture decodes its marker stream, but does not provide submit spans for marker-to-submit zone association. These format-specific joins have not been independently validated across a wider hardware set. Loader base addresses are parsed but are not yet used for PC-based wave attribution.

### Phase 3: GPU Shader Disassembly & Timing View [Prototype]
* **Goal:** Inspect shader ISA and measured instruction activity.
* **Files:** `server/TracyEvent.hpp`, `server/TracyWorker.hpp/cpp`, and `profiler/src/profiler/TracyView_GpuWaveSlots.cpp`.
* **Current scope:** Schema, serialization, and a shader table exist. The displayed instruction statistics and shader associations are placeholders, not measured per-instruction execution data.

### Phase 4: Wavefront Wave-Slot Timeline [Prototype]
* **Goal:** Display wavefront execution across GPU compute units, SIMDs, and wave slots.
* **Files:** `server/TracyEvent.hpp`, `profiler/src/profiler/TracyTimelineItemWaveSlots.hpp/cpp`, and `profiler/src/profiler/TracyView_GpuWaveSlots.cpp`.
* **Current scope:** Timeline and interaction scaffolding exist. The importer links state slices to completed waves and derives wave and slice intervals from SQTT timing tokens for GFX10.3, GFX11, and GFX12. GFX11.0 is exercised on a GFX1100 capture and against tinygrad's reference token excerpts; GFX12 token cases have reference unit coverage but have not been tested end-to-end on a capture. CU placement is derived from ASIC topology and packet fields; the per-architecture WGP mapping still needs independent hardware validation.

### Next Steps, In Order
1. Independently validate GFX10.3/GFX11 timing, CU/WGP placement, and userdata marker encodings/zone correlations; obtain an end-to-end GFX12 capture and validate its decoder and topology; validate the PSO/ELF/loader joins and add PC-based attribution where the capture exposes wave PCs.
2. Replace hard-coded shader statistics with per-PC aggregation from `INST` tokens.
3. Validate the resulting import and displayed data against independent tools and known captures before calling the phases verified.

### Open Questions
* Should imported queues become real Tracy GPU contexts, or remain pseudo-thread tracks?
* Where should the SPM counter-name table come from, and how should unsupported counters be represented?

After these correctness steps, remaining product work includes AMD_RDF (`libamdrdf`) container support, source-code mapping, aggregate shader views, and live VGPR-pressure visualization.
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

---

## 9. Current Implementation Status & Remaining Work (Backlog)

### 9.1. Prototype Components
* `tracy-import-rgp` parses B00P input and writes Tracy trace files; queue spans currently appear on pseudo-thread tracks.
* The Tracy schema and file read/write paths contain GPU wave and shader data structures, and the profiler has wave-slot and shader views.
* These components are not yet verified for data correctness. The decoder and UI have the following known limitations and placeholder data.

### 9.2. Known Limitations / Placeholder Data
* **Queue and marker model:** Queues and imported marker zones are pseudo-thread tracks, not real Tracy GPU contexts. The importer decodes `SQ_THREAD_TRACE_USERDATA_2/3` register writes into command-buffer, API event, barrier, user-event, and layout-transition markers. Chunk 11 maps 64-bit API PSO hashes to 128-bit internal pipeline hashes; the importer joins those to chunk 9 `.internal_pipeline_hash` metadata and stores the low 64-bit word as the Tracy shader key. Distinct full hashes with colliding low words are omitted rather than conflated. Draw/dispatch zones display the resolved shader key and matching `psoNames` name when available. Zone times are clipped to submit spans, and markers without a matching span are counted as unmatched. NAVI22 GFX10.3 is the validated submit/zone-join capture (4/4 shader zones resolved); GFX11.0 Bioshock decodes 27,913 markers with 2 decode errors, but has no usable submit spans and all 111,065 waves are outside any submit span. GFX11 marker-to-submit joining and event nesting therefore remain unverified.
* **SPM:** Only timestamps are read. No plots are created, and counter names are not resolved.
* **Time conversion:** GFX10.3 type-2 realtime samples and GFX11/GFX12 realtime timestamp markers correlate each SQTT stream's shader clock to the shared GPU timestamp counter; truncated GFX11/GFX12 samples are extended to the epoch nearest the calibration origin and wrap-extended in stream order. Multiple CPU/GPU calibration pairs are interpolated to map GPU ticks to CPU monotonic nanoseconds, with the ASIC timestamp frequency used for a single calibration pair. A stream with no realtime sample (or only one, for which the conversion rate cannot be measured) still requires a fallback estimate. NAVI22's 1,743 correlation points align all 170,873 waves with its nine submit spans (0 outside at ±10 μs). Bioshock's 2,554 GFX11 correlation points produce 111,065 waves with durations min/median/p99/max `0/1,260/687,194,766,000/10,995,134,901,730 ns`; the long tail includes 4,457 invalid-duration and 3,355 unterminated waves, and none of its waves can be checked against submit spans.
* **Shader identity and metadata:** Waves take the pipeline active at their wave-start time on the same SE stream, after resolving bind-pipeline API hashes through chunk 11 and the code-object database. If active command buffers disagree, the PSO/hash is absent, or the pipeline cannot resolve to a parsed ELF, the wave hash is zero (unknown). CB start/end, barrier, and event markers use the normalized `cb_index`; bind-pipeline and chunk 5 queue-submit CB IDs are plain values. On NAVI22, queue-submit CB IDs are zero, so marker-to-submit matching falls back to timestamp proximity. Chunk 10 code-object hashes are matched against the full 128-bit `.internal_pipeline_hash` from the ELF metadata; their base addresses are parsed but not yet used for PC-based attribution. `GpuWavefront` and shader keys carry the low 64 bits of the internal pipeline hash; a low-word collision is treated as ambiguous. In the NAVI22 sample, 126,192 waves resolve to one of 4 parsed shaders and 44,681 remain unknown. The shader stage, VGPR/SGPR counts, and LDS size come from PAL msgpack notes for a single known hardware stage; combined/unknown stages are marked unknown and their register counts remain zero. `issueCount` is fixed at 1, `totalActiveLanes` at 32, and stall values are still guessed from instruction category.
* **SQTT wave decoder:** Wave slices are buffered per active wave and appended contiguously when that wave ends; `stateCount` reflects the appended slices. Per-stream time advances by token deltas, including `TIME`, `VALU`/`INST`, and timed wave tokens; durations are not clamped, and incomplete final tokens, invalid durations, and unterminated waves are counted. GFX11 uses the RDNA3 token layouts; the Bioshock trace contains 844 unrecognized nibbles, which the reference decoder consumes as four-bit NOP fallbacks, plus one truncated final token. Tinygrad reference token excerpts validate GFX11 and GFX12 token lengths and fields, while the GFX10.3 `0xE`/`0xF` NOP interpretation remains capture-validated rather than independently verified. GFX11.0 wave decoding has been exercised on Bioshock; GFX12 has only unit/reference-excerpt coverage and is untested end-to-end.
* **Wave state meaning:** Slices are derived from `VALU` and `INST` token deltas and clipped to their parent wave interval. `INST` is represented as `SALU`, lane activity is fixed at 32, and stall categories are not decoded; these slices are not a complete hardware execution/stall breakdown.
* **Workgroup and CU mapping:** Wave-start tokens do not provide workgroup coordinates, so all coordinates are zero. Physical CU placement uses ASIC_INFO's CU/SIMD topology plus SE/SA/WGP/SIMD token fields; GFX11/12 WGPs are mapped to two CUs with two SIMDs each, and GFX12 uses a four-bit WGP field. The topology mapping is implemented but has not been independently checked against hardware or an end-to-end GFX12 capture.
* **Schema compatibility:** The appended file block bumps `TracyVersion` to 0.14.4. The reader retains compatibility with 0.14.3 wave records by widening the legacy 32-bit pipeline key on load. This is a fork-local version change and will clash with the next upstream version unless coordinated.
* **ISA disassembly and reported counts:** Disassembly requires external LLVM `llvm-objdump` built with AMDGPU support; see §3.3. Without it, instructions appear as unknown raw dwords. Bioshock GFX11 reports 111,065 completed waves, 317 linked slices, 844 unrecognized nibbles (handled as four-bit NOP fallbacks), one truncated token, and 2 marker decode errors. The packet-level GFX11 reference tests and capture import are verified, but the capture has substantial invalid/unterminated durations and no submit spans; these counts and timings are not independent ground-truth measurements. GFX12 has not been validated end-to-end.

### 9.3. Remaining Product Backlog
* [ ] **Dual Shader Aggregation View:** Toggle between a selected-wave view and statistics aggregated across a shader or dispatch after per-PC statistics are correct.
* [ ] **Live VGPR Pressure Visualization:** Render register allocation pressure alongside instruction lines.
* [ ] **HLSL / GLSL Source Code Mapping:** Show source alongside ISA when code-object debug information or embedded SPIR-V strings are available.
* [ ] **Per-CU Collapsible Folders:** Add expand/collapse controls to isolate compute units in the timeline.
* [ ] **Windows `AMD_RDF` (`libamdrdf`) Container Support:** Parse Radeon Developer Panel RDF containers alongside B00P.
