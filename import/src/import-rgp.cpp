#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

#include <algorithm>
#include <fstream>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
#include <zstd.h>

#ifdef _MSC_VER
#  define stat64 _stat64
#endif
#if defined __APPLE__
#  define stat64 stat
#endif

#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyMmap.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../public/common/TracyVersion.hpp"
#include "GitRef.hpp"

// RGP File Format Magic & Constants
static constexpr uint32_t SQTT_FILE_MAGIC_NUMBER  = 0x50303042; // 'B00P'
static constexpr uint32_t SQTT_FILE_HEADER_SIZE   = 56;
static constexpr uint32_t SQTT_GFXIP_LEVEL_GFXIP_10_3 = 0x9;
static constexpr int SQTT_SA_PER_SE = 2;

// SQTT Chunk Types
enum sqtt_file_chunk_type : uint8_t
{
    SQTT_FILE_CHUNK_TYPE_ASIC_INFO                  = 0,
    SQTT_FILE_CHUNK_TYPE_SQTT_DESC                  = 1,
    SQTT_FILE_CHUNK_TYPE_SQTT_DATA                  = 2,
    SQTT_FILE_CHUNK_TYPE_API_INFO                   = 3,
    SQTT_FILE_CHUNK_TYPE_RESERVED                   = 4,
    SQTT_FILE_CHUNK_TYPE_QUEUE_EVENT_TIMINGS        = 5,
    SQTT_FILE_CHUNK_TYPE_CLOCK_CALIBRATION          = 6,
    SQTT_FILE_CHUNK_TYPE_CPU_INFO                   = 7,
    SQTT_FILE_CHUNK_TYPE_SPM_DB                     = 8,
    SQTT_FILE_CHUNK_TYPE_CODE_OBJECT_DATABASE       = 9,
    SQTT_FILE_CHUNK_TYPE_CODE_OBJECT_LOADER_EVENTS  = 10,
    SQTT_FILE_CHUNK_TYPE_PSO_CORRELATION            = 11,
    SQTT_FILE_CHUNK_TYPE_INSTRUMENTATION_TABLE      = 12,

    SQTT_FILE_CHUNK_TYPE_DERIVED_SPM_DB             = 128
};

enum sqtt_queue_type : uint8_t
{
    SQTT_QUEUE_TYPE_UNKNOWN   = 0x0,
    SQTT_QUEUE_TYPE_UNIVERSAL = 0x1,
    SQTT_QUEUE_TYPE_COMPUTE   = 0x2,
    SQTT_QUEUE_TYPE_DMA       = 0x3,
};

enum sqtt_queue_event_type : uint32_t
{
    SQTT_QUEUE_TIMING_EVENT_CMDBUF_SUBMIT   = 0,
    SQTT_QUEUE_TIMING_EVENT_SIGNAL_SEMAPHORE= 1,
    SQTT_QUEUE_TIMING_EVENT_WAIT_SEMAPHORE  = 2,
    SQTT_QUEUE_TIMING_EVENT_PRESENT         = 3,
};

#pragma pack(push, 1)

struct sqtt_file_header
{
    uint32_t magic_number;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t flags;
    int32_t  chunk_offset;
    int32_t  second;
    int32_t  minute;
    int32_t  hour;
    int32_t  day_in_month;
    int32_t  month;
    int32_t  year;
    int32_t  day_in_week;
    int32_t  day_in_year;
    int32_t  is_daylight_savings;
};

struct sqtt_file_chunk_header
{
    uint32_t chunk_id; // type: 8, index: 8, reserved: 16
    uint16_t minor_version;
    uint16_t major_version;
    int32_t  size_in_bytes;
    int32_t  padding;
};

struct sqtt_queue_info_record
{
    uint64_t queue_id;
    uint64_t queue_context;
    uint32_t hardware_info;
    uint32_t reserved;
};

struct sqtt_queue_event_record
{
    uint32_t event_type;
    uint32_t sqtt_cb_id;
    uint64_t frame_index;
    uint32_t queue_info_index;
    uint32_t submit_sub_index;
    uint64_t api_id;
    uint64_t cpu_timestamp;
    uint64_t gpu_timestamps[2];
};

struct sqtt_clock_calibration_record
{
    uint64_t cpu_timestamp;
    uint64_t gpu_timestamp;
    uint64_t reserved;
};

struct ClockCalibrationPoint
{
    uint64_t cpuTimestamp;
    uint64_t gpuTimestamp;
};

struct SqttRealtimePoint
{
    uint64_t shaderTimestamp;
    uint64_t gpuTimestamp;
};

struct sqtt_pso_correlation_record
{
    uint64_t api_pso_hash;
    uint64_t pipeline_hash[2];
    char     api_level_obj_name[64];
};

struct sqtt_desc_record
{
    int32_t shader_engine_index;
    int32_t sqtt_version;
    int16_t instrumentation_spec_version;
    int16_t instrumentation_api_version;
    int32_t compute_unit_index;
};

#pragma pack(pop)

// GFX10 SQTT Token Types & Bitstream Lookup
enum SqttTokenType : uint8_t
{
    TOKEN_UNKNOWN = 0,
    TOKEN_INST,
    TOKEN_VALU,
    TOKEN_IMM_ONE,
    TOKEN_IMMED,
    TOKEN_WAVE_READY,
    TOKEN_NEW_PC,
    TOKEN_WAVE_START,
    TOKEN_WAVE_START_EXT,
    TOKEN_WAVE_ALLOC,
    TOKEN_WAVE_END,
    TOKEN_SHADER_DATA,
    TOKEN_SHADER_DATA_SHORT,
    TOKEN_UTIL_COUNTER,
    TOKEN_TIME,
    TOKEN_NOP,
    TOKEN_MISC,
    TOKEN_EVENT,
    TOKEN_EVENT_SYNC,
    TOKEN_REG,
    TOKEN_REG_INIT,
    TOKEN_TIMESTAMP,
    TOKEN_HEADER,
};

struct TokenEncoding
{
    uint8_t type;
    uint8_t pattern;
    uint8_t pattern_len;
    uint8_t token_len;
    uint8_t time_begin;
    uint8_t time_end;
};

static const TokenEncoding s_gfx10Encodings[] = {
    { TOKEN_INST,           0b010,     3, 20, 4,  7 },
    { TOKEN_VALU,           0b011,     3, 12, 3,  6 },
    { TOKEN_IMM_ONE,        0b1101,    4, 12, 4,  7 },
    { TOKEN_IMMED,          0b00100,   5, 24, 5,  8 },
    { TOKEN_WAVE_READY,     0b10100,   5, 24, 5,  8 },
    { TOKEN_NEW_PC,         0b0100001, 7, 64, 8, 11 },
    { TOKEN_WAVE_START,     0b01100,   5, 32, 5,  7 },
    { TOKEN_WAVE_START_EXT, 0b11100,   5, 48, 5,  7 },
    { TOKEN_WAVE_ALLOC,     0b00101,   5, 20, 5,  8 },
    { TOKEN_WAVE_END,       0b10101,   5, 20, 5,  8 },
    { TOKEN_SHADER_DATA,    0b00110,   5, 52, 5,  8 },
    { TOKEN_SHADER_DATA_SHORT, 0b10110, 5, 28, 5, 8 },
    { TOKEN_UTIL_COUNTER,   0b0110001, 7, 64, 7,  9 },
    { TOKEN_TIME,           0b1000,    4,  8, 4,  8 },
    { TOKEN_NOP,            0b0000,    4,  4, 0,  0 },
    { TOKEN_NOP,            0b1110,    4,  8, 0,  0 },
    { TOKEN_NOP,            0b1111,    4,  8, 0,  0 },
    { TOKEN_MISC,           0b1010001, 7, 24, 7, 16 },
    { TOKEN_EVENT,          0b01100001, 8, 24, 8, 11 },
    { TOKEN_EVENT_SYNC,     0b11100001, 8, 32, 8, 11 },
    { TOKEN_REG,            0b1001,    4, 64, 4,  7 },
    { TOKEN_REG_INIT,       0b1110001, 7, 64, 7, 10 },
    { TOKEN_TIMESTAMP,      0b0000001, 7, 64, 16, 64 },
    { TOKEN_HEADER,         0b0010001, 7, 64, 0,  0 },
};

struct TokenLookupEntry
{
    uint8_t type;
    uint8_t token_len;
    uint8_t time_begin;
    uint8_t time_end;
};

static TokenLookupEntry s_tokenTable[256];
static bool s_tableInitialized = false;

static void InitTokenTable()
{
    if( s_tableInitialized ) return;
    for( int i = 0; i < 256; i++ )
    {
        s_tokenTable[i] = { TOKEN_UNKNOWN, 4, 0, 0 };
    }
    for( const auto& enc : s_gfx10Encodings )
    {
        int step = 1 << enc.pattern_len;
        for( int i = enc.pattern; i < 256; i += step )
        {
            s_tokenTable[i] = { enc.type, enc.token_len, enc.time_begin, enc.time_end };
        }
    }
    s_tableInitialized = true;
}

static bool Gfx10CuLocation( int se, int shaderEngines, int cuPerSe, int shaderArraysPerSe,
    uint32_t sa, uint32_t wgp, uint32_t simd, int& cuId, uint32_t& simdInCu )
{
    constexpr int cusPerWgp = 2;
    constexpr int simdsPerCu = 2;
    if( se < 0 || se >= shaderEngines || shaderArraysPerSe <= 0 || cuPerSe % shaderArraysPerSe != 0 ) return false;
    if( sa >= static_cast<uint32_t>( shaderArraysPerSe ) || simd >= cusPerWgp * simdsPerCu ) return false;

    const int cusPerSa = cuPerSe / shaderArraysPerSe;
    if( cusPerSa % cusPerWgp != 0 || wgp >= static_cast<uint32_t>( cusPerSa / cusPerWgp ) ) return false;

    cuId = se * cuPerSe + sa * cusPerSa + wgp * cusPerWgp + simd / simdsPerCu;
    simdInCu = simd % simdsPerCu;
    return true;
}

static uint64_t ReadTokenWindow( const uint8_t* data, size_t bytes, size_t bitpos )
{
    const size_t byteIdx = bitpos / 8;
    const size_t bitOffset = bitpos % 8;
    uint64_t window = 0;
    const size_t bytesToCopy = (std::min)( sizeof( window ), bytes - byteIdx );
    memcpy( &window, data + byteIdx, bytesToCopy );
    window >>= bitOffset;
    if( bitOffset && byteIdx + sizeof( window ) < bytes )
    {
        window |= uint64_t( data[byteIdx + sizeof( window )] ) << ( 64 - bitOffset );
    }
    return window;
}

void Usage()
{
    printf( "tracy-import-rgp %i.%i.%i / %s\n\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef );
    printf( "Usage: import-rgp [--stats] input.rgp output.tracy\n\n" );
    printf( "Imports AMD Radeon GPU Profiler (.rgp) traces into Tracy Profiler format.\n" );
    printf( "Supports:\n" );
    printf( "  - Wave-Slot execution timeline (CU -> SIMD -> Slot multi-wave concurrency)\n" );
    printf( "  - Instruction-level disassembly with stall & execution heatmaps\n" );
    printf( "  - GPU hardware queues (Universal/Graphics, Compute, DMA)\n" );
    printf( "  - Command buffer submissions and synchronization markers\n" );
    printf( "  - Frame presentation marks\n" );
    printf( "  - SPM counter time-series plots (wavefront occupancy / GPU metrics)\n" );
    exit( 1 );
}

struct SqttDecodeStats
{
    std::vector<uint64_t> wavesPerSe;
    std::vector<int64_t> waveDurations;
    size_t wavesWithStates = 0;
    size_t wavesOutsideSubmitSpans = 0;
    size_t unknownTokens = 0;
    size_t desyncs = 0;
    size_t truncatedTokens = 0;
    size_t invalidDurationWaves = 0;
    size_t unterminatedWaves = 0;
    size_t slicesOutsideWaves = 0;
};

static void PrintSqttStats( const SqttDecodeStats& stats, size_t totalSlices )
{
    printf( "SQTT stats:\n" );
    for( size_t se = 0; se < stats.wavesPerSe.size(); se++ )
    {
        printf( "  SE %zu waves: %llu\n", se, (unsigned long long)stats.wavesPerSe[se] );
    }

    if( !stats.waveDurations.empty() )
    {
        auto durations = stats.waveDurations;
        std::sort( durations.begin(), durations.end() );
        const size_t count = durations.size();
        const int64_t median = count % 2 ? durations[count / 2] : durations[count / 2 - 1] + ( durations[count / 2] - durations[count / 2 - 1] ) / 2;
        const size_t p99 = ( count * 99 + 99 ) / 100 - 1;
        printf( "  wave duration ns (min/median/p99/max): %lld/%lld/%lld/%lld\n",
            (long long)durations.front(), (long long)median, (long long)durations[p99], (long long)durations.back() );
    }
    else
    {
        printf( "  wave duration ns (min/median/p99/max): unavailable\n" );
    }

    printf( "  waves with stateCount > 0: %zu/%zu; total slices: %zu; slices outside wave: %zu\n",
        stats.wavesWithStates, stats.waveDurations.size(), totalSlices, stats.slicesOutsideWaves );
    printf( "  waves outside any submit span (+/-10 us): %zu/%zu\n",
        stats.wavesOutsideSubmitSpans, stats.waveDurations.size() );
    printf( "  unknown tokens: %zu; desyncs: %zu; truncated tokens: %zu; invalid-duration waves: %zu; unterminated waves: %zu\n",
        stats.unknownTokens, stats.desyncs, stats.truncatedTokens, stats.invalidDurationWaves, stats.unterminatedWaves );
}

static const char* GetQueueTypeName( uint8_t qtype )
{
    switch( qtype )
    {
    case SQTT_QUEUE_TYPE_UNIVERSAL: return "Graphics/Universal";
    case SQTT_QUEUE_TYPE_COMPUTE:   return "Async Compute";
    case SQTT_QUEUE_TYPE_DMA:       return "DMA / Copy";
    default:                        return "Unknown Queue";
    }
}

struct DisasmResult
{
    std::string text;
    bool is64Bit;
    uint8_t category; // 0: VALU, 1: SALU, 2: VMEM, 3: SMEM, 4: LDS, 5: Barrier, 6: Wait, 7: Branch, 8: Export
    uint16_t latencyCycles;
};

static DisasmResult DisassembleAmdGpuInst( uint32_t dw0, uint32_t dw1 )
{
    DisasmResult res {};
    res.is64Bit = false;
    res.category = 0;
    res.latencyCycles = 4;

    const uint32_t top6 = ( dw0 >> 26 ) & 0x3F;
    const uint32_t top2 = ( dw0 >> 30 ) & 0x3;
    const uint32_t top1 = ( dw0 >> 31 ) & 0x1;

    // Check SOPP: 0xBF800000 mask 0xFF800000
    if( ( dw0 & 0xFF800000 ) == 0xBF800000 )
    {
        uint32_t op = ( dw0 >> 16 ) & 0x7F;
        uint16_t simm = uint16_t( dw0 & 0xFFFF );
        switch( op )
        {
        case 0:  res.text = "s_nop"; res.category = 1; res.latencyCycles = 1; return res;
        case 1:  res.text = "s_endpgm"; res.category = 7; res.latencyCycles = 1; return res;
        case 2:  res.text = "s_branch"; res.category = 7; res.latencyCycles = 4; return res;
        case 3:  res.text = "s_cbranch_scc0"; res.category = 7; res.latencyCycles = 4; return res;
        case 4:  res.text = "s_cbranch_scc1"; res.category = 7; res.latencyCycles = 4; return res;
        case 5:  res.text = "s_cbranch_vccz"; res.category = 7; res.latencyCycles = 4; return res;
        case 6:  res.text = "s_cbranch_vccnz"; res.category = 7; res.latencyCycles = 4; return res;
        case 7:  res.text = "s_cbranch_execz"; res.category = 7; res.latencyCycles = 4; return res;
        case 8:  res.text = "s_cbranch_execnz"; res.category = 7; res.latencyCycles = 4; return res;
        case 10: res.text = "s_barrier"; res.category = 5; res.latencyCycles = 64; return res;
        case 12: {
            char buf[64];
            snprintf( buf, sizeof( buf ), "s_waitcnt 0x%04x", simm );
            res.text = buf;
            res.category = 6;
            res.latencyCycles = ( simm == 0xc07f ) ? 48 : 32;
            return res;
        }
        case 13: res.text = "s_sethalt"; res.category = 7; res.latencyCycles = 1; return res;
        case 14: res.text = "s_sleep"; res.category = 7; res.latencyCycles = 1; return res;
        default: {
            char buf[64];
            snprintf( buf, sizeof( buf ), "s_sopp_%u 0x%04x", op, simm );
            res.text = buf;
            res.category = 1;
            res.latencyCycles = 1;
            return res;
        }
        }
    }

    // Check SOP1: 0xBE800000 mask 0xFF800000
    if( ( dw0 & 0xFF800000 ) == 0xBE800000 )
    {
        uint32_t sdst = ( dw0 >> 16 ) & 0x7F;
        uint32_t op = ( dw0 >> 8 ) & 0xFF;
        uint32_t ssrc0 = dw0 & 0xFF;
        const char* name = "s_mov_b32";
        if( op == 1 ) name = "s_mov_b64";
        else if( op == 2 ) name = "s_cmov_b32";
        else if( op == 4 ) name = "s_not_b32";
        else if( op == 28 ) name = "s_getpc_b64";
        else if( op == 29 ) name = "s_setpc_b64";
        else if( op == 30 ) name = "s_swappc_b64";
        char buf[64];
        snprintf( buf, sizeof( buf ), "%s s%u, s%u", name, sdst, ssrc0 );
        res.text = buf;
        res.category = 1;
        res.latencyCycles = 1;
        return res;
    }

    // Check SOPC: 0xBF000000 mask 0xFF800000
    if( ( dw0 & 0xFF800000 ) == 0xBF000000 )
    {
        uint32_t op = ( dw0 >> 16 ) & 0x7F;
        uint32_t ssrc1 = ( dw0 >> 8 ) & 0xFF;
        uint32_t ssrc0 = dw0 & 0xFF;
        const char* name = "s_cmp_eq_u32";
        if( op == 8 ) name = "s_cmp_gt_u32";
        else if( op == 9 ) name = "s_cmp_ge_u32";
        else if( op == 10 ) name = "s_cmp_lt_u32";
        else if( op == 11 ) name = "s_cmp_le_u32";
        char buf[64];
        snprintf( buf, sizeof( buf ), "%s s%u, s%u", name, ssrc0, ssrc1 );
        res.text = buf;
        res.category = 1;
        res.latencyCycles = 1;
        return res;
    }

    // Check SOPK: 0xB0000000 mask 0xF0000000
    if( ( dw0 & 0xF0000000 ) == 0xB0000000 )
    {
        uint32_t sdst = ( dw0 >> 16 ) & 0x7F;
        uint16_t simm = uint16_t( dw0 & 0xFFFF );
        char buf[64];
        snprintf( buf, sizeof( buf ), "s_movk_i32 s%u, 0x%04x", sdst, simm );
        res.text = buf;
        res.category = 1;
        res.latencyCycles = 1;
        return res;
    }

    // Check SOP2: bits [31:30] = 0b10 (0x80000000 mask 0xC0000000)
    if( top2 == 0b10 )
    {
        uint32_t op = ( dw0 >> 23 ) & 0x7F;
        uint32_t sdst = ( dw0 >> 16 ) & 0x7F;
        uint32_t ssrc1 = ( dw0 >> 8 ) & 0xFF;
        uint32_t ssrc0 = dw0 & 0xFF;
        const char* name = "s_add_u32";
        if( op == 1 ) name = "s_sub_u32";
        else if( op == 12 ) name = "s_and_b32";
        else if( op == 13 ) name = "s_or_b32";
        else if( op == 14 ) name = "s_xor_b32";
        else if( op == 20 ) name = "s_lshl_b32";
        else if( op == 21 ) name = "s_lshr_b32";
        else if( op == 24 ) name = "s_mul_i32";
        char buf[64];
        snprintf( buf, sizeof( buf ), "%s s%u, s%u, s%u", name, sdst, ssrc0, ssrc1 );
        res.text = buf;
        res.category = 1;
        res.latencyCycles = 1;
        return res;
    }

    // Check VOP1: 0x7E000000 mask 0xFE000000
    if( ( dw0 & 0xFE000000 ) == 0x7E000000 )
    {
        uint32_t vdst = ( dw0 >> 17 ) & 0xFF;
        uint32_t src0 = dw0 & 0x1FF;
        char buf[64];
        snprintf( buf, sizeof( buf ), "v_mov_b32 v%u, v%u", vdst, src0 );
        res.text = buf;
        res.category = 0;
        res.latencyCycles = 4;
        return res;
    }

    // Check VOPC: 0x7C000000 mask 0xFE000000
    if( ( dw0 & 0xFE000000 ) == 0x7C000000 )
    {
        uint32_t vsrc1 = ( dw0 >> 9 ) & 0xFF;
        uint32_t src0 = dw0 & 0x1FF;
        char buf[64];
        snprintf( buf, sizeof( buf ), "v_cmp_gt_f32 vcc, v%u, v%u", src0, vsrc1 );
        res.text = buf;
        res.category = 0;
        res.latencyCycles = 4;
        return res;
    }

    // Check VOP2: bits [31] = 0 (0x00000000 mask 0x80000000)
    if( top1 == 0b0 )
    {
        uint32_t op = ( dw0 >> 25 ) & 0x3F;
        uint32_t vdst = ( dw0 >> 17 ) & 0xFF;
        uint32_t vsrc1 = ( dw0 >> 9 ) & 0xFF;
        uint32_t src0 = dw0 & 0x1FF;
        const char* name = "v_add_f32";
        if( op == 0 ) name = "v_cndmask_b32";
        else if( op == 2 ) name = "v_sub_f32";
        else if( op == 4 ) name = "v_mul_f32";
        else if( op == 8 ) name = "v_min_f32";
        else if( op == 9 ) name = "v_max_f32";
        else if( op == 14 ) name = "v_lshlrev_b32";
        else if( op == 15 ) name = "v_lshrrev_b32";
        char buf[64];
        snprintf( buf, sizeof( buf ), "%s v%u, v%u, v%u", name, vdst, src0, vsrc1 );
        res.text = buf;
        res.category = 0;
        res.latencyCycles = 4;
        return res;
    }

    // Check SMEM (64-bit scalar memory): 0xF4000000 mask 0xFC000000
    if( top6 == 0b111101 )
    {
        res.is64Bit = true;
        uint32_t sdst = ( dw0 >> 6 ) & 0x7F;
        uint32_t sbase = ( dw0 & 0x3F ) << 1;
        char buf[64];
        snprintf( buf, sizeof( buf ), "s_load_dwordx4 s[%u:%u], s[%u:%u]", sdst, sdst + 3, sbase, sbase + 1 );
        res.text = buf;
        res.category = 3;
        res.latencyCycles = 40;
        return res;
    }

    // Check VOP3 (64-bit): 0xD4000000 or 0xD7000000 mask 0xFC000000
    if( top6 == 0b110101 || top6 == 0b110100 )
    {
        res.is64Bit = true;
        uint32_t vdst = dw0 & 0xFF;
        char buf[64];
        snprintf( buf, sizeof( buf ), "v_fma_f32 v%u, v0, v1, v2", vdst );
        res.text = buf;
        res.category = 0;
        res.latencyCycles = 4;
        return res;
    }

    // Check MUBUF / MTBUF (64-bit): 0xE0000000 mask 0xFC000000 or 0xC8
    if( top6 == 0b111000 || ( ( dw0 >> 24 ) == 0xc8 ) || ( ( dw0 >> 24 ) == 0xcc ) )
    {
        res.is64Bit = true;
        uint32_t vdata = ( dw0 >> 12 ) & 0xFF;
        uint32_t vaddr = dw0 & 0xFF;
        char buf[64];
        snprintf( buf, sizeof( buf ), "buffer_load_dword v%u, v%u, s[0:3], 0", vdata, vaddr );
        res.text = buf;
        res.category = 2;
        res.latencyCycles = 220;
        return res;
    }

    // Check FLAT / GLOBAL (64-bit): 0xDC000000 mask 0xFC000000
    if( top6 == 0b110111 )
    {
        res.is64Bit = true;
        uint32_t vdst = ( dw0 >> 24 ) & 0xFF;
        char buf[64];
        snprintf( buf, sizeof( buf ), "global_load_dword v%u, v[0:1], s[0:1]", vdst );
        res.text = buf;
        res.category = 2;
        res.latencyCycles = 220;
        return res;
    }

    // Check DS (64-bit LDS): 0xD8000000 mask 0xFC000000
    if( top6 == 0b110110 )
    {
        res.is64Bit = true;
        uint32_t vdst = ( dw0 >> 8 ) & 0xFF;
        char buf[64];
        snprintf( buf, sizeof( buf ), "ds_read_b32 v%u, v0", vdst );
        res.text = buf;
        res.category = 4;
        res.latencyCycles = 16;
        return res;
    }

    // Check EXP (64-bit export): 0xF8000000 mask 0xFC000000
    if( top6 == 0b111110 )
    {
        res.is64Bit = true;
        char buf[64];
        snprintf( buf, sizeof( buf ), "exp pos0, v0, v1, v2, v3" );
        res.text = buf;
        res.category = 8;
        res.latencyCycles = 16;
        return res;
    }

    // Literal constant
    char buf[64];
    snprintf( buf, sizeof( buf ), "literal_dword 0x%08x", dw0 );
    res.text = buf;
    res.category = 1;
    res.latencyCycles = 0;
    return res;
}

int main( int argc, char** argv )
{
#ifdef _WIN32
    if( !AttachConsole( ATTACH_PARENT_PROCESS ) )
    {
        AllocConsole();
        SetConsoleMode( GetStdHandle( STD_OUTPUT_HANDLE ), 0x07 );
    }
#endif

    InitTokenTable();

    tracy::FileCompression clev = tracy::FileCompression::Fast;

    bool printStats = false;
    int arg = 1;
    if( argc > arg && strcmp( argv[arg], "--stats" ) == 0 )
    {
        printStats = true;
        arg++;
    }
    if( argc != arg + 2 ) Usage();

    const char* input = argv[arg];
    const char* output = argv[arg + 1];

    printf( "Loading RGP trace: %s\n", input );
    fflush( stdout );

    FILE* f = fopen( input, "rb" );
    if( !f )
    {
        fprintf( stderr, "Cannot open input file: %s\n", input );
        return 1;
    }

    struct stat64 sb;
    if( stat64( input, &sb ) != 0 )
    {
        fprintf( stderr, "Cannot stat input file: %s\n", input );
        fclose( f );
        return 1;
    }

    const size_t fileSize = (size_t)sb.st_size;
    std::vector<uint8_t> fileBuffer;

    const auto fnsz = strlen( input );
    if( fnsz > 4 && memcmp( input + fnsz - 4, ".zst", 4 ) == 0 )
    {
        auto zbuf = (char*)mmap( nullptr, fileSize, PROT_READ, MAP_SHARED, fileno( f ), 0 );
        fclose( f );
        if( !zbuf )
        {
            fprintf( stderr, "Cannot mmap compressed file!\n" );
            return 1;
        }

        auto zctx = ZSTD_createDStream();
        ZSTD_initDStream( zctx );

        constexpr size_t tmpSize = 128 * 1024;
        auto tmp = std::make_unique<char[]>( tmpSize );

        ZSTD_inBuffer_s zin = { zbuf, fileSize };
        ZSTD_outBuffer_s zout = { tmp.get(), tmpSize };

        fileBuffer.reserve( 16 * 1024 * 1024 );

        while( zin.pos < zin.size )
        {
            const auto res = ZSTD_decompressStream( zctx, &zout, &zin );
            if( ZSTD_isError( res ) )
            {
                ZSTD_freeDStream( zctx );
                fprintf( stderr, "Couldn't decompress input file: %s\n", ZSTD_getErrorName( res ) );
                return 1;
            }
            if( zout.pos > 0 )
            {
                const auto bsz = fileBuffer.size();
                fileBuffer.resize( bsz + zout.pos );
                memcpy( fileBuffer.data() + bsz, tmp.get(), zout.pos );
                zout.pos = 0;
            }
        }
        ZSTD_freeDStream( zctx );
        munmap( zbuf, fileSize );
    }
    else
    {
        fileBuffer.resize( fileSize );
        const size_t bytesRead = fread( fileBuffer.data(), 1, fileSize, f );
        fclose( f );
        if( bytesRead != fileSize )
        {
            fprintf( stderr, "Failed to read complete file (%zu of %zu bytes)\n", bytesRead, fileSize );
            return 1;
        }
    }

    const uint8_t* data = fileBuffer.data();
    const size_t dataSize = fileBuffer.size();

    if( dataSize < SQTT_FILE_HEADER_SIZE )
    {
        fprintf( stderr, "File too small to be a valid RGP capture (%zu bytes)\n", dataSize );
        return 1;
    }

    const auto* header = reinterpret_cast<const sqtt_file_header*>( data );
    if( header->magic_number != SQTT_FILE_MAGIC_NUMBER )
    {
        fprintf( stderr, "Invalid RGP magic number: 0x%08X (expected 0x%08X 'B00P')\n", header->magic_number, SQTT_FILE_MAGIC_NUMBER );
        return 1;
    }

    printf( "RGP capture version %u.%u (Header size: %u bytes)\n", header->version_major, header->version_minor, SQTT_FILE_HEADER_SIZE );

    char gpuName[256] = "AMD Radeon GPU";
    uint64_t gpuTimestampFreq = 1000000000ULL;
    int32_t computeUnits = 40;
    int32_t shaderEngines = 2;
    int32_t wavesPerSimd = 20;
    int32_t simdsPerCu = 0;
    uint32_t sqttGfxipLevel = 0;

    std::vector<ClockCalibrationPoint> clockCalibrations;

    std::vector<sqtt_queue_info_record> queueInfos;
    std::vector<sqtt_queue_event_record> queueEvents;
    std::unordered_map<uint64_t, std::string> psoNames;

    uint32_t spmNumTimestamps = 0;
    uint32_t spmNumCounters = 0;
    uint32_t spmSampleInterval = 0;
    std::vector<uint64_t> spmTimestamps;

    // SQTT Chunk payloads (SE index -> raw token byte stream)
    std::vector<std::pair<sqtt_desc_record, std::pair<const uint8_t*, size_t>>> sqttStreams;
    sqtt_desc_record currentDesc {};

    // Code Object database ELFs
    std::vector<std::pair<const uint8_t*, size_t>> elfObjects;

    size_t offset = ( header->chunk_offset > 0 ) ? (size_t)header->chunk_offset : SQTT_FILE_HEADER_SIZE;
    size_t chunkCount = 0;

    while( offset + sizeof( sqtt_file_chunk_header ) <= dataSize )
    {
        const auto* chunkHdr = reinterpret_cast<const sqtt_file_chunk_header*>( data + offset );
        const uint8_t chunkType = static_cast<uint8_t>( chunkHdr->chunk_id & 0xFF );
        const int32_t chunkSize = chunkHdr->size_in_bytes;

        if( chunkSize <= 0 || offset + static_cast<size_t>( chunkSize ) > dataSize )
        {
            break;
        }

        const uint8_t* chunkPayload = data + offset + sizeof( sqtt_file_chunk_header );
        const size_t payloadSize = static_cast<size_t>( chunkSize ) - sizeof( sqtt_file_chunk_header );

        switch( chunkType )
        {
        case SQTT_FILE_CHUNK_TYPE_ASIC_INFO:
        {
            if( payloadSize >= 424 )
            {
                shaderEngines = *reinterpret_cast<const int32_t*>( chunkPayload + 40 );
                const int32_t cuPerSe = *reinterpret_cast<const int32_t*>( chunkPayload + 44 );
                computeUnits = shaderEngines * cuPerSe;
                simdsPerCu = *reinterpret_cast<const int32_t*>( chunkPayload + 48 );
                wavesPerSimd = *reinterpret_cast<const int32_t*>( chunkPayload + 52 );
                sqttGfxipLevel = *reinterpret_cast<const uint32_t*>( chunkPayload + 80 );

                const char* nameStr = reinterpret_cast<const char*>( chunkPayload + 136 );
                const size_t nameCapacity = (std::min)( sizeof( gpuName ) - 1, payloadSize - 136 );
                size_t nameLength = 0;
                while( nameLength < nameCapacity && nameStr[nameLength] ) nameLength++;
                if( nameLength > 0 )
                {
                    memcpy( gpuName, nameStr, nameLength );
                    gpuName[nameLength] = '\0';
                }

                const uint64_t freq = *reinterpret_cast<const uint64_t*>( chunkPayload + 408 );
                if( freq > 0 )
                {
                    gpuTimestampFreq = freq;
                }
            }
            printf( "  ASIC: %s, GFX level 0x%x, %d CUs (%d SEs, %d SIMDs/CU), %d Waves/SIMD, Freq: %llu Hz\n",
                gpuName, sqttGfxipLevel, computeUnits, shaderEngines, simdsPerCu, wavesPerSimd, (unsigned long long)gpuTimestampFreq );
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_CLOCK_CALIBRATION:
        {
            if( payloadSize >= sizeof( sqtt_clock_calibration_record ) )
            {
                const auto* calib = reinterpret_cast<const sqtt_clock_calibration_record*>( chunkPayload );
                clockCalibrations.push_back( ClockCalibrationPoint { calib->cpu_timestamp, calib->gpu_timestamp } );
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_QUEUE_EVENT_TIMINGS:
        {
            if( payloadSize >= 16 )
            {
                const uint32_t qInfoCount = *reinterpret_cast<const uint32_t*>( chunkPayload + 0 );
                const uint32_t qEvtCount  = *reinterpret_cast<const uint32_t*>( chunkPayload + 8 );

                const uint8_t* ptr = chunkPayload + 16;
                for( uint32_t i = 0; i < qInfoCount; i++ )
                {
                    queueInfos.push_back( *reinterpret_cast<const sqtt_queue_info_record*>( ptr + i * sizeof( sqtt_queue_info_record ) ) );
                }
                ptr += qInfoCount * sizeof( sqtt_queue_info_record );

                for( uint32_t i = 0; i < qEvtCount; i++ )
                {
                    queueEvents.push_back( *reinterpret_cast<const sqtt_queue_event_record*>( ptr + i * sizeof( sqtt_queue_event_record ) ) );
                }
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_PSO_CORRELATION:
        {
            if( payloadSize >= 16 )
            {
                const uint32_t rsize = *reinterpret_cast<const uint32_t*>( chunkPayload + 8 );
                const uint32_t rcount = *reinterpret_cast<const uint32_t*>( chunkPayload + 12 );

                const uint8_t* ptr = chunkPayload + 16;
                for( uint32_t i = 0; i < rcount && ptr + sizeof( sqtt_pso_correlation_record ) <= chunkPayload + payloadSize; i++ )
                {
                    const auto* rec = reinterpret_cast<const sqtt_pso_correlation_record*>( ptr );
                    if( rec->api_level_obj_name[0] != '\0' )
                    {
                        psoNames[rec->api_pso_hash] = rec->api_level_obj_name;
                    }
                    ptr += ( rsize > 0 ) ? rsize : sizeof( sqtt_pso_correlation_record );
                }
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_CODE_OBJECT_DATABASE:
        {
            if( payloadSize >= 16 )
            {
                const uint32_t rcount = *reinterpret_cast<const uint32_t*>( chunkPayload + 12 );
                size_t p = 16;
                for( uint32_t i = 0; i < rcount && p + 4 <= payloadSize; i++ )
                {
                    uint32_t elfSz = *reinterpret_cast<const uint32_t*>( chunkPayload + p );
                    p += 4;
                    if( p + elfSz <= payloadSize )
                    {
                        elfObjects.emplace_back( chunkPayload + p, elfSz );
                    }
                    p = ( p + elfSz + 3 ) & ~3;
                }
                printf( "  Found %zu compiled shader ELFs in CODE_OBJECT_DATABASE\n", elfObjects.size() );
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_SQTT_DESC:
        {
            if( payloadSize >= sizeof( sqtt_desc_record ) )
            {
                currentDesc = *reinterpret_cast<const sqtt_desc_record*>( chunkPayload );
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_SQTT_DATA:
        {
            if( payloadSize >= 8 )
            {
                const uint8_t* sqttStreamPtr = chunkPayload + 8;
                const size_t streamBytes = payloadSize - 8;
                sqttStreams.emplace_back( currentDesc, std::make_pair( sqttStreamPtr, streamBytes ) );
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_SPM_DB:
        {
            if( payloadSize >= 24 )
            {
                spmNumTimestamps   = *reinterpret_cast<const uint32_t*>( chunkPayload + 8 );
                spmNumCounters     = *reinterpret_cast<const uint32_t*>( chunkPayload + 12 );
                spmSampleInterval  = *reinterpret_cast<const uint32_t*>( chunkPayload + 20 );

                const size_t preambleBytes = 24 + spmNumCounters * 20;
                if( payloadSize >= preambleBytes + 32 + spmNumTimestamps * sizeof( uint64_t ) )
                {
                    const uint8_t* spmData = chunkPayload + preambleBytes + 32;
                    const uint64_t* tsPtr = reinterpret_cast<const uint64_t*>( spmData );
                    spmTimestamps.assign( tsPtr, tsPtr + spmNumTimestamps );
                }
            }
            break;
        }
        default:
            break;
        }

        offset += static_cast<size_t>( chunkSize );
        chunkCount++;
    }

    printf( "Processed %zu chunks (%zu SQTT streams, %zu shader ELFs).\n", chunkCount, sqttStreams.size(), elfObjects.size() );

    const bool sqttArchSupported = sqttGfxipLevel == SQTT_GFXIP_LEVEL_GFXIP_10_3 && simdsPerCu == 2;
    if( sqttArchSupported )
    {
        printf( "  SQTT architecture: GFX10.3 (%s)\n", gpuName );
    }
    else
    {
        fprintf( stderr, "Warning: unsupported SQTT architecture for ASIC '%s' (GFX level 0x%x, %d SIMDs/CU); skipping SQTT wave decoding.\n",
            gpuName, sqttGfxipLevel, simdsPerCu );
    }

    // Clock calibration CPU timestamps use CLOCK_MONOTONIC and are expressed in nanoseconds.
    std::stable_sort( clockCalibrations.begin(), clockCalibrations.end(), []( const auto& l, const auto& r ) {
        return l.gpuTimestamp < r.gpuTimestamp;
    } );
    clockCalibrations.erase( std::unique( clockCalibrations.begin(), clockCalibrations.end(), []( const auto& l, const auto& r ) {
        return l.gpuTimestamp == r.gpuTimestamp;
    } ), clockCalibrations.end() );

    const uint64_t fallbackBaseGpuTs = !queueEvents.empty() && queueEvents[0].gpu_timestamps[0] > 0 ? queueEvents[0].gpu_timestamps[0] : 0;
    const uint64_t baseCpuTs = clockCalibrations.empty() ? 0 : clockCalibrations.front().cpuTimestamp;
    const uint64_t sqttGpuOrigin = clockCalibrations.empty() ? fallbackBaseGpuTs : clockCalibrations.front().gpuTimestamp;
    printf( "  Clock calibration points: %zu%s\n", clockCalibrations.size(),
        clockCalibrations.empty() ? " (using nominal ASIC frequency)" : "" );

    auto GpuTickToNs = [&]( uint64_t gpuTick ) -> int64_t {
        if( !clockCalibrations.empty() )
        {
            const ClockCalibrationPoint* lo;
            const ClockCalibrationPoint* hi;
            if( clockCalibrations.size() == 1 )
            {
                lo = hi = &clockCalibrations.front();
            }
            else
            {
                const auto upper = std::lower_bound( clockCalibrations.begin(), clockCalibrations.end(), gpuTick,
                    []( const auto& point, uint64_t tick ) { return point.gpuTimestamp < tick; } );
                if( upper == clockCalibrations.begin() )
                {
                    lo = &clockCalibrations[0];
                    hi = &clockCalibrations[1];
                }
                else if( upper == clockCalibrations.end() )
                {
                    lo = &clockCalibrations[clockCalibrations.size() - 2];
                    hi = &clockCalibrations.back();
                }
                else
                {
                    lo = &*( upper - 1 );
                    hi = &*upper;
                }
            }

            long double mappedCpuTs;
            if( lo == hi || hi->gpuTimestamp == lo->gpuTimestamp )
            {
                mappedCpuTs = static_cast<long double>( lo->cpuTimestamp ) +
                    ( static_cast<long double>( gpuTick ) - lo->gpuTimestamp ) * 1e9L / gpuTimestampFreq;
            }
            else
            {
                const long double cpuSpan = static_cast<long double>( hi->cpuTimestamp ) - lo->cpuTimestamp;
                const long double gpuSpan = static_cast<long double>( hi->gpuTimestamp ) - lo->gpuTimestamp;
                mappedCpuTs = static_cast<long double>( lo->cpuTimestamp ) +
                    ( static_cast<long double>( gpuTick ) - lo->gpuTimestamp ) * cpuSpan / gpuSpan;
            }
            return static_cast<int64_t>( mappedCpuTs - baseCpuTs );
        }

        if( gpuTick < fallbackBaseGpuTs ) return 0;
        const double deltaTicks = static_cast<double>( gpuTick - fallbackBaseGpuTs );
        return static_cast<int64_t>( ( deltaTicks / static_cast<double>( gpuTimestampFreq ) ) * 1e9 );
    };

    std::vector<std::vector<SqttRealtimePoint>> streamRealtimePoints( sqttStreams.size() );
    size_t totalRealtimePoints = 0;
    for( size_t streamIndex = 0; streamIndex < sqttStreams.size(); streamIndex++ )
    {
        const auto& streamPair = sqttStreams[streamIndex];
        const uint8_t* streamData = streamPair.second.first;
        const size_t streamBytes = streamPair.second.second;
        const size_t totalBits = streamBytes * 8;
        size_t bitpos = 0;
        uint64_t shaderTimestamp = 0;
        auto& points = streamRealtimePoints[streamIndex];

        while( bitpos + 4 <= totalBits )
        {
            const uint64_t window = ReadTokenWindow( streamData, streamBytes, bitpos );
            const auto& info = s_tokenTable[window & 0xFF];
            if( info.type == TOKEN_UNKNOWN )
            {
                bitpos += 4;
                continue;
            }
            if( info.token_len > totalBits - bitpos ) break;

            if( info.type == TOKEN_TIMESTAMP )
            {
                const uint64_t timestampType = ( window >> 14 ) & 0x3;
                const uint64_t timestamp = window >> 16;
                if( timestampType == 1 )
                {
                    shaderTimestamp += timestamp;
                }
                else if( timestampType == 2 )
                {
                    points.push_back( SqttRealtimePoint { shaderTimestamp, timestamp } );
                }
            }
            else if( info.time_end > info.time_begin )
            {
                const uint64_t timeMask = ( uint64_t( 1 ) << ( info.time_end - info.time_begin ) ) - 1;
                shaderTimestamp += ( window >> info.time_begin ) & timeMask;
                if( info.type == TOKEN_TIME ) shaderTimestamp += 4;
            }

            bitpos += info.token_len;
        }

        std::stable_sort( points.begin(), points.end(), []( const auto& l, const auto& r ) {
            return l.shaderTimestamp < r.shaderTimestamp;
        } );
        points.erase( std::unique( points.begin(), points.end(), []( const auto& l, const auto& r ) {
            return l.shaderTimestamp == r.shaderTimestamp;
        } ), points.end() );
        totalRealtimePoints += points.size();
    }
    printf( "  SQTT shader/GPU clock correlation points: %zu across %zu streams\n", totalRealtimePoints, sqttStreams.size() );

    auto ShaderTickToGpuTick = [&]( const std::vector<SqttRealtimePoint>& points, uint64_t shaderTick ) -> uint64_t {
        if( points.empty() ) return sqttGpuOrigin + shaderTick;
        if( points.size() == 1 )
        {
            const long double gpuTick = static_cast<long double>( points.front().gpuTimestamp ) +
                static_cast<long double>( shaderTick ) - points.front().shaderTimestamp;
            if( gpuTick <= 0 ) return 0;
            if( gpuTick >= static_cast<long double>( UINT64_MAX ) ) return UINT64_MAX;
            return static_cast<uint64_t>( gpuTick );
        }

        const auto upper = std::lower_bound( points.begin(), points.end(), shaderTick,
            []( const auto& point, uint64_t tick ) { return point.shaderTimestamp < tick; } );
        const SqttRealtimePoint* lo;
        const SqttRealtimePoint* hi;
        if( upper == points.begin() )
        {
            lo = &points[0];
            hi = &points[1];
        }
        else if( upper == points.end() )
        {
            lo = &points[points.size() - 2];
            hi = &points.back();
        }
        else
        {
            lo = &*( upper - 1 );
            hi = &*upper;
        }

        const long double shaderSpan = static_cast<long double>( hi->shaderTimestamp ) - lo->shaderTimestamp;
        if( shaderSpan <= 0 ) return lo->gpuTimestamp;
        const long double gpuTick = static_cast<long double>( lo->gpuTimestamp ) +
            ( static_cast<long double>( shaderTick ) - lo->shaderTimestamp ) *
            ( static_cast<long double>( hi->gpuTimestamp ) - lo->gpuTimestamp ) / shaderSpan;
        if( gpuTick <= 0 ) return 0;
        if( gpuTick >= static_cast<long double>( UINT64_MAX ) ) return UINT64_MAX;
        return static_cast<uint64_t>( gpuTick );
    };

    // Prepare Tracy Ingestion Containers
    std::vector<tracy::Worker::ImportEventTimeline> timeline;
    std::vector<tracy::Worker::ImportEventMessages> messages;
    std::vector<tracy::Worker::ImportEventPlots>    plots;
    std::unordered_map<uint64_t, std::string>      threadNames;

    struct SubmitSpan
    {
        uint32_t queueIndex;
        uint32_t cbId;
        int64_t start;
        int64_t end;
    };
    std::vector<SubmitSpan> submitSpans;
    bool printedSubmitClockSample = false;
    int64_t minSubmitStartNs = INT64_MAX;
    int64_t maxSubmitEndNs = INT64_MIN;

    if( queueInfos.empty() )
    {
        sqtt_queue_info_record q {};
        q.hardware_info = SQTT_QUEUE_TYPE_UNIVERSAL;
        queueInfos.push_back( q );
    }

    for( size_t qIdx = 0; qIdx < queueInfos.size(); qIdx++ )
    {
        const uint64_t pseudoTid = static_cast<uint64_t>( qIdx + 1 );
        const uint8_t qType = static_cast<uint8_t>( queueInfos[qIdx].hardware_info & 0xFF );
        char nameBuf[128];
        snprintf( nameBuf, sizeof( nameBuf ), "%s (Queue %zu)", GetQueueTypeName( qType ), qIdx );
        threadNames[pseudoTid] = nameBuf;
    }

    for( const auto& ev : queueEvents )
    {
        const uint64_t pseudoTid = static_cast<uint64_t>( ( ev.queue_info_index < queueInfos.size() ? ev.queue_info_index : 0 ) + 1 );

        if( ev.event_type == SQTT_QUEUE_TIMING_EVENT_CMDBUF_SUBMIT )
        {
            const uint64_t gpu0 = ev.gpu_timestamps[0];
            const uint64_t gpu1 = ev.gpu_timestamps[1];

            if( gpu0 > 0 && gpu1 > gpu0 )
            {
                const int64_t ts0 = GpuTickToNs( gpu0 );
                const int64_t ts1 = GpuTickToNs( gpu1 );
                if( !printedSubmitClockSample )
                {
                    printf( "  Submit GPU timestamps: %llu..%llu -> %lld..%lld ns\n",
                        (unsigned long long)gpu0, (unsigned long long)gpu1, (long long)ts0, (long long)ts1 );
                    printedSubmitClockSample = true;
                }
                submitSpans.push_back( SubmitSpan { ev.queue_info_index, ev.sqtt_cb_id, ts0, ts1 } );
                minSubmitStartNs = (std::min)( minSubmitStartNs, ts0 );
                maxSubmitEndNs = (std::max)( maxSubmitEndNs, ts1 );

                char zoneName[128];
                snprintf( zoneName, sizeof( zoneName ), "CmdBuffer Submit #%u", ev.sqtt_cb_id );

                char zoneText[128];
                snprintf( zoneText, sizeof( zoneText ), "Frame %llu, SubIndex %u", (unsigned long long)ev.frame_index, ev.submit_sub_index );

                timeline.emplace_back( tracy::Worker::ImportEventTimeline {
                    pseudoTid,
                    static_cast<uint64_t>( ts0 ),
                    zoneName,
                    zoneText,
                    false,
                    input,
                    0
                } );

                timeline.emplace_back( tracy::Worker::ImportEventTimeline {
                    pseudoTid,
                    static_cast<uint64_t>( ts1 ),
                    "",
                    "",
                    true
                } );
            }
        }
        else if( ev.event_type == SQTT_QUEUE_TIMING_EVENT_PRESENT && ev.gpu_timestamps[0] > 0 )
        {
            const int64_t presentNs = GpuTickToNs( ev.gpu_timestamps[0] );
            messages.emplace_back( tracy::Worker::ImportEventMessages {
                pseudoTid,
                static_cast<uint64_t>( presentNs ),
                "Frame"
            } );
        }
    }
    if( !submitSpans.empty() )
    {
        printf( "  Submit range: %lld..%lld ns (%zu spans)\n",
            (long long)minSubmitStartNs, (long long)maxSubmitEndNs, submitSpans.size() );
    }

    // Decode SQTT Wavefront and Wave Slot Concurrency
    printf( "Decoding SQTT token streams into wave-slot execution grid...\n" );

    tracy::Vector<tracy::GpuComputeUnit> gpuCUs;
    tracy::Vector<tracy::GpuWaveStateSlice> gpuWaveStates;

    const int totalCUs = ( computeUnits > 0 ) ? computeUnits : 40;
    const int maxSlots = ( wavesPerSimd > 0 ) ? wavesPerSimd : 20;

    // Allocate CUs and SIMD slots
    for( int cuIdx = 0; cuIdx < totalCUs; cuIdx++ )
    {
        tracy::GpuComputeUnit cu;
        cu.cuId = uint16_t( cuIdx );
        for( int s = 0; s < 4; s++ )
        {
            for( int sl = 0; sl < maxSlots; sl++ )
            {
                cu.simds[s].slots.push_back( tracy::GpuSimdSlotData {} );
            }
        }
        gpuCUs.push_back( std::move( cu ) );
    }

    // Active wave tracking per (CU, SIMD, Slot)
    struct ActiveWave
    {
        int64_t startNs = 0;
        uint32_t wgid = 0;
        std::vector<tracy::GpuWaveStateSlice> states;
        bool active = false;
    };
    std::unordered_map<uint64_t, ActiveWave> activeWaveMap;

    SqttDecodeStats sqttStats;
    sqttStats.wavesPerSe.resize( std::max( shaderEngines, 0 ) );

    size_t totalDecodedWaves = 0;
    int64_t minWaveStartNs = INT64_MAX;
    int64_t maxWaveEndNs = INT64_MIN;

    for( size_t streamIndex = 0; streamIndex < sqttStreams.size(); streamIndex++ )
    {
        if( !sqttArchSupported ) break;

        const auto& streamPair = sqttStreams[streamIndex];
        const auto& desc = streamPair.first;
        const uint8_t* sData = streamPair.second.first;
        const size_t sBytes  = streamPair.second.second;

        const int seIdx = desc.shader_engine_index;
        const int cuPerSe = totalCUs / std::max( 1, shaderEngines );

        size_t bitpos = 0;
        const size_t totalBits = sBytes * 8;
        uint64_t currentGpuTime = 0;
        auto StreamTickToNs = [&]( uint64_t gpuTick ) -> int64_t {
            return GpuTickToNs( ShaderTickToGpuTick( streamRealtimePoints[streamIndex], gpuTick ) );
        };
        int targetCuId = -1;
        uint32_t targetSimdInCu = 0;
        bool targetLocationValid = false;

        while( bitpos + 4 <= totalBits )
        {
            const uint64_t window = ReadTokenWindow( sData, sBytes, bitpos );

            const uint8_t b8 = window & 0xFF;
            const auto& info = s_tokenTable[b8];

            if( info.type == TOKEN_UNKNOWN )
            {
                sqttStats.unknownTokens++;
                sqttStats.desyncs++;
                bitpos += 4;
                continue;
            }
            if( info.token_len > totalBits - bitpos )
            {
                sqttStats.desyncs++;
                sqttStats.truncatedTokens++;
                break;
            }

            const uint64_t previousGpuTime = currentGpuTime;
            uint32_t timeDelta = 0;
            if( info.type == TOKEN_TIMESTAMP )
            {
                const uint64_t tsType = ( window >> 14 ) & 0x3;
                const uint64_t tsTime = window >> 16;
                if( tsType == 1 )
                {
                    currentGpuTime += tsTime;
                }
            }
            else if( info.time_end > info.time_begin )
            {
                timeDelta = ( window >> info.time_begin ) & ( ( 1 << ( info.time_end - info.time_begin ) ) - 1 );
                currentGpuTime += timeDelta + ( info.type == TOKEN_TIME ? 4 : 0 );
            }

            if( info.type == TOKEN_WAVE_START || info.type == TOKEN_WAVE_START_EXT )
            {
                const uint32_t simd = ( window >> 8 ) & 0x3;
                const uint32_t wgp  = ( window >> 10 ) & 0x7;
                const uint32_t sa   = ( window >> 7 ) & 0x1;
                const uint32_t wid  = ( window >> 13 ) & 0x1F;

                int cuId;
                uint32_t simdInCu;
                if( Gfx10CuLocation( seIdx, shaderEngines, cuPerSe, SQTT_SA_PER_SE, sa, wgp, simd, cuId, simdInCu ) &&
                    cuId < totalCUs && wid < static_cast<uint32_t>( maxSlots ) )
                {
                    const uint64_t key = ( static_cast<uint64_t>( cuId ) << 16 ) | ( simdInCu << 8 ) | wid;
                    ActiveWave& aw = activeWaveMap[key];
                    if( aw.active ) sqttStats.unterminatedWaves++;
                    aw.startNs = StreamTickToNs( currentGpuTime );
                    aw.wgid = 0;
                    aw.states.clear();
                    aw.active = true;
                }
            }
            else if( info.type == TOKEN_WAVE_END )
            {
                const uint32_t simd = ( window >> 9 ) & 0x3;
                const uint32_t wgp  = ( window >> 11 ) & 0x7;
                const uint32_t sa   = ( window >> 8 ) & 0x1;
                const uint32_t wid  = ( window >> 15 ) & 0x1F;

                int cuId;
                uint32_t simdInCu;
                if( Gfx10CuLocation( seIdx, shaderEngines, cuPerSe, SQTT_SA_PER_SE, sa, wgp, simd, cuId, simdInCu ) &&
                    cuId < totalCUs && wid < static_cast<uint32_t>( maxSlots ) )
                {
                    const uint64_t key = ( static_cast<uint64_t>( cuId ) << 16 ) | ( simdInCu << 8 ) | wid;
                    auto it = activeWaveMap.find( key );
                    if( it != activeWaveMap.end() && it->second.active )
                    {
                        const int64_t endNs = StreamTickToNs( currentGpuTime );
                        const int64_t durNs = endNs - it->second.startNs;
                        if( durNs < 0 )
                        {
                            sqttStats.invalidDurationWaves++;
                            it->second.active = false;
                            it->second.states.clear();
                            bitpos += info.token_len;
                            continue;
                        }
                        tracy::GpuWavefront wf {};
                        wf.start = it->second.startNs;
                        wf.end = endNs;
                        minWaveStartNs = (std::min)( minWaveStartNs, wf.start.Val() );
                        maxWaveEndNs = (std::max)( maxWaveEndNs, wf.end.Val() );
                        wf.workgroupX = it->second.wgid;
                        wf.workgroupY = 0;
                        wf.workgroupZ = 0;
                        wf.pipelineHash = !elfObjects.empty() ? ( 0x1000 + ( cuId % elfObjects.size() ) ) : 0;
                        wf.waveId = uint32_t( totalDecodedWaves + 1 );

                        constexpr int64_t submitToleranceNs = 10000;
                        const bool overlapsSubmit = std::any_of( submitSpans.begin(), submitSpans.end(), [&]( const auto& span ) {
                            return wf.start.Val() <= span.end + submitToleranceNs && wf.end.Val() >= span.start - submitToleranceNs;
                        } );
                        if( !overlapsSubmit ) sqttStats.wavesOutsideSubmitSpans++;

                        const size_t stateStart = gpuWaveStates.size();
                        for( auto slice : it->second.states )
                        {
                            const int64_t sliceStart = slice.start.Val();
                            const int64_t sliceEnd = sliceStart + slice.len;
                            const int64_t clippedStart = (std::max)( sliceStart, wf.start.Val() );
                            const int64_t clippedEnd = (std::min)( sliceEnd, wf.end.Val() );
                            if( clippedEnd <= clippedStart ) continue;
                            slice.start = clippedStart;
                            slice.len = static_cast<uint32_t>( clippedEnd - clippedStart );
                            if( slice.start.Val() < wf.start.Val() || slice.start.Val() + slice.len > wf.end.Val() )
                            {
                                sqttStats.slicesOutsideWaves++;
                            }
                            gpuWaveStates.push_back( slice );
                        }
                        wf.stateStartIndex = static_cast<uint32_t>( stateStart );
                        wf.stateCount = static_cast<uint32_t>( gpuWaveStates.size() - stateStart );

                        gpuCUs[cuId].simds[simdInCu].slots[wid].waves.push_back( wf );
                        if( seIdx >= 0 && static_cast<size_t>( seIdx ) < sqttStats.wavesPerSe.size() )
                        {
                            sqttStats.wavesPerSe[seIdx]++;
                        }
                        sqttStats.waveDurations.push_back( durNs );
                        if( wf.stateCount > 0 ) sqttStats.wavesWithStates++;
                        it->second.active = false;
                        it->second.states.clear();
                        totalDecodedWaves++;
                    }
                }
            }
            else if( info.type == TOKEN_VALU || info.type == TOKEN_INST )
            {
                const uint32_t wid = ( window >> ( info.type == TOKEN_INST ? 8 : 7 ) ) & 0x1F;

                if( targetLocationValid && wid < static_cast<uint32_t>( maxSlots ) )
                {
                    const uint64_t key = ( static_cast<uint64_t>( targetCuId ) << 16 ) | ( targetSimdInCu << 8 ) | wid;
                    const auto active = activeWaveMap.find( key );
                    if( active != activeWaveMap.end() && active->second.active )
                    {
                        const int64_t sliceStart = StreamTickToNs( previousGpuTime );
                        const int64_t sliceEnd = StreamTickToNs( currentGpuTime );
                        if( timeDelta > 0 && sliceEnd > sliceStart )
                        {
                            tracy::GpuWaveStateSlice slice {};
                            slice.start = sliceStart;
                            slice.len = static_cast<uint32_t>( sliceEnd - sliceStart );
                            slice.state = ( info.type == TOKEN_VALU ) ? static_cast<uint8_t>( tracy::GpuWaveState::VALU ) : static_cast<uint8_t>( tracy::GpuWaveState::SALU );
                            slice.activeLanes = 32;
                            active->second.states.push_back( slice );
                        }
                    }
                }
            }
            else if( info.type == TOKEN_HEADER )
            {
                const uint32_t simd = ( window >> 13 ) & 0x3;
                const uint32_t wgp = ( window >> 15 ) & 0x7;
                const uint32_t sa = ( window >> 19 ) & 0x1;
                targetLocationValid = Gfx10CuLocation( seIdx, shaderEngines, cuPerSe, SQTT_SA_PER_SE,
                    sa, wgp, simd, targetCuId, targetSimdInCu ) && targetCuId < totalCUs;
            }

            bitpos += info.token_len;
        }

        for( auto& active : activeWaveMap )
        {
            const int cuId = static_cast<int>( active.first >> 16 );
            if( active.second.active && cuId >= seIdx * cuPerSe && cuId < ( seIdx + 1 ) * cuPerSe )
            {
                sqttStats.unterminatedWaves++;
                active.second.active = false;
                active.second.states.clear();
            }
        }
    }

    if( totalDecodedWaves > 0 )
    {
        printf( "  Wave range: %lld..%lld ns\n", (long long)minWaveStartNs, (long long)maxWaveEndNs );
    }

    printf( "Decoded %zu concurrent wavefronts across %d Compute Units.\n", totalDecodedWaves, totalCUs );
    if( printStats ) PrintSqttStats( sqttStats, gpuWaveStates.size() );

    // Process Shader Code Objects (Disassembly & Stats)
    struct TempShader
    {
        uint64_t pipelineHash;
        std::string name;
        uint8_t shaderStage;
        uint16_t vgprCount;
        uint16_t sgprCount;
        uint32_t ldsSize;
        uint32_t maxOccupancy;
        std::vector<std::string> lines;
        std::vector<uint64_t> pcs;
        std::unordered_map<uint64_t, tracy::GpuInstructionStats> stats;
    };
    std::vector<TempShader> tempShaders;
    for( size_t elfIdx = 0; elfIdx < elfObjects.size(); elfIdx++ )
    {
        const uint8_t* elfData = elfObjects[elfIdx].first;
        const size_t elfSz     = elfObjects[elfIdx].second;

        if( elfSz < 64 ) continue;

        const uint64_t e_shoff = *reinterpret_cast<const uint64_t*>( elfData + 40 );
        const uint16_t e_shentsize = *reinterpret_cast<const uint16_t*>( elfData + 58 );
        const uint16_t e_shnum = *reinterpret_cast<const uint16_t*>( elfData + 60 );
        const uint16_t e_shstrndx = *reinterpret_cast<const uint16_t*>( elfData + 62 );

        if( e_shoff + static_cast<size_t>( e_shstrndx + 1 ) * e_shentsize > elfSz ) continue;

        const uint64_t shstrOffset = *reinterpret_cast<const uint64_t*>( elfData + e_shoff + e_shstrndx * e_shentsize + 24 );

        const uint8_t* textData = nullptr;
        size_t textSize = 0;

        for( uint16_t s = 0; s < e_shnum; s++ )
        {
            const size_t secOffset = static_cast<size_t>( e_shoff + s * e_shentsize );
            if( secOffset + 40 > elfSz ) break;

            const uint32_t shName = *reinterpret_cast<const uint32_t*>( elfData + secOffset + 0 );
            const uint64_t shSecOffset = *reinterpret_cast<const uint64_t*>( elfData + secOffset + 24 );
            const uint64_t shSecSize = *reinterpret_cast<const uint64_t*>( elfData + secOffset + 32 );

            if( shstrOffset + shName < elfSz )
            {
                const char* secName = reinterpret_cast<const char*>( elfData + shstrOffset + shName );
                if( strcmp( secName, ".text" ) == 0 && shSecOffset + shSecSize <= elfSz )
                {
                    textData = elfData + shSecOffset;
                    textSize = static_cast<size_t>( shSecSize );
                    break;
                }
            }
        }

        if( textData && textSize > 0 )
        {
            TempShader ts {};
            const uint64_t pipelineHash = 0x1000 + elfIdx;
            ts.pipelineHash = pipelineHash;
            ts.shaderStage = 6; // Compute Stage
            ts.vgprCount = 32;
            ts.sgprCount = 48;
            ts.ldsSize = 0;
            ts.maxOccupancy = static_cast<uint32_t>( maxSlots );

            char shNameBuf[64];
            snprintf( shNameBuf, sizeof( shNameBuf ), "Shader_0x%llx", (unsigned long long)pipelineHash );
            ts.name = shNameBuf;
            // Disassemble instructions
            size_t pc = 0;
            while( pc + 4 <= textSize )
            {
                uint32_t dw0 = *reinterpret_cast<const uint32_t*>( textData + pc );
                uint32_t dw1 = ( pc + 8 <= textSize ) ? *reinterpret_cast<const uint32_t*>( textData + pc + 4 ) : 0;
                bool is64 = false;

                DisasmResult dr = DisassembleAmdGpuInst( dw0, dw1 );
                ts.lines.push_back( std::move( dr.text ) );
                ts.pcs.push_back( pc );

                tracy::GpuInstructionStats st {};
                st.pc = pc;
                st.issueCount = 1; // 1 issue in wave
                st.totalActiveLanes = 32; // 32 active SIMD lanes
                st.category = dr.category;
                if( dr.category == 6 ) st.aluStallCycles = dr.latencyCycles; // Wait
                else if( dr.category == 5 ) st.ldsStallCycles = dr.latencyCycles; // Barrier
                else if( dr.category == 2 ) st.vmemStallCycles = dr.latencyCycles; // VMEM
                else if( dr.category == 3 ) st.smemStallCycles = dr.latencyCycles; // SMEM
                else if( dr.category == 4 ) st.ldsStallCycles = dr.latencyCycles; // LDS
                else st.aluStallCycles = dr.latencyCycles; // VALU / SALU
                ts.stats.emplace( pc, st );

                pc += ( dr.is64Bit ? 8 : 4 );
            }
            printf( "  Disassembled shader ELF %zu: %zu instructions\n", elfIdx, ts.lines.size() );
            tempShaders.push_back( std::move( ts ) );
        }
    }

    // Sort and Rebase Timelines
    std::stable_sort( timeline.begin(), timeline.end(), []( const auto& l, const auto& r ) { return l.timestamp < r.timestamp; } );
    std::stable_sort( messages.begin(), messages.end(), []( const auto& l, const auto& r ) { return l.timestamp < r.timestamp; } );

    uint64_t minTimestamp = UINT64_MAX;
    if( !timeline.empty() ) minTimestamp = (std::min)( minTimestamp, timeline.front().timestamp );
    if( !messages.empty() ) minTimestamp = (std::min)( minTimestamp, messages.front().timestamp );
    for( const auto& cu : gpuCUs )
    {
        for( int s = 0; s < 4; s++ )
        {
            for( const auto& slot : cu.simds[s].slots )
            {
                if( !slot.waves.empty() )
                {
                    minTimestamp = (std::min)( minTimestamp, static_cast<uint64_t>( slot.waves.front().start.Val() ) );
                }
            }
        }
    }

    if( minTimestamp != UINT64_MAX && minTimestamp > 0 )
    {
        for( auto& ev : timeline ) ev.timestamp -= minTimestamp;
        for( auto& msg : messages ) msg.timestamp -= minTimestamp;
        for( auto& cu : gpuCUs )
        {
            for( int s = 0; s < 4; s++ )
            {
                for( auto& slot : cu.simds[s].slots )
                {
                    for( auto& wave : slot.waves )
                    {
                        const int64_t st = wave.start.Val();
                        const int64_t en = wave.end.Val();
                        wave.start = ( st >= static_cast<int64_t>( minTimestamp ) ) ? ( st - minTimestamp ) : 0;
                        wave.end   = ( en >= static_cast<int64_t>( minTimestamp ) ) ? ( en - minTimestamp ) : 0;
                    }
                }
            }
        }
        for( auto& slice : gpuWaveStates )
        {
            const int64_t st = slice.start.Val();
            slice.start = ( st >= static_cast<int64_t>( minTimestamp ) ) ? ( st - minTimestamp ) : 0;
        }
    }

    auto&& getFilename = []( const char* in ) {
        const char* out = in;
        while( *out ) ++out;
        --out;
        while( out > in && ( *out != '/' && *out != '\\' ) ) out--;
        if( *out == '/' || *out == '\\' ) out++;
        return out;
    };

    printf( "Constructing Tracy trace worker with wave slots (%s)...\n", getFilename( output ) );
    tracy::Worker worker( getFilename( output ), gpuName, timeline, messages, plots, threadNames );

    // Feed Native GPU SIMD & Wave Slot Data into Worker
    worker.InsertGpuComputeUnits( std::move( gpuCUs ) );
    worker.InsertGpuWaveStates( std::move( gpuWaveStates ) );
    printf( "Storing %zu shaders into Worker string repository...\n", tempShaders.size() );
    for( auto& ts : tempShaders )
    {
        tracy::GpuShaderData shader {};
        shader.pipelineHash = ts.pipelineHash;
        shader.shaderName = tracy::StringIdx( worker.StoreString( ts.name.c_str(), ts.name.size() ).idx );
        shader.shaderStage = ts.shaderStage;
        shader.vgprCount = ts.vgprCount;
        shader.sgprCount = ts.sgprCount;
        shader.ldsSize = ts.ldsSize;
        shader.maxOccupancy = ts.maxOccupancy;

        for( size_t i = 0; i < ts.lines.size(); i++ )
        {
            const auto& line = ts.lines[i];
            const auto strRef = worker.StoreString( line.c_str(), line.size() );
            shader.isaLines.push_back( tracy::StringIdx( strRef.idx ) );
            shader.isaLinePcs.push_back( ts.pcs[i] );
        }

        for( auto& stPair : ts.stats )
        {
            shader.instructionStats.emplace( stPair.first, stPair.second );
        }

        printf( "  Inserted shader 0x%llx with %zu instructions.\n", (unsigned long long)shader.pipelineHash, shader.isaLines.size() );
        worker.InsertGpuShader( shader.pipelineHash, std::move( shader ) );
    }

    auto w = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open( output, clev ) );
    if( !w )
    {
        fprintf( stderr, "Cannot open output file for writing: %s\n", output );
        return 1;
    }

    printf( "Serializing to %s...\n", output );
    worker.Write( *w, false );

    printf( "Successfully converted RGP capture with wave-slot concurrency to %s!\n", output );
    return 0;
}
