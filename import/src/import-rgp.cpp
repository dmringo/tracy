#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
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
#include "RgpIsa.hpp"
#include "RgpSqtt.hpp"

namespace rgp_sqtt = tracy::rgp_sqtt;
using enum rgp_sqtt::SqttTokenType;
using rgp_sqtt::DecodeMarkers;
using rgp_sqtt::GetInstWaveId;
using rgp_sqtt::GetRealtimeTimestamp;
using rgp_sqtt::GetRegAddress;
using rgp_sqtt::GetTokenTimeDelta;
using rgp_sqtt::GetWaveEndFields;
using rgp_sqtt::GetWaveStartFields;
using rgp_sqtt::ReadToken;
using rgp_sqtt::MarkerDecodeResult;
using rgp_sqtt::SqttMarker;
using rgp_sqtt::SqttArch;
using rgp_sqtt::SqttToken;
using rgp_sqtt::UserdataWord;
using rgp_sqtt::MARKER_BARRIER_END;
using rgp_sqtt::MARKER_BARRIER_START;
using rgp_sqtt::MARKER_BIND_PIPELINE;
using rgp_sqtt::MARKER_CB_END;
using rgp_sqtt::MARKER_CB_START;
using rgp_sqtt::MARKER_EVENT;
using rgp_sqtt::MARKER_GENERAL_API;
using rgp_sqtt::MARKER_LAYOUT_TRANSITION;
using rgp_sqtt::MARKER_USER_EVENT;
using rgp_sqtt::SQ_THREAD_TRACE_USERDATA_2;
using rgp_sqtt::SQ_THREAD_TRACE_USERDATA_3;

// RGP File Format Magic & Constants
static constexpr uint32_t SQTT_FILE_MAGIC_NUMBER  = 0x50303042; // 'B00P'
static constexpr uint32_t SQTT_FILE_HEADER_SIZE   = 56;
static constexpr uint32_t SQTT_GFXIP_LEVEL_GFXIP_10_3 = 0x9;
static constexpr uint32_t SQTT_GFXIP_LEVEL_GFXIP_11_0 = 0xC;
static constexpr uint32_t SQTT_GFXIP_LEVEL_GFXIP_11_5 = 0xD;
static constexpr uint32_t SQTT_GFXIP_LEVEL_GFXIP_12 = 0x10;
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

struct RgpPipelineHash
{
    uint64_t low;
    uint64_t high;

    bool operator==( const RgpPipelineHash& other ) const
    {
        return low == other.low && high == other.high;
    }
};

struct RgpPipelineHashHasher
{
    size_t operator()( const RgpPipelineHash& hash ) const
    {
        return std::hash<uint64_t> {}( hash.low ) ^ ( std::hash<uint64_t> {}( hash.high ) << 1 );
    }
};

struct RgpLoaderEvent
{
    uint32_t type;
    uint64_t baseAddress;
    RgpPipelineHash codeObjectHash;
    uint64_t timestamp;
};

struct RgpStageMetadata
{
    bool present = false;
    uint16_t vgprCount = 0;
    uint16_t sgprCount = 0;
    uint32_t ldsSize = 0;
};

struct RgpElfMetadata
{
    bool hasPipelineHash = false;
    RgpPipelineHash pipelineHash {};
    uint8_t shaderStage = 0xFF;
    uint16_t vgprCount = 0;
    uint16_t sgprCount = 0;
    uint32_t ldsSize = 0;
    std::vector<RgpPipelineHash> apiShaderHashes;
};

struct RgpElfObject
{
    const uint8_t* textData = nullptr;
    size_t textSize = 0;
    RgpElfMetadata metadata;
    std::vector<uint64_t> loadBaseAddresses;
};

struct PendingZone
{
    uint64_t tid;
    int64_t start;
    int64_t end;
    std::string name;
    std::string text;
    uint32_t depth;
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

static bool GfxCuLocation( int se, int shaderEngines, int cuPerSe, int shaderArraysPerSe, int simdsPerCu,
    uint32_t sa, uint32_t wgp, uint32_t simd, int& cuId, uint32_t& simdInCu )
{
    constexpr int cusPerWgp = 2;
    if( se < 0 || se >= shaderEngines || shaderArraysPerSe <= 0 || simdsPerCu <= 0 ||
        cuPerSe % shaderArraysPerSe != 0 ) return false;
    if( sa >= static_cast<uint32_t>( shaderArraysPerSe ) || simd >= cusPerWgp * simdsPerCu ) return false;

    const int cusPerSa = cuPerSe / shaderArraysPerSe;
    if( cusPerSa % cusPerWgp != 0 || wgp >= static_cast<uint32_t>( cusPerSa / cusPerWgp ) ) return false;

    cuId = se * cuPerSe + sa * cusPerSa + wgp * cusPerWgp + simd / simdsPerCu;
    simdInCu = simd % simdsPerCu;
    return true;
}

static bool SqttArchFromGfxip( uint32_t gfxipLevel, SqttArch& arch )
{
    switch( gfxipLevel )
    {
    case SQTT_GFXIP_LEVEL_GFXIP_10_3:
        arch = SqttArch::Gfx10;
        return true;
    case SQTT_GFXIP_LEVEL_GFXIP_11_0:
    case SQTT_GFXIP_LEVEL_GFXIP_11_5:
        arch = SqttArch::Gfx11;
        return true;
    case SQTT_GFXIP_LEVEL_GFXIP_12:
        arch = SqttArch::Gfx12;
        return true;
    default:
        return false;
    }
}

struct MsgpackValue
{
    enum class Kind : uint8_t { Other, Number, String, Array, Map } kind = Kind::Other;
    uint64_t number = 0;
    std::string string;
    std::vector<MsgpackValue> array;
    std::vector<std::pair<std::string, MsgpackValue>> map;
};

class MsgpackReader
{
public:
    MsgpackReader( const uint8_t* data, size_t size )
        : m_data( data )
        , m_size( size )
    {
    }

    bool Read( MsgpackValue& value, unsigned depth = 0 )
    {
        if( depth > 16 || m_offset >= m_size ) return false;
        const uint8_t tag = m_data[m_offset++];

        if( tag <= 0x7F )
        {
            value.kind = MsgpackValue::Kind::Number;
            value.number = tag;
            return true;
        }
        if( tag >= 0xE0 )
        {
            value.kind = MsgpackValue::Kind::Number;
            value.number = static_cast<uint64_t>( static_cast<int64_t>( static_cast<int8_t>( tag ) ) );
            return true;
        }
        if( ( tag & 0xE0 ) == 0xA0 ) return ReadString( tag & 0x1F, value );
        if( ( tag & 0xF0 ) == 0x90 ) return ReadArray( tag & 0x0F, value, depth );
        if( ( tag & 0xF0 ) == 0x80 ) return ReadMap( tag & 0x0F, value, depth );

        uint64_t length = 0;
        switch( tag )
        {
        case 0xC0:
        case 0xC2:
        case 0xC3:
            return true;
        case 0xC4:
            return ReadLength( 1, length ) && Skip( length );
        case 0xC5:
            return ReadLength( 2, length ) && Skip( length );
        case 0xC6:
            return ReadLength( 4, length ) && Skip( length );
        case 0xCA:
            return Skip( 4 );
        case 0xCB:
            return Skip( 8 );
        case 0xCC:
        case 0xD0:
            if( !ReadLength( 1, value.number ) ) return false;
            if( tag == 0xD0 ) value.number = static_cast<uint64_t>( static_cast<int64_t>( static_cast<int8_t>( value.number ) ) );
            value.kind = MsgpackValue::Kind::Number;
            return true;
        case 0xCD:
        case 0xD1:
            if( !ReadLength( 2, value.number ) ) return false;
            if( tag == 0xD1 ) value.number = static_cast<uint64_t>( static_cast<int64_t>( static_cast<int16_t>( value.number ) ) );
            value.kind = MsgpackValue::Kind::Number;
            return true;
        case 0xCE:
        case 0xD2:
            if( !ReadLength( 4, value.number ) ) return false;
            if( tag == 0xD2 ) value.number = static_cast<uint64_t>( static_cast<int64_t>( static_cast<int32_t>( value.number ) ) );
            value.kind = MsgpackValue::Kind::Number;
            return true;
        case 0xCF:
        case 0xD3:
            if( !ReadLength( 8, value.number ) ) return false;
            if( tag == 0xD3 ) value.number = static_cast<uint64_t>( static_cast<int64_t>( value.number ) );
            value.kind = MsgpackValue::Kind::Number;
            return true;
        case 0xD9:
            if( !ReadLength( 1, length ) ) return false;
            return ReadString( length, value );
        case 0xDA:
            if( !ReadLength( 2, length ) ) return false;
            return ReadString( length, value );
        case 0xDB:
            if( !ReadLength( 4, length ) ) return false;
            return ReadString( length, value );
        case 0xDC:
            if( !ReadLength( 2, length ) ) return false;
            return ReadArray( length, value, depth );
        case 0xDD:
            if( !ReadLength( 4, length ) ) return false;
            return ReadArray( length, value, depth );
        case 0xDE:
            if( !ReadLength( 2, length ) ) return false;
            return ReadMap( length, value, depth );
        case 0xDF:
            if( !ReadLength( 4, length ) ) return false;
            return ReadMap( length, value, depth );
        case 0xC7:
            if( !ReadLength( 1, length ) ) return false;
            return Skip( length + 1 );
        case 0xC8:
            if( !ReadLength( 2, length ) ) return false;
            return Skip( length + 1 );
        case 0xC9:
            if( !ReadLength( 4, length ) ) return false;
            return Skip( length + 1 );
        case 0xD4: return Skip( 2 );
        case 0xD5: return Skip( 3 );
        case 0xD6: return Skip( 5 );
        case 0xD7: return Skip( 9 );
        case 0xD8: return Skip( 17 );
        default:
            return false;
        }
    }

private:
    bool ReadLength( size_t bytes, uint64_t& value )
    {
        if( bytes > m_size - m_offset ) return false;
        value = 0;
        for( size_t i = 0; i < bytes; i++ ) value = ( value << 8 ) | m_data[m_offset++];
        return true;
    }

    bool Skip( uint64_t bytes )
    {
        if( bytes > m_size - m_offset ) return false;
        m_offset += static_cast<size_t>( bytes );
        return true;
    }

    bool ReadString( uint64_t length, MsgpackValue& value )
    {
        if( length > m_size - m_offset || length > 1024 * 1024 ) return false;
        value.kind = MsgpackValue::Kind::String;
        value.string.assign( reinterpret_cast<const char*>( m_data + m_offset ), static_cast<size_t>( length ) );
        m_offset += static_cast<size_t>( length );
        return true;
    }

    bool ReadArray( uint64_t count, MsgpackValue& value, unsigned depth )
    {
        if( count > 4096 ) return false;
        value.kind = MsgpackValue::Kind::Array;
        value.array.resize( static_cast<size_t>( count ) );
        for( auto& item : value.array ) if( !Read( item, depth + 1 ) ) return false;
        return true;
    }

    bool ReadMap( uint64_t count, MsgpackValue& value, unsigned depth )
    {
        if( count > 4096 ) return false;
        value.kind = MsgpackValue::Kind::Map;
        value.map.reserve( static_cast<size_t>( count ) );
        for( uint64_t i = 0; i < count; i++ )
        {
            MsgpackValue key;
            MsgpackValue item;
            if( !Read( key, depth + 1 ) || !Read( item, depth + 1 ) ) return false;
            if( key.kind == MsgpackValue::Kind::String ) value.map.emplace_back( std::move( key.string ), std::move( item ) );
        }
        return true;
    }

    const uint8_t* m_data;
    size_t m_size;
    size_t m_offset = 0;
};

static const MsgpackValue* GetMsgpackField( const MsgpackValue& value, const char* name )
{
    if( value.kind != MsgpackValue::Kind::Map ) return nullptr;
    for( const auto& field : value.map ) if( field.first == name ) return &field.second;
    return nullptr;
}

static bool GetMsgpackNumber( const MsgpackValue* value, uint64_t& number )
{
    if( !value || value->kind != MsgpackValue::Kind::Number ) return false;
    number = value->number;
    return true;
}

static int RgpHardwareStage( const std::string& name )
{
    static const char* stages[] = { ".vs", ".ls", ".hs", ".es", ".gs", ".ps", ".cs" };
    for( int i = 0; i < 7; i++ ) if( name == stages[i] ) return i;
    return -1;
}

static bool ParseRgpPipelineMetadata( const MsgpackValue& pipeline, RgpElfMetadata& metadata )
{
    const auto* hash = GetMsgpackField( pipeline, ".internal_pipeline_hash" );
    if( !hash || hash->kind != MsgpackValue::Kind::Array || hash->array.size() < 2 ||
        !GetMsgpackNumber( &hash->array[0], metadata.pipelineHash.low ) ||
        !GetMsgpackNumber( &hash->array[1], metadata.pipelineHash.high ) ) return false;
    metadata.hasPipelineHash = true;

    std::array<RgpStageMetadata, 7> stages {};
    const auto* hardwareStages = GetMsgpackField( pipeline, ".hardware_stages" );
    if( hardwareStages && hardwareStages->kind == MsgpackValue::Kind::Map )
    {
        for( const auto& item : hardwareStages->map )
        {
            const int stage = RgpHardwareStage( item.first );
            if( stage < 0 ) continue;
            auto& data = stages[stage];
            data.present = true;
            uint64_t value = 0;
            if( GetMsgpackNumber( GetMsgpackField( item.second, ".vgpr_count" ), value ) && value <= UINT16_MAX ) data.vgprCount = static_cast<uint16_t>( value );
            if( GetMsgpackNumber( GetMsgpackField( item.second, ".sgpr_count" ), value ) && value <= UINT16_MAX ) data.sgprCount = static_cast<uint16_t>( value );
            if( GetMsgpackNumber( GetMsgpackField( item.second, ".lds_size" ), value ) && value <= UINT32_MAX ) data.ldsSize = static_cast<uint32_t>( value );
        }
    }

    const auto* shaders = GetMsgpackField( pipeline, ".shaders" );
    if( shaders && shaders->kind == MsgpackValue::Kind::Map )
    {
        for( const auto& item : shaders->map )
        {
            const auto* apiHash = GetMsgpackField( item.second, ".api_shader_hash" );
            if( apiHash && apiHash->kind == MsgpackValue::Kind::Array && apiHash->array.size() >= 2 )
            {
                RgpPipelineHash shaderHash {};
                if( GetMsgpackNumber( &apiHash->array[0], shaderHash.low ) &&
                    GetMsgpackNumber( &apiHash->array[1], shaderHash.high ) ) metadata.apiShaderHashes.push_back( shaderHash );
            }

            const auto* mapping = GetMsgpackField( item.second, ".hardware_mapping" );
            if( mapping && mapping->kind == MsgpackValue::Kind::Array && !mapping->array.empty() &&
                mapping->array[0].kind == MsgpackValue::Kind::String )
            {
                const int stage = RgpHardwareStage( mapping->array[0].string );
                if( stage >= 0 ) stages[stage].present = true;
            }
        }
    }

    int stageCount = 0;
    int onlyStage = -1;
    for( int stage = 0; stage < static_cast<int>( stages.size() ); stage++ )
    {
        if( stages[stage].present )
        {
            stageCount++;
            onlyStage = stage;
        }
    }
    if( stageCount == 1 )
    {
        metadata.shaderStage = static_cast<uint8_t>( onlyStage );
        metadata.vgprCount = stages[onlyStage].vgprCount;
        metadata.sgprCount = stages[onlyStage].sgprCount;
        metadata.ldsSize = stages[onlyStage].ldsSize;
    }
    return true;
}

static uint16_t ReadElf16( const uint8_t* data )
{
    uint16_t value;
    memcpy( &value, data, sizeof( value ) );
    return value;
}

static uint32_t ReadElf32( const uint8_t* data )
{
    uint32_t value;
    memcpy( &value, data, sizeof( value ) );
    return value;
}

static uint64_t ReadElf64( const uint8_t* data )
{
    uint64_t value;
    memcpy( &value, data, sizeof( value ) );
    return value;
}

static bool ParseRgpElfObject( const uint8_t* elfData, size_t elfSize, RgpElfObject& object )
{
    if( elfSize < 64 || elfData[4] != 2 || elfData[5] != 1 ) return false;
    const uint64_t sectionOffset = ReadElf64( elfData + 40 );
    const uint16_t sectionEntrySize = ReadElf16( elfData + 58 );
    const uint16_t sectionCount = ReadElf16( elfData + 60 );
    const uint16_t sectionNameIndex = ReadElf16( elfData + 62 );
    if( sectionEntrySize < 64 || sectionCount == 0 || sectionNameIndex >= sectionCount ||
        sectionOffset > elfSize || sectionCount > ( elfSize - sectionOffset ) / sectionEntrySize ) return false;

    const uint8_t* namesHeader = elfData + sectionOffset + sectionNameIndex * sectionEntrySize;
    const uint64_t namesOffset = ReadElf64( namesHeader + 24 );
    const uint64_t namesSize = ReadElf64( namesHeader + 32 );
    if( namesOffset > elfSize || namesSize > elfSize - namesOffset ) return false;

    const uint8_t* noteData = nullptr;
    size_t noteSize = 0;
    for( uint16_t i = 0; i < sectionCount; i++ )
    {
        const uint8_t* section = elfData + sectionOffset + static_cast<size_t>( i ) * sectionEntrySize;
        const uint32_t nameOffset = ReadElf32( section );
        if( nameOffset >= namesSize ) continue;
        const char* name = reinterpret_cast<const char*>( elfData + namesOffset + nameOffset );
        const size_t remaining = static_cast<size_t>( namesSize - nameOffset );
        const void* terminator = memchr( name, '\0', remaining );
        if( !terminator ) continue;
        const uint64_t dataOffset = ReadElf64( section + 24 );
        const uint64_t dataSize = ReadElf64( section + 32 );
        if( dataOffset > elfSize || dataSize > elfSize - dataOffset ) continue;
        if( strcmp( name, ".text" ) == 0 )
        {
            object.textData = elfData + dataOffset;
            object.textSize = static_cast<size_t>( dataSize );
        }
        else if( strcmp( name, ".note" ) == 0 )
        {
            noteData = elfData + dataOffset;
            noteSize = static_cast<size_t>( dataSize );
        }
    }
    if( !noteData || noteSize < 12 ) return false;

    const uint32_t nameSize = ReadElf32( noteData );
    const uint32_t descriptionSize = ReadElf32( noteData + 4 );
    const uint32_t noteType = ReadElf32( noteData + 8 );
    const size_t nameOffset = 12;
    const size_t descriptionOffset = nameOffset + ( ( static_cast<size_t>( nameSize ) + 3 ) & ~size_t( 3 ) );
    if( nameSize == 0 || nameSize > noteSize - nameOffset || descriptionOffset > noteSize ||
        descriptionSize > noteSize - descriptionOffset || noteType != 32 ||
        memcmp( noteData + nameOffset, "AMDGPU", 6 ) != 0 ) return false;

    MsgpackValue root;
    MsgpackReader reader( noteData + descriptionOffset, descriptionSize );
    if( !reader.Read( root ) ) return false;
    const auto* pipelines = GetMsgpackField( root, "amdpal.pipelines" );
    if( !pipelines || pipelines->kind != MsgpackValue::Kind::Array ) return false;
    for( const auto& pipeline : pipelines->array )
    {
        if( ParseRgpPipelineMetadata( pipeline, object.metadata ) ) return object.textData && object.textSize > 0;
    }
    return false;
}

void Usage()
{
    printf( "tracy-import-rgp %i.%i.%i / %s\n\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef );
    printf( "Usage: import-rgp [--stats] [--llvm-objdump <path>] input.rgp output.tracy\n\n" );
    printf( "Imports AMD Radeon GPU Profiler (.rgp) traces into Tracy Profiler format.\n" );
    printf( "Supports:\n" );
    printf( "  - Wave-Slot execution timeline (CU -> SIMD -> Slot multi-wave concurrency)\n" );
    printf( "  - Instruction-level disassembly with stall & execution heatmaps\n" );
    printf( "  - GPU hardware queues (Universal/Graphics, Compute, DMA)\n" );
    printf( "  - Command buffer submissions and synchronization markers\n" );
    printf( "  - Frame presentation marks\n" );
    printf( "  - SPM counter time-series plots (wavefront occupancy / GPU metrics)\n" );
    printf( "  - LLVM AMDGPU disassembly via --llvm-objdump or TRACY_LLVM_OBJDUMP\n" );
    exit( 1 );
}

struct SqttDecodeStats
{
    std::vector<uint64_t> wavesPerSe;
    std::vector<int64_t> waveDurations;
    size_t wavesWithStates = 0;
    size_t wavesAttributed = 0;
    size_t wavesUnattributed = 0;
    size_t wavesOutsideSubmitSpans = 0;
    size_t unknownTokens = 0;
    size_t desyncs = 0;
    size_t truncatedTokens = 0;
    size_t invalidDurationWaves = 0;
    size_t unterminatedWaves = 0;
    size_t slicesOutsideWaves = 0;
    std::map<uint8_t, size_t> unknownTokenPrefixes;
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
    printf( "  waves attributed to a shader pipeline: %zu; unattributed: %zu\n",
        stats.wavesAttributed, stats.wavesUnattributed );
    printf( "  unknown tokens: %zu; desyncs: %zu; truncated tokens: %zu; invalid-duration waves: %zu; unterminated waves: %zu\n",
        stats.unknownTokens, stats.desyncs, stats.truncatedTokens, stats.invalidDurationWaves, stats.unterminatedWaves );
    if( !stats.unknownTokenPrefixes.empty() )
    {
        printf( "  unknown token prefixes:" );
        for( const auto& prefix : stats.unknownTokenPrefixes )
        {
            printf( " 0x%02X(%zu)", prefix.first, prefix.second );
        }
        printf( "\n" );
    }
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

int main( int argc, char** argv )
{
#ifdef _WIN32
    if( !AttachConsole( ATTACH_PARENT_PROCESS ) )
    {
        AllocConsole();
        SetConsoleMode( GetStdHandle( STD_OUTPUT_HANDLE ), 0x07 );
    }
#endif

    tracy::FileCompression clev = tracy::FileCompression::Fast;

    bool printStats = false;
    const char* llvmObjdump = getenv( "TRACY_LLVM_OBJDUMP" );
    int arg = 1;
    while( argc > arg )
    {
        if( strcmp( argv[arg], "--stats" ) == 0 )
        {
            printStats = true;
            arg++;
        }
        else if( strcmp( argv[arg], "--llvm-objdump" ) == 0 )
        {
            if( argc <= arg + 1 ) Usage();
            llvmObjdump = argv[arg + 1];
            arg += 2;
        }
        else
        {
            break;
        }
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
    std::unordered_map<uint64_t, RgpPipelineHash> psoPipelineHashes;
    std::unordered_set<uint64_t> ambiguousPsoHashes;
    std::vector<RgpLoaderEvent> loaderEvents;

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
                    const RgpPipelineHash pipelineHash { rec->pipeline_hash[0], rec->pipeline_hash[1] };
                    const auto existingHash = psoPipelineHashes.find( rec->api_pso_hash );
                    if( existingHash != psoPipelineHashes.end() && !( existingHash->second == pipelineHash ) )
                    {
                        psoPipelineHashes.erase( existingHash );
                        ambiguousPsoHashes.insert( rec->api_pso_hash );
                    }
                    else if( !ambiguousPsoHashes.count( rec->api_pso_hash ) )
                    {
                        psoPipelineHashes.emplace( rec->api_pso_hash, pipelineHash );
                    }
                    if( rec->api_level_obj_name[0] != '\0' )
                    {
                        psoNames[rec->api_pso_hash] = rec->api_level_obj_name;
                    }
                    ptr += ( rsize > 0 ) ? rsize : sizeof( sqtt_pso_correlation_record );
                }
            }
            break;
        }
        case SQTT_FILE_CHUNK_TYPE_CODE_OBJECT_LOADER_EVENTS:
        {
            if( payloadSize >= 16 )
            {
                const uint32_t recordSize = ReadElf32( chunkPayload + 8 );
                const uint32_t recordCount = ReadElf32( chunkPayload + 12 );
                if( recordSize >= 40 && recordCount <= ( payloadSize - 16 ) / recordSize )
                {
                    const uint8_t* ptr = chunkPayload + 16;
                    for( uint32_t i = 0; i < recordCount; i++, ptr += recordSize )
                    {
                        loaderEvents.push_back( RgpLoaderEvent {
                            ReadElf32( ptr ),
                            ReadElf64( ptr + 8 ),
                            RgpPipelineHash { ReadElf64( ptr + 16 ), ReadElf64( ptr + 24 ) },
                            ReadElf64( ptr + 32 )
                        } );
                    }
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

    std::vector<RgpElfObject> shaderObjects;
    std::vector<std::pair<const uint8_t*, size_t>> shaderElfObjects;
    shaderObjects.reserve( elfObjects.size() );
    shaderElfObjects.reserve( elfObjects.size() );
    for( const auto& elf : elfObjects )
    {
        RgpElfObject object;
        if( ParseRgpElfObject( elf.first, elf.second, object ) )
        {
            shaderObjects.push_back( std::move( object ) );
            shaderElfObjects.push_back( elf );
        }
    }

    std::unordered_map<uint64_t, RgpPipelineHash> pipelineHashOwners;
    std::unordered_set<uint64_t> ambiguousPipelineHashes;
    for( const auto& object : shaderObjects )
    {
        const auto& hash = object.metadata.pipelineHash;
        const auto existingHash = pipelineHashOwners.find( hash.low );
        if( existingHash != pipelineHashOwners.end() && !( existingHash->second == hash ) )
        {
            pipelineHashOwners.erase( existingHash );
            ambiguousPipelineHashes.insert( hash.low );
        }
        else if( !ambiguousPipelineHashes.count( hash.low ) )
        {
            pipelineHashOwners.emplace( hash.low, hash );
        }
    }

    std::unordered_map<RgpPipelineHash, uint64_t, RgpPipelineHashHasher> shaderHashByPipeline;
    for( const auto& owner : pipelineHashOwners )
    {
        if( !ambiguousPipelineHashes.count( owner.first ) ) shaderHashByPipeline.emplace( owner.second, owner.first );
    }

    std::unordered_map<RgpPipelineHash, std::vector<const RgpLoaderEvent*>, RgpPipelineHashHasher> loaderEventsByHash;
    for( const auto& event : loaderEvents )
    {
        if( event.type == 0 && event.baseAddress != 0 ) loaderEventsByHash[event.codeObjectHash].push_back( &event );
    }
    size_t matchedLoaderObjects = 0;
    for( auto& object : shaderObjects )
    {
        const auto pipelineEvents = loaderEventsByHash.find( object.metadata.pipelineHash );
        if( pipelineEvents != loaderEventsByHash.end() )
        {
            for( const auto* event : pipelineEvents->second )
            {
                if( std::find( object.loadBaseAddresses.begin(), object.loadBaseAddresses.end(), event->baseAddress ) == object.loadBaseAddresses.end() )
                {
                    object.loadBaseAddresses.push_back( event->baseAddress );
                }
            }
        }
        for( const RgpPipelineHash apiShaderHash : object.metadata.apiShaderHashes )
        {
            const auto events = loaderEventsByHash.find( apiShaderHash );
            if( events == loaderEventsByHash.end() ) continue;
            for( const auto* event : events->second )
            {
                if( std::find( object.loadBaseAddresses.begin(), object.loadBaseAddresses.end(), event->baseAddress ) == object.loadBaseAddresses.end() )
                {
                    object.loadBaseAddresses.push_back( event->baseAddress );
                }
            }
        }
        if( !object.loadBaseAddresses.empty() ) matchedLoaderObjects++;
    }

    printf( "Processed %zu chunks (%zu SQTT streams, %zu shader ELFs).\n", chunkCount, sqttStreams.size(), elfObjects.size() );

    SqttArch sqttArch = SqttArch::Gfx10;
    const bool sqttArchKnown = SqttArchFromGfxip( sqttGfxipLevel, sqttArch );
    const bool sqttArchSupported = sqttArchKnown && simdsPerCu == 2;
    if( sqttArchSupported )
    {
        const char* archName = sqttArch == SqttArch::Gfx10 ? "GFX10.3" :
            sqttArch == SqttArch::Gfx11 ? ( sqttGfxipLevel == SQTT_GFXIP_LEVEL_GFXIP_11_5 ? "GFX11.5" : "GFX11.0" ) :
            "GFX12";
        printf( "  SQTT architecture: %s (%s)\n", archName, gpuName );
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
        uint64_t realtimeEpoch = 0;
        uint64_t previousRealtimeRaw = 0;
        uint64_t previousRealtimeExtended = 0;
        bool hasRealtimeTimestamp = false;
        auto& points = streamRealtimePoints[streamIndex];

        while( bitpos + 4 <= totalBits )
        {
            SqttToken token {};
            if( !ReadToken( sqttArch, streamData, streamBytes, bitpos, token ) ) break;
            const uint64_t window = token.window;
            const auto& info = token.encoding;
            if( info.type == TOKEN_UNKNOWN )
            {
                bitpos += 4;
                continue;
            }

            shaderTimestamp += GetTokenTimeDelta( sqttArch, token );
            uint64_t realtimeTimestamp;
            if( GetRealtimeTimestamp( sqttArch, token, realtimeTimestamp ) )
            {
                bool validRealtimeTimestamp = true;
                if( sqttArch != SqttArch::Gfx10 )
                {
                    const uint8_t timestampWidth = sqttArch == SqttArch::Gfx12 ? 52 : 36;
                    const uint64_t modulus = uint64_t( 1 ) << timestampWidth;
                    const uint64_t timestampMask = modulus - 1;
                    const uint64_t originRaw = sqttGpuOrigin & timestampMask;
                    if( !hasRealtimeTimestamp )
                    {
                        realtimeEpoch = sqttGpuOrigin & ~timestampMask;
                        if( realtimeTimestamp < originRaw && originRaw - realtimeTimestamp > modulus / 2 )
                        {
                            realtimeEpoch += modulus;
                        }
                        else if( realtimeTimestamp > originRaw && realtimeTimestamp - originRaw > modulus / 2 && realtimeEpoch >= modulus )
                        {
                            realtimeEpoch -= modulus;
                        }
                        realtimeTimestamp += realtimeEpoch;
                        previousRealtimeRaw = realtimeTimestamp & timestampMask;
                        previousRealtimeExtended = realtimeTimestamp;
                        hasRealtimeTimestamp = true;
                    }
                    else if( sqttArch == SqttArch::Gfx11 )
                    {
                        uint64_t mergedTimestamp;
                        if( !rgp_sqtt::MergePartialTimestamp( realtimeTimestamp, previousRealtimeExtended, timestampMask,
                                (std::max)( gpuTimestampFreq, uint64_t( 1 ) ), mergedTimestamp ) )
                        {
                            validRealtimeTimestamp = false;
                        }
                        else
                        {
                            realtimeTimestamp = mergedTimestamp;
                            previousRealtimeRaw = realtimeTimestamp & timestampMask;
                            previousRealtimeExtended = realtimeTimestamp;
                        }
                    }
                    else
                    {
                        if( realtimeTimestamp < previousRealtimeRaw && previousRealtimeRaw - realtimeTimestamp > modulus / 2 )
                        {
                            realtimeEpoch += modulus;
                        }
                        previousRealtimeRaw = realtimeTimestamp;
                        realtimeTimestamp += realtimeEpoch;
                        previousRealtimeExtended = realtimeTimestamp;
                    }
                }
                if( validRealtimeTimestamp ) points.push_back( SqttRealtimePoint { shaderTimestamp, realtimeTimestamp } );
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
    for( size_t streamIndex = 0; streamIndex < streamRealtimePoints.size(); streamIndex++ )
    {
        const auto& points = streamRealtimePoints[streamIndex];
        if( !points.empty() )
        {
            printf( "    Stream %zu correlation range: shader %llu..%llu, GPU %llu..%llu ticks\n", streamIndex,
                static_cast<unsigned long long>( points.front().shaderTimestamp ),
                static_cast<unsigned long long>( points.back().shaderTimestamp ),
                static_cast<unsigned long long>( points.front().gpuTimestamp ),
                static_cast<unsigned long long>( points.back().gpuTimestamp ) );
        }
    }

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
    std::vector<PendingZone> pendingZones;

    struct SubmitSpan
    {
        uint32_t queueIndex;
        uint32_t cbId;
        int64_t start;
        int64_t end;
    };
    std::vector<SubmitSpan> submitSpans;
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
                submitSpans.push_back( SubmitSpan { ev.queue_info_index, ev.sqtt_cb_id, ts0, ts1 } );
                minSubmitStartNs = (std::min)( minSubmitStartNs, ts0 );
                maxSubmitEndNs = (std::max)( maxSubmitEndNs, ts1 );

                char zoneName[128];
                snprintf( zoneName, sizeof( zoneName ), "CmdBuffer Submit #%u", ev.sqtt_cb_id );

                char zoneText[128];
                snprintf( zoneText, sizeof( zoneText ), "Frame %llu, SubIndex %u", (unsigned long long)ev.frame_index, ev.submit_sub_index );

                pendingZones.push_back( PendingZone { pseudoTid, ts0, ts1, zoneName, zoneText, 0 } );
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

    std::vector<MarkerDecodeResult> markerStreams( sqttStreams.size() );
    size_t selectedMarkerStream = SIZE_MAX;

    for( size_t streamIndex = 0; streamIndex < sqttStreams.size(); streamIndex++ )
    {
        const auto& streamPair = sqttStreams[streamIndex];
        const uint8_t* streamData = streamPair.second.first;
        const size_t streamBytes = streamPair.second.second;
        const size_t totalBits = streamBytes * 8;
        size_t bitpos = 0;
        uint64_t shaderTimestamp = 0;
        std::vector<UserdataWord> words;

        while( bitpos + 4 <= totalBits )
        {
            SqttToken token {};
            if( !ReadToken( sqttArch, streamData, streamBytes, bitpos, token ) ) break;
            const uint64_t window = token.window;
            const auto& info = token.encoding;
            if( info.type == TOKEN_UNKNOWN )
            {
                bitpos += 4;
                continue;
            }

            shaderTimestamp += GetTokenTimeDelta( sqttArch, token );

            if( info.type == TOKEN_REG )
            {
                const uint16_t address = GetRegAddress( sqttArch, window );
                if( address == SQ_THREAD_TRACE_USERDATA_2 || address == SQ_THREAD_TRACE_USERDATA_3 )
                {
                    words.push_back( UserdataWord { address, static_cast<uint32_t>( window >> 32 ), shaderTimestamp } );
                }
            }
            bitpos += info.token_len;
        }
        markerStreams[streamIndex] = DecodeMarkers( sqttArch, words );

        if( selectedMarkerStream == SIZE_MAX ||
            markerStreams[streamIndex].markers.size() > markerStreams[selectedMarkerStream].markers.size() ||
            ( markerStreams[streamIndex].markers.size() == markerStreams[selectedMarkerStream].markers.size() &&
                markerStreams[streamIndex].errors < markerStreams[selectedMarkerStream].errors ) )
        {
            selectedMarkerStream = streamIndex;
        }
    }

    std::array<size_t, 5> markerZoneCounts {};
    std::map<uint32_t, size_t> markerApiTypeCounts;
    std::map<uint32_t, size_t> generalApiTypeCounts;
    std::unordered_set<uint32_t> markerCommandBufferIds;
    size_t dispatchZonesWithPipeline = 0;
    size_t dispatchZonesWithShader = 0;
    size_t dispatchZonesTotal = 0;
    size_t markerDecodeErrors = 0;
    size_t unmatchedMarkers = 0;
    size_t selectedMarkerCount = 0;
    int selectedMarkerSe = -1;
    size_t drawMarkerCount = 0;
    int64_t firstDrawMarkerTime = 0;
    bool hasFirstDrawMarkerTime = false;
    size_t thirdDrawZoneIndex = SIZE_MAX;
    uint64_t thirdDrawPsoHash = 0;
    std::array<uint32_t, 4> thirdDrawArgs {};
    bool thirdDrawHasArgs = false;
    size_t drawArgumentsPrinted = 0;
    if( printStats )
    {
        static constexpr std::array<const char*, 6> markerNames = {
            "CB_START", "CB_END", "BARRIER_START", "BARRIER_END", "EVENT", "BIND_PIPELINE"
        };
        static constexpr std::array<uint8_t, 6> markerIds = {
            MARKER_CB_START, MARKER_CB_END, MARKER_BARRIER_START, MARKER_BARRIER_END, MARKER_EVENT, MARKER_BIND_PIPELINE
        };
        for( size_t streamIndex = 0; streamIndex < markerStreams.size(); streamIndex++ )
        {
            std::array<std::map<uint32_t, size_t>, markerIds.size()> rawCbIds;
            for( const auto& marker : markerStreams[streamIndex].markers )
            {
                const auto markerType = std::find( markerIds.begin(), markerIds.end(), marker.identifier );
                if( markerType != markerIds.end() )
                {
                    const size_t markerIndex = static_cast<size_t>( markerType - markerIds.begin() );
                    rawCbIds[markerIndex][marker.rawCbId]++;
                }
            }
            printf( "  SE %d raw marker CB IDs:", sqttStreams[streamIndex].first.shader_engine_index );
            for( size_t markerIndex = 0; markerIndex < rawCbIds.size(); markerIndex++ )
            {
                if( rawCbIds[markerIndex].empty() ) continue;
                printf( " %s", markerNames[markerIndex] );
                for( const auto& [rawId, count] : rawCbIds[markerIndex] )
                {
                    const uint32_t cbId = markerIds[markerIndex] == MARKER_BIND_PIPELINE ? rawId :
                        rgp_sqtt::NormalizeCommandBufferId( rawId );
                    printf( " 0x%05X->%u(%zu)", rawId, cbId, count );
                }
            }
            printf( "\n" );
        }
        std::map<uint32_t, size_t> queueCbIds;
        for( const auto& event : queueEvents )
        {
            if( event.event_type == SQTT_QUEUE_TIMING_EVENT_CMDBUF_SUBMIT ) queueCbIds[event.sqtt_cb_id]++;
        }
        printf( "  Queue submit CB IDs (plain):" );
        for( const auto& [cbId, count] : queueCbIds ) printf( " %u(%zu)", cbId, count );
        printf( "\n" );

        for( size_t streamIndex = 0; streamIndex < markerStreams.size(); streamIndex++ )
        {
            size_t bindPipelineMarkers = 0;
            size_t correlatedBindPipelineMarkers = 0;
            for( const auto& marker : markerStreams[streamIndex].markers )
            {
                if( marker.identifier == MARKER_BIND_PIPELINE )
                {
                    bindPipelineMarkers++;
                    if( psoPipelineHashes.count( marker.pipelineHash ) && !ambiguousPsoHashes.count( marker.pipelineHash ) )
                        correlatedBindPipelineMarkers++;
                }
            }
            printf( "  SE %d bind-pipeline markers: %zu (%zu correlated)\n",
                sqttStreams[streamIndex].first.shader_engine_index, bindPipelineMarkers, correlatedBindPipelineMarkers );
        }
    }
    if( selectedMarkerStream != SIZE_MAX )
    {
        const auto& decoded = markerStreams[selectedMarkerStream];
        const auto& streamPoints = streamRealtimePoints[selectedMarkerStream];
        selectedMarkerCount = decoded.markers.size();
        markerDecodeErrors = decoded.errors;
        selectedMarkerSe = sqttStreams[selectedMarkerStream].first.shader_engine_index;
        for( const auto& marker : decoded.markers )
        {
            if( marker.identifier == MARKER_CB_START ) markerCommandBufferIds.insert( marker.cbId );
            else if( marker.identifier == MARKER_EVENT ) markerApiTypeCounts[marker.apiType]++;
            else if( marker.identifier == MARKER_GENERAL_API ) generalApiTypeCounts[marker.apiType]++;
        }
        auto FindSubmitSpan = [&]( uint32_t cbId, int64_t time ) -> const SubmitSpan* {
            const SubmitSpan* match = nullptr;
            int64_t bestDistance = INT64_MAX;
            int64_t bestDuration = INT64_MAX;
            bool bestExactCbId = false;
            for( const auto& span : submitSpans )
            {
                const bool exactCbId = span.cbId == cbId;
                const int64_t distance = time < span.start ? span.start - time : ( time > span.end ? time - span.end : 0 );
                if( distance > 10000 ) continue;
                const int64_t duration = span.end - span.start;
                if( !match || ( exactCbId && !bestExactCbId ) ||
                    ( exactCbId == bestExactCbId && ( distance < bestDistance ||
                        ( distance == bestDistance && duration < bestDuration ) ) ) )
                {
                    match = &span;
                    bestDistance = distance;
                    bestDuration = duration;
                    bestExactCbId = exactCbId;
                }
            }
            return match;
        };
        auto MarkerTimeNs = [&]( const SqttMarker& marker ) {
            return GpuTickToNs( ShaderTickToGpuTick( streamPoints, marker.shaderTimestamp ) );
        };
        if( submitSpans.empty() )
        {
            std::unordered_map<uint32_t, int64_t> openSyntheticSpans;
            int64_t lastMarkerTime = INT64_MIN;
            for( const auto& marker : decoded.markers )
            {
                const int64_t time = MarkerTimeNs( marker );
                lastMarkerTime = (std::max)( lastMarkerTime, time );
                if( marker.identifier == MARKER_CB_START ) openSyntheticSpans[marker.cbId] = time;
                else if( marker.identifier == MARKER_CB_END )
                {
                    const auto start = openSyntheticSpans.find( marker.cbId );
                    if( start != openSyntheticSpans.end() )
                    {
                        submitSpans.push_back( SubmitSpan { 0, marker.cbId, start->second, (std::max)( start->second, time ) } );
                        openSyntheticSpans.erase( start );
                    }
                }
            }
            for( const auto& openSpan : openSyntheticSpans )
            {
                const int64_t end = (std::max)( openSpan.second, lastMarkerTime );
                submitSpans.push_back( SubmitSpan { 0, openSpan.first, openSpan.second, end } );
            }
            if( printStats )
            {
                printf( "  No queue submit spans; synthesized %zu queue-0 command-buffer spans from SQTT markers\n", submitSpans.size() );
            }
        }
        struct OpenMarkerZone
        {
            size_t zoneIndex;
            const SubmitSpan* span;
        };
        std::unordered_map<uint32_t, size_t> openCommandBuffers;
        std::unordered_map<uint32_t, const SubmitSpan*> commandBufferSpans;
        std::unordered_map<uint32_t, OpenMarkerZone> openEvents;
        std::unordered_map<uint32_t, OpenMarkerZone> openBarriers;
        std::unordered_map<uint32_t, std::vector<OpenMarkerZone>> openUserEvents;
        std::unordered_map<uint32_t, uint64_t> boundPipelinesByCb;

        auto AddMarkerZone = [&]( const SubmitSpan& span, int64_t start, int64_t end,
            const std::string& name, const std::string& text, uint32_t depth, size_t kind ) -> size_t {
            start = (std::max)( start, span.start );
            end = (std::min)( end, span.end );
            if( end < start ) end = start;
            pendingZones.push_back( PendingZone { static_cast<uint64_t>( span.queueIndex + 1 ), start, end, name, text, depth } );
            markerZoneCounts[kind]++;
            return pendingZones.size() - 1;
        };
        auto CloseMarkerZone = [&]( OpenMarkerZone openZone, int64_t end ) {
            auto& zone = pendingZones[openZone.zoneIndex];
            zone.end = (std::max)( zone.start, (std::min)( end, openZone.span->end ) );
        };
        auto CloseEvent = [&]( uint32_t cbId, int64_t end ) {
            const auto it = openEvents.find( cbId );
            if( it != openEvents.end() )
            {
                CloseMarkerZone( it->second, end );
                openEvents.erase( it );
            }
        };
        auto EventName = []( uint32_t type ) -> const char* {
            switch( type )
            {
            case 6: return "CmdDispatch";
            case 9: return "CmdCopyImage";
            case 11: return "CmdCopyBufferToImage";
            case 22: return "vkCmdCopyQueryPoolResults";
            case 23: return "CmdRenderPassColorClear";
            case 24: return "CmdRenderPassDepthStencilClear";
            case 26: return "CmdDraw";
            default: return "GPU Event";
            }
        };

        for( const auto& marker : decoded.markers )
        {
            const int64_t time = MarkerTimeNs( marker );
            if( marker.identifier == MARKER_EVENT && marker.apiType == 26 )
            {
                drawMarkerCount++;
                if( !hasFirstDrawMarkerTime )
                {
                    firstDrawMarkerTime = time;
                    hasFirstDrawMarkerTime = true;
                }
            }
            const SubmitSpan* span = FindSubmitSpan( marker.cbId, time );
            if( marker.identifier == MARKER_CB_START ) boundPipelinesByCb.erase( marker.cbId );
            else if( marker.identifier == MARKER_CB_END ) boundPipelinesByCb.erase( marker.cbId );
            else if( marker.identifier == MARKER_BIND_PIPELINE )
            {
                boundPipelinesByCb[marker.cbId] = marker.pipelineHash;
                continue;
            }
            if( marker.identifier == MARKER_CB_START )
            {
                span = FindSubmitSpan( marker.cbId, time );
                if( !span )
                {
                    unmatchedMarkers++;
                    continue;
                }
                char zoneName[96];
                snprintf( zoneName, sizeof( zoneName ), "Command Buffer %u (RGP 0x%05X)", marker.cbId, marker.rawCbId );
                const size_t zoneIndex = AddMarkerZone( *span, time, span->end, zoneName, "", 1, 0 );
                openCommandBuffers[marker.cbId] = zoneIndex;
                commandBufferSpans[marker.cbId] = span;
                continue;
            }

            if( marker.identifier == MARKER_CB_END )
            {
                CloseEvent( marker.cbId, time );
                const auto barrier = openBarriers.find( marker.cbId );
                if( barrier != openBarriers.end() )
                {
                    CloseMarkerZone( barrier->second, time );
                    openBarriers.erase( barrier );
                }
                auto users = openUserEvents.find( marker.cbId );
                if( users != openUserEvents.end() )
                {
                    for( const auto& user : users->second ) CloseMarkerZone( user, time );
                    openUserEvents.erase( users );
                }
                const auto cb = openCommandBuffers.find( marker.cbId );
                if( cb != openCommandBuffers.end() )
                {
                    const SubmitSpan* cbSpan = commandBufferSpans[marker.cbId];
                    pendingZones[cb->second].end = (std::max)( pendingZones[cb->second].start,
                        (std::min)( time, cbSpan->end ) );
                    openCommandBuffers.erase( cb );
                    commandBufferSpans.erase( marker.cbId );
                }
                continue;
            }

            if( !span ) span = commandBufferSpans.count( marker.cbId ) ? commandBufferSpans[marker.cbId] : nullptr;
            if( !span )
            {
                unmatchedMarkers++;
                continue;
            }

            if( marker.identifier == MARKER_EVENT )
            {
                CloseEvent( marker.cbId, time );
                std::string name = EventName( marker.apiType );
                std::string text = "Command " + std::to_string( marker.apiType );
                if( marker.hasApiArgs )
                {
                    char args[64];
                    snprintf( args, sizeof( args ), "(%u,%u,%u,%u)", marker.apiArgs[0], marker.apiArgs[1],
                        marker.apiArgs[2], marker.apiArgs[3] );
                    name += args;
                }
                const auto boundPipeline = boundPipelinesByCb.find( marker.cbId );
                const uint64_t apiPsoHash = boundPipeline == boundPipelinesByCb.end() || boundPipeline->second == 0 ?
                    marker.pipelineHash : boundPipeline->second;
                if( printStats && marker.apiType == 26 && marker.hasApiArgs && drawArgumentsPrinted < 16 )
                {
                    printf( "  Debug draw args #%zu at %lld ns: (%u,%u,%u,%u), marker PSO 0x%016llX, bound PSO 0x%016llX\n",
                        drawArgumentsPrinted + 1, (long long)time, marker.apiArgs[0], marker.apiArgs[1], marker.apiArgs[2],
                        marker.apiArgs[3], (unsigned long long)marker.pipelineHash,
                        (unsigned long long)( boundPipeline == boundPipelinesByCb.end() ? 0 : boundPipeline->second ) );
                    drawArgumentsPrinted++;
                }
                const auto psoName = psoNames.find( apiPsoHash );
                if( psoName != psoNames.end() && !psoName->second.empty() ) name += " - " + psoName->second;
                const bool isDrawOrDispatch = marker.apiType == 6 || marker.apiType == 26;
                if( isDrawOrDispatch )
                {
                    dispatchZonesTotal++;
                    if( apiPsoHash != 0 )
                    {
                        dispatchZonesWithPipeline++;
                        char psoText[40];
                        snprintf( psoText, sizeof( psoText ), ", PSO 0x%016llx", (unsigned long long)apiPsoHash );
                        text += psoText;
                        const auto psoPipeline = psoPipelineHashes.find( apiPsoHash );
                        if( psoPipeline != psoPipelineHashes.end() && !ambiguousPsoHashes.count( apiPsoHash ) )
                        {
                            const auto shaderHash = shaderHashByPipeline.find( psoPipeline->second );
                            if( shaderHash != shaderHashByPipeline.end() && shaderHash->second != 0 )
                            {
                                char pipelineText[48];
                                snprintf( pipelineText, sizeof( pipelineText ), " -> Pipeline 0x%016llx", (unsigned long long)shaderHash->second );
                                text += pipelineText;
                                dispatchZonesWithShader++;
                            }
                        }
                    }
                }
                const size_t zoneIndex = AddMarkerZone( *span, time, span->end, name,
                    text, 2, 1 );
                openEvents[marker.cbId] = OpenMarkerZone { zoneIndex, span };
                if( marker.apiType == 26 && drawMarkerCount == 3 )
                {
                    thirdDrawZoneIndex = zoneIndex;
                    thirdDrawPsoHash = apiPsoHash;
                    thirdDrawArgs = marker.apiArgs;
                    thirdDrawHasArgs = marker.hasApiArgs;
                }
            }
            else if( marker.identifier == MARKER_BARRIER_START )
            {
                CloseEvent( marker.cbId, time );
                const size_t zoneIndex = AddMarkerZone( *span, time, span->end, "Barrier", "", 2, 2 );
                openBarriers[marker.cbId] = OpenMarkerZone { zoneIndex, span };
            }
            else if( marker.identifier == MARKER_BARRIER_END )
            {
                const auto it = openBarriers.find( marker.cbId );
                if( it != openBarriers.end() )
                {
                    CloseMarkerZone( it->second, time );
                    openBarriers.erase( it );
                }
                else
                {
                    AddMarkerZone( *span, time, time + 1, "Barrier End", "", 2, 2 );
                }
            }
            else if( marker.identifier == MARKER_USER_EVENT )
            {
                if( marker.userEventType == 2 )
                {
                    const std::string name = marker.userEventName.empty() ? "User Event" : marker.userEventName;
                    const size_t zoneIndex = AddMarkerZone( *span, time, span->end, name, name, 2, 3 );
                    openUserEvents[marker.cbId].push_back( OpenMarkerZone { zoneIndex, span } );
                }
                else if( marker.userEventType == 1 )
                {
                    auto it = openUserEvents.find( marker.cbId );
                    if( it != openUserEvents.end() && !it->second.empty() )
                    {
                        CloseMarkerZone( it->second.back(), time );
                        it->second.pop_back();
                        if( it->second.empty() ) openUserEvents.erase( it );
                    }
                }
            }
            else if( marker.identifier == MARKER_LAYOUT_TRANSITION )
            {
                AddMarkerZone( *span, time, time + 1, "Layout Transition", "", 2, 4 );
            }
            else if( marker.identifier == MARKER_GENERAL_API )
            {
                const std::string name = "API " + std::to_string( marker.apiType );
                AddMarkerZone( *span, time, time + 1, name, name, 2, 1 );
            }
        }

        for( const auto& openEvent : openEvents ) CloseMarkerZone( openEvent.second, openEvent.second.span->end );
        for( const auto& openBarrier : openBarriers ) CloseMarkerZone( openBarrier.second, openBarrier.second.span->end );
        for( const auto& openUsers : openUserEvents )
            for( const auto& user : openUsers.second ) CloseMarkerZone( user, user.span->end );
        for( const auto& openCb : openCommandBuffers )
        {
            const SubmitSpan* span = commandBufferSpans[openCb.first];
            pendingZones[openCb.second].end = span->end;
        }
    }

    struct OrderedTimelineEvent
    {
        tracy::Worker::ImportEventTimeline event;
        uint32_t depth;
        size_t order;
    };
    std::vector<OrderedTimelineEvent> orderedTimeline;
    orderedTimeline.reserve( pendingZones.size() * 2 );
    size_t timelineOrder = 0;
    for( const auto& zone : pendingZones )
    {
        orderedTimeline.push_back( OrderedTimelineEvent {
            tracy::Worker::ImportEventTimeline { zone.tid, static_cast<uint64_t>( zone.start ), zone.name, zone.text, false, input, 0 },
            zone.depth, timelineOrder++ } );
        orderedTimeline.push_back( OrderedTimelineEvent {
            tracy::Worker::ImportEventTimeline { zone.tid, static_cast<uint64_t>( zone.end ), "", "", true, "", 0 },
            zone.depth, timelineOrder++ } );
    }
    std::stable_sort( orderedTimeline.begin(), orderedTimeline.end(), []( const auto& l, const auto& r ) {
        if( l.event.timestamp != r.event.timestamp ) return l.event.timestamp < r.event.timestamp;
        if( l.event.tid != r.event.tid ) return l.event.tid < r.event.tid;
        if( l.event.isEnd != r.event.isEnd ) return l.event.isEnd;
        if( l.depth != r.depth ) return l.event.isEnd ? l.depth > r.depth : l.depth < r.depth;
        return l.order < r.order;
    } );
    timeline.reserve( orderedTimeline.size() );
    for( auto& ordered : orderedTimeline ) timeline.push_back( std::move( ordered.event ) );

    if( printStats )
    {
        if( selectedMarkerStream != SIZE_MAX )
        {
            printf( "  SQTT markers: SE %d, %zu decoded, %zu decode errors, %zu unmatched to submit spans\n",
                selectedMarkerSe, selectedMarkerCount, markerDecodeErrors, unmatchedMarkers );
        }
        else
        {
            printf( "  SQTT markers: no SQTT streams available\n" );
        }
        printf( "  Marker zones: command buffers %zu, events %zu, barriers %zu, user events %zu, layout transitions %zu\n",
            markerZoneCounts[0], markerZoneCounts[1], markerZoneCounts[2], markerZoneCounts[3], markerZoneCounts[4] );
        printf( "  Command-buffer IDs in SQTT markers: %zu\n", markerCommandBufferIds.size() );
        for( const auto& [apiType, count] : markerApiTypeCounts )
        {
            printf( "  SQTT EVENT apiType %u: %zu\n", apiType, count );
        }
        for( const auto& [apiType, count] : generalApiTypeCounts )
        {
            printf( "  SQTT GENERAL_API apiType %u: %zu\n", apiType, count );
        }
        printf( "  Draw/dispatch zones with bound pipeline: %zu/%zu; resolved shader: %zu/%zu\n",
            dispatchZonesWithPipeline, dispatchZonesTotal, dispatchZonesWithShader, dispatchZonesTotal );
        if( thirdDrawZoneIndex != SIZE_MAX && thirdDrawZoneIndex < pendingZones.size() && hasFirstDrawMarkerTime )
        {
            const auto& zone = pendingZones[thirdDrawZoneIndex];
            printf( "  Third CmdDraw(%u,%u,%u,%u) relative to first CmdDraw: %.3f us, duration %.3f us, PSO 0x%016llX\n",
                thirdDrawHasArgs ? thirdDrawArgs[0] : 0, thirdDrawHasArgs ? thirdDrawArgs[1] : 0,
                thirdDrawHasArgs ? thirdDrawArgs[2] : 0, thirdDrawHasArgs ? thirdDrawArgs[3] : 0,
                static_cast<double>( zone.start - firstDrawMarkerTime ) / 1000.0,
                static_cast<double>( zone.end - zone.start ) / 1000.0,
                static_cast<unsigned long long>( thirdDrawPsoHash ) );
        }
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
        uint64_t pipelineHash = 0;
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

        std::vector<const SqttMarker*> timedMarkers;
        timedMarkers.reserve( markerStreams[streamIndex].markers.size() );
        for( const auto& marker : markerStreams[streamIndex].markers ) timedMarkers.push_back( &marker );
        std::stable_sort( timedMarkers.begin(), timedMarkers.end(), []( const auto* lhs, const auto* rhs ) {
            return lhs->shaderTimestamp < rhs->shaderTimestamp;
        } );
        size_t nextMarker = 0;
        std::unordered_map<uint32_t, uint64_t> commandBufferPipelines;
        std::unordered_set<uint32_t> activeCommandBuffers;
        auto PipelineAtTime = [&]( uint64_t shaderTimestamp ) -> uint64_t {
            while( nextMarker < timedMarkers.size() && timedMarkers[nextMarker]->shaderTimestamp <= shaderTimestamp )
            {
                const auto& marker = *timedMarkers[nextMarker++];
                if( marker.identifier == MARKER_CB_START )
                {
                    activeCommandBuffers.insert( marker.cbId );
                    commandBufferPipelines.erase( marker.cbId );
                }
                else if( marker.identifier == MARKER_CB_END )
                {
                    activeCommandBuffers.erase( marker.cbId );
                    commandBufferPipelines.erase( marker.cbId );
                }
                else if( marker.identifier == MARKER_BIND_PIPELINE )
                {
                    commandBufferPipelines[marker.cbId] = marker.pipelineHash;
                }
            }

            uint64_t pipelineHash = 0;
            bool hasPipeline = false;
            bool ambiguous = false;
            for( const uint32_t cbId : activeCommandBuffers )
            {
                const auto commandBufferPipeline = commandBufferPipelines.find( cbId );
                if( commandBufferPipeline == commandBufferPipelines.end() || commandBufferPipeline->second == 0 )
                {
                    ambiguous = true;
                    continue;
                }
                const auto psoPipeline = psoPipelineHashes.find( commandBufferPipeline->second );
                if( psoPipeline == psoPipelineHashes.end() || ambiguousPsoHashes.count( commandBufferPipeline->second ) )
                {
                    ambiguous = true;
                    continue;
                }
                const auto shaderHash = shaderHashByPipeline.find( psoPipeline->second );
                if( shaderHash == shaderHashByPipeline.end() || shaderHash->second == 0 )
                {
                    ambiguous = true;
                    continue;
                }
                if( hasPipeline && pipelineHash != shaderHash->second ) ambiguous = true;
                pipelineHash = shaderHash->second;
                hasPipeline = true;
            }
            return hasPipeline && !ambiguous ? pipelineHash : 0;
        };

        while( bitpos + 4 <= totalBits )
        {
            SqttToken token {};
            if( !ReadToken( sqttArch, sData, sBytes, bitpos, token ) )
            {
                sqttStats.desyncs++;
                sqttStats.truncatedTokens++;
                break;
            }
            const uint64_t window = token.window;
            const auto& info = token.encoding;

            if( info.type == TOKEN_UNKNOWN )
            {
                sqttStats.unknownTokens++;
                sqttStats.unknownTokenPrefixes[static_cast<uint8_t>( window )]++;
                bitpos += 4;
                continue;
            }
            const uint64_t previousGpuTime = currentGpuTime;
            currentGpuTime += GetTokenTimeDelta( sqttArch, token );

            if( info.type == TOKEN_WAVE_START || info.type == TOKEN_WAVE_START_EXT )
            {
                const auto fields = GetWaveStartFields( sqttArch, window );

                int cuId;
                uint32_t simdInCu;
                if( GfxCuLocation( seIdx, shaderEngines, cuPerSe, SQTT_SA_PER_SE, simdsPerCu,
                    fields.sa, fields.wgp, fields.simd, cuId, simdInCu ) &&
                    cuId < totalCUs && fields.wave < static_cast<uint32_t>( maxSlots ) )
                {
                    const uint64_t key = ( static_cast<uint64_t>( cuId ) << 16 ) | ( simdInCu << 8 ) | fields.wave;
                    ActiveWave& aw = activeWaveMap[key];
                    if( aw.active ) sqttStats.unterminatedWaves++;
                    aw.startNs = StreamTickToNs( currentGpuTime );
                    aw.wgid = 0;
                    aw.pipelineHash = PipelineAtTime( currentGpuTime );
                    aw.states.clear();
                    aw.active = true;
                }
            }
            else if( info.type == TOKEN_WAVE_END )
            {
                const auto fields = GetWaveEndFields( sqttArch, window );

                int cuId;
                uint32_t simdInCu;
                if( GfxCuLocation( seIdx, shaderEngines, cuPerSe, SQTT_SA_PER_SE, simdsPerCu,
                    fields.sa, fields.wgp, fields.simd, cuId, simdInCu ) &&
                    cuId < totalCUs && fields.wave < static_cast<uint32_t>( maxSlots ) )
                {
                    const uint64_t key = ( static_cast<uint64_t>( cuId ) << 16 ) | ( simdInCu << 8 ) | fields.wave;
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
                        wf.pipelineHash = it->second.pipelineHash;
                        if( wf.pipelineHash != 0 ) sqttStats.wavesAttributed++;
                        else sqttStats.wavesUnattributed++;
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

                        gpuCUs[cuId].simds[simdInCu].slots[fields.wave].waves.push_back( wf );
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
                const uint32_t wid = GetInstWaveId( sqttArch, info.type, window );

                if( targetLocationValid && wid < static_cast<uint32_t>( maxSlots ) )
                {
                    const uint64_t key = ( static_cast<uint64_t>( targetCuId ) << 16 ) | ( targetSimdInCu << 8 ) | wid;
                    const auto active = activeWaveMap.find( key );
                    if( active != activeWaveMap.end() && active->second.active )
                    {
                        const int64_t sliceStart = StreamTickToNs( previousGpuTime );
                        const int64_t sliceEnd = StreamTickToNs( currentGpuTime );
                        if( currentGpuTime > previousGpuTime && sliceEnd > sliceStart )
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
                targetLocationValid = GfxCuLocation( seIdx, shaderEngines, cuPerSe, SQTT_SA_PER_SE, simdsPerCu,
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
    std::unordered_set<RgpPipelineHash, RgpPipelineHashHasher> insertedPipelineHashes;
    std::string asicName = gpuName;
    std::transform( asicName.begin(), asicName.end(), asicName.begin(), []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
    std::string mcpu;
    if( asicName.find( "navi22" ) != std::string::npos || asicName.find( "navi 22" ) != std::string::npos || asicName.find( "gfx1031" ) != std::string::npos ) mcpu = "gfx1031";
    else if( asicName.find( "navi10" ) != std::string::npos || asicName.find( "navi 10" ) != std::string::npos || asicName.find( "gfx1010" ) != std::string::npos ) mcpu = "gfx1010";
    else if( asicName.find( "navi31" ) != std::string::npos || asicName.find( "bioshock" ) != std::string::npos || asicName.find( "gfx1100" ) != std::string::npos ) mcpu = "gfx1100";
    else if( asicName.find( "navi48" ) != std::string::npos || asicName.find( "gfx1200" ) != std::string::npos ) mcpu = "gfx1200";
    else if( sqttGfxipLevel == 0x9 ) mcpu = "gfx1030";
    else if( sqttGfxipLevel == 0xA || sqttGfxipLevel == 0xC ) mcpu = "gfx1100";
    else if( sqttGfxipLevel == 0xD ) mcpu = "gfx1150";
    else if( sqttGfxipLevel == 0x10 ) mcpu = "gfx1200";
    else mcpu = "gfx1030";
    for( size_t elfIdx = 0; elfIdx < shaderObjects.size(); elfIdx++ )
    {
        const auto& object = shaderObjects[elfIdx];
        const auto& pipelineHash = object.metadata.pipelineHash;
        if( pipelineHash.low == 0 || ambiguousPipelineHashes.count( pipelineHash.low ) ||
            !shaderHashByPipeline.count( pipelineHash ) || !insertedPipelineHashes.insert( pipelineHash ).second ) continue;

        TempShader ts {};
        ts.pipelineHash = pipelineHash.low;
        ts.shaderStage = object.metadata.shaderStage;
        ts.vgprCount = object.metadata.vgprCount;
        ts.sgprCount = object.metadata.sgprCount;
        ts.ldsSize = object.metadata.ldsSize;
        ts.maxOccupancy = static_cast<uint32_t>( maxSlots );

        for( const auto& psoPipeline : psoPipelineHashes )
        {
            if( ambiguousPsoHashes.count( psoPipeline.first ) || !( psoPipeline.second == pipelineHash ) ) continue;
            const auto psoName = psoNames.find( psoPipeline.first );
            if( psoName != psoNames.end() && !psoName->second.empty() )
            {
                ts.name = psoName->second;
                break;
            }
        }
        if( ts.name.empty() )
        {
            char shaderName[64];
            snprintf( shaderName, sizeof( shaderName ), "Shader_0x%016llx", (unsigned long long)ts.pipelineHash );
            ts.name = shaderName;
        }

        std::vector<tracy::rgp_isa::IsaInst> instructions;
        std::string disasmError;
        const auto& elf = shaderElfObjects[elfIdx];
        if( !tracy::rgp_isa::DisassembleCodeObject( elf.first, elf.second, mcpu.c_str(), llvmObjdump, instructions, disasmError ) )
        {
            fprintf( stderr, "Failed to disassemble shader ELF %zu: %s\n", elfIdx, disasmError.c_str() );
            continue;
        }
        if( !disasmError.empty() )
        {
            fprintf( stderr, "Shader ELF %zu: %s\n", elfIdx, disasmError.c_str() );
        }
        for( const auto& instruction : instructions )
        {
            std::string line = instruction.mnemonic;
            if( !instruction.operands.empty() ) line += " " + instruction.operands;
            ts.lines.push_back( std::move( line ) );
            ts.pcs.push_back( instruction.offset );

            tracy::GpuInstructionStats st {};
            st.pc = instruction.offset;
            st.issueCount = 1; // 1 issue in wave
            st.totalActiveLanes = 32; // 32 active SIMD lanes
            if( instruction.mnemonic == "s_waitcnt" || instruction.mnemonic.find( "s_waitcnt_" ) == 0 )
            {
                st.category = 6;
                st.aluStallCycles = 32;
            }
            else if( instruction.mnemonic == "s_barrier" )
            {
                st.category = 5;
                st.ldsStallCycles = 64;
            }
            else if( instruction.mnemonic.find( "s_branch" ) == 0 || instruction.mnemonic.find( "s_cbranch" ) == 0 )
            {
                st.category = 7;
                st.aluStallCycles = 4;
            }
            else if( instruction.mnemonic.find( "s_load" ) == 0 || instruction.mnemonic.find( "s_buffer_load" ) == 0 )
            {
                st.category = 3;
                st.smemStallCycles = 40;
            }
            else if( instruction.mnemonic.find( "buffer_" ) == 0 || instruction.mnemonic.find( "image_" ) == 0 || instruction.mnemonic.find( "flat_" ) == 0 )
            {
                st.category = 2;
                st.vmemStallCycles = 220;
            }
            else if( instruction.mnemonic.find( "ds_" ) == 0 )
            {
                st.category = 4;
                st.ldsStallCycles = 16;
            }
            else if( instruction.mnemonic.find( "s_" ) == 0 )
            {
                st.category = 1;
                st.aluStallCycles = 1;
            }
            else
            {
                st.category = 0;
                st.aluStallCycles = 4;
            }
            ts.stats.emplace( instruction.offset, st );
        }
        printf( "  Disassembled shader ELF %zu (pipeline 0x%016llx): %zu instructions\n",
            elfIdx, (unsigned long long)ts.pipelineHash, ts.lines.size() );
        tempShaders.push_back( std::move( ts ) );
    }

    if( printStats )
    {
        printf( "  Shader pipeline join: %zu shaders, %zu distinct pipeline hashes, %zu code objects matched to loader events (%zu loader events)\n",
            tempShaders.size(), insertedPipelineHashes.size(), matchedLoaderObjects, loaderEvents.size() );
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
