#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tracy::rgp_sqtt
{

enum class SqttArch : uint8_t
{
    Gfx10,
    Gfx11,
    Gfx12,
};

enum class SqttTokenType : uint8_t
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
    TOKEN_VMEM_EXEC,
    TOKEN_ALU_EXEC,
    TOKEN_PERF,
};

struct TokenEncoding
{
    SqttTokenType type;
    uint8_t token_len;
    uint8_t time_begin;
    uint8_t time_end;
};

struct SqttToken
{
    uint64_t window;
    TokenEncoding encoding;
};

struct WaveTokenFields
{
    uint8_t sa;
    uint8_t simd;
    uint8_t wgp;
    uint8_t wave;
};

TokenEncoding GetTokenEncoding( SqttArch arch, uint8_t prefix );
TokenEncoding GetGfx10TokenEncoding( uint8_t prefix );
uint64_t ReadTokenWindow( const uint8_t* data, size_t bytes, size_t bitpos );
bool ReadToken( SqttArch arch, const uint8_t* data, size_t bytes, size_t bitpos, SqttToken& token );
bool ReadGfx10Token( const uint8_t* data, size_t bytes, size_t bitpos, SqttToken& token );
uint16_t GetRegAddress( SqttArch arch, uint64_t window );
uint16_t GetGfx10RegAddress( uint64_t window );
WaveTokenFields GetWaveStartFields( SqttArch arch, uint64_t window );
WaveTokenFields GetWaveEndFields( SqttArch arch, uint64_t window );
uint8_t GetInstOpCode( SqttArch arch, uint64_t window );
uint32_t NormalizeCommandBufferId( uint32_t encodedId );

inline constexpr uint16_t SQ_THREAD_TRACE_USERDATA_2 = 0x342;
inline constexpr uint16_t SQ_THREAD_TRACE_USERDATA_3 = 0x343;

inline constexpr uint8_t MARKER_EVENT = 0x0;
inline constexpr uint8_t MARKER_CB_START = 0x1;
inline constexpr uint8_t MARKER_CB_END = 0x2;
inline constexpr uint8_t MARKER_BARRIER_START = 0x3;
inline constexpr uint8_t MARKER_BARRIER_END = 0x4;
inline constexpr uint8_t MARKER_USER_EVENT = 0x5;
inline constexpr uint8_t MARKER_GENERAL_API = 0x6;
inline constexpr uint8_t MARKER_LAYOUT_TRANSITION = 0x9;
inline constexpr uint8_t MARKER_BIND_PIPELINE = 0xC;

struct UserdataWord
{
    uint16_t address;
    uint32_t value;
    uint64_t shaderTimestamp;
};

struct SqttMarker
{
    uint64_t shaderTimestamp;
    uint8_t identifier;
    uint32_t cbId;
    uint32_t rawCbId;
    uint32_t apiType;
    uint32_t userEventType;
    uint64_t pipelineHash;
    std::string userEventName;
};

struct MarkerDecodeResult
{
    std::vector<SqttMarker> markers;
    size_t errors = 0;
};

MarkerDecodeResult DecodeMarkers( const std::vector<UserdataWord>& words );
MarkerDecodeResult DecodeMarkers( SqttArch arch, const std::vector<UserdataWord>& words );

}