#include "RgpSqtt.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <utility>

namespace tracy::rgp_sqtt
{

namespace
{

struct TokenPattern
{
    SqttTokenType type;
    uint8_t pattern;
    uint8_t pattern_len;
    uint8_t token_len;
    uint8_t time_begin;
    uint8_t time_end;
};

constexpr TokenPattern gfx10Encodings[] = {
    { SqttTokenType::TOKEN_INST, 0b010, 3, 20, 4, 7 },
    { SqttTokenType::TOKEN_VALU, 0b011, 3, 12, 3, 6 },
    { SqttTokenType::TOKEN_IMM_ONE, 0b1101, 4, 12, 4, 7 },
    { SqttTokenType::TOKEN_IMMED, 0b00100, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_READY, 0b10100, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_NEW_PC, 0b0100001, 7, 64, 8, 11 },
    { SqttTokenType::TOKEN_WAVE_START, 0b01100, 5, 32, 5, 7 },
    { SqttTokenType::TOKEN_WAVE_START_EXT, 0b11100, 5, 48, 5, 7 },
    { SqttTokenType::TOKEN_WAVE_ALLOC, 0b00101, 5, 20, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_END, 0b10101, 5, 20, 5, 8 },
    { SqttTokenType::TOKEN_SHADER_DATA, 0b00110, 5, 52, 5, 8 },
    { SqttTokenType::TOKEN_SHADER_DATA_SHORT, 0b10110, 5, 28, 5, 8 },
    { SqttTokenType::TOKEN_UTIL_COUNTER, 0b0110001, 7, 64, 7, 9 },
    { SqttTokenType::TOKEN_TIME, 0b1000, 4, 8, 4, 8 },
    { SqttTokenType::TOKEN_NOP, 0b0000, 4, 4, 0, 0 },
    { SqttTokenType::TOKEN_NOP, 0b1110, 4, 8, 0, 0 },
    { SqttTokenType::TOKEN_NOP, 0b1111, 4, 8, 0, 0 },
    { SqttTokenType::TOKEN_MISC, 0b1010001, 7, 24, 7, 16 },
    { SqttTokenType::TOKEN_EVENT, 0b01100001, 8, 24, 8, 11 },
    { SqttTokenType::TOKEN_EVENT_SYNC, 0b11100001, 8, 32, 8, 11 },
    { SqttTokenType::TOKEN_REG, 0b1001, 4, 64, 4, 7 },
    { SqttTokenType::TOKEN_REG_INIT, 0b1110001, 7, 64, 7, 10 },
    { SqttTokenType::TOKEN_TIMESTAMP, 0b0000001, 7, 64, 16, 64 },
    { SqttTokenType::TOKEN_HEADER, 0b0010001, 7, 64, 0, 0 },
};

constexpr TokenPattern gfx11Encodings[] = {
    { SqttTokenType::TOKEN_INST, 0b010, 3, 20, 4, 7 },
    { SqttTokenType::TOKEN_VALU, 0b011, 3, 12, 3, 6 },
    { SqttTokenType::TOKEN_IMM_ONE, 0b1101, 4, 12, 4, 7 },
    { SqttTokenType::TOKEN_ALU_EXEC, 0b1110, 4, 8, 4, 6 },
    { SqttTokenType::TOKEN_VMEM_EXEC, 0b1111, 4, 8, 4, 6 },
    { SqttTokenType::TOKEN_IMMED, 0b00100, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_READY, 0b10100, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_START, 0b01100, 5, 32, 5, 7 },
    { SqttTokenType::TOKEN_SHADER_DATA_SHORT, 0b11100, 5, 48, 5, 7 },
    { SqttTokenType::TOKEN_WAVE_ALLOC, 0b00101, 5, 20, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_END, 0b10101, 5, 20, 5, 8 },
    { SqttTokenType::TOKEN_SHADER_DATA, 0b00110, 5, 52, 5, 8 },
    { SqttTokenType::TOKEN_PERF, 0b10110, 5, 28, 5, 8 },
    { SqttTokenType::TOKEN_NEW_PC, 0b0100001, 7, 72, 8, 11 },
    { SqttTokenType::TOKEN_UTIL_COUNTER, 0b0110001, 7, 48, 7, 9 },
    { SqttTokenType::TOKEN_MISC, 0b1010001, 7, 24, 7, 16 },
    { SqttTokenType::TOKEN_REG_INIT, 0b1110001, 7, 64, 7, 10 },
    { SqttTokenType::TOKEN_TIMESTAMP, 0b0000001, 7, 48, 12, 48 },
    { SqttTokenType::TOKEN_HEADER, 0b0010001, 7, 64, 0, 0 },
    { SqttTokenType::TOKEN_TIME, 0b1000, 4, 8, 4, 8 },
    { SqttTokenType::TOKEN_NOP, 0b0000, 4, 4, 0, 0 },
    { SqttTokenType::TOKEN_EVENT, 0b01100001, 8, 24, 8, 11 },
    { SqttTokenType::TOKEN_EVENT_SYNC, 0b11100001, 8, 32, 8, 11 },
    { SqttTokenType::TOKEN_REG, 0b1001, 4, 64, 4, 7 },
};

constexpr TokenPattern gfx12Encodings[] = {
    { SqttTokenType::TOKEN_INST, 0b010, 3, 20, 3, 6 },
    { SqttTokenType::TOKEN_VALU, 0b011, 3, 12, 3, 6 },
    { SqttTokenType::TOKEN_IMM_ONE, 0b1101, 4, 12, 4, 7 },
    { SqttTokenType::TOKEN_ALU_EXEC, 0b1110, 4, 8, 4, 6 },
    { SqttTokenType::TOKEN_VMEM_EXEC, 0b1111, 4, 8, 4, 6 },
    { SqttTokenType::TOKEN_IMMED, 0b00100, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_READY, 0b10100, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_START, 0b01100, 5, 32, 5, 7 },
    { SqttTokenType::TOKEN_SHADER_DATA_SHORT, 0b11100, 5, 40, 5, 7 },
    { SqttTokenType::TOKEN_WAVE_ALLOC, 0b00101, 5, 24, 5, 8 },
    { SqttTokenType::TOKEN_WAVE_END, 0b10101, 5, 20, 5, 8 },
    { SqttTokenType::TOKEN_SHADER_DATA, 0b00110, 5, 56, 7, 10 },
    { SqttTokenType::TOKEN_PERF, 0b10110, 5, 32, 7, 10 },
    { SqttTokenType::TOKEN_NEW_PC, 0b0100001, 7, 72, 8, 11 },
    { SqttTokenType::TOKEN_UTIL_COUNTER, 0b0110001, 7, 48, 7, 9 },
    { SqttTokenType::TOKEN_MISC, 0b1010001, 7, 24, 7, 16 },
    { SqttTokenType::TOKEN_REG_INIT, 0b1110001, 7, 64, 7, 10 },
    { SqttTokenType::TOKEN_TIMESTAMP, 0b0000001, 7, 64, 12, 64 },
    { SqttTokenType::TOKEN_HEADER, 0b0010001, 7, 64, 0, 0 },
    { SqttTokenType::TOKEN_TIME, 0b1000, 4, 8, 4, 8 },
    { SqttTokenType::TOKEN_NOP, 0b0000, 4, 4, 0, 0 },
    { SqttTokenType::TOKEN_EVENT, 0b01100001, 8, 24, 8, 11 },
    { SqttTokenType::TOKEN_EVENT_SYNC, 0b11100001, 8, 32, 8, 11 },
    { SqttTokenType::TOKEN_REG, 0b1001, 4, 64, 4, 7 },
};

template<size_t N>
std::array<TokenEncoding, 256> BuildTokenTable( const TokenPattern ( &encodings )[N] )
{
    std::array<TokenEncoding, 256> result {};
    result.fill( TokenEncoding { SqttTokenType::TOKEN_UNKNOWN, 4, 0, 0 } );
    for( const auto& encoding : encodings )
    {
        const int step = 1 << encoding.pattern_len;
        for( int prefix = encoding.pattern; prefix < 256; prefix += step )
        {
            result[prefix] = TokenEncoding { encoding.type, encoding.token_len, encoding.time_begin, encoding.time_end };
        }
    }
    return result;
}

const std::array<TokenEncoding, 256>& GetGfx10TokenTable()
{
    static const auto table = [] {
        std::array<TokenEncoding, 256> result {};
        result.fill( TokenEncoding { SqttTokenType::TOKEN_UNKNOWN, 4, 0, 0 } );
        for( const auto& encoding : gfx10Encodings )
        {
            const int step = 1 << encoding.pattern_len;
            for( int prefix = encoding.pattern; prefix < 256; prefix += step )
            {
                result[prefix] = TokenEncoding { encoding.type, encoding.token_len, encoding.time_begin, encoding.time_end };
            }
        }
        return result;
    }();
    return table;
}

const std::array<TokenEncoding, 256>& GetGfx11TokenTable()
{
    static const auto table = BuildTokenTable( gfx11Encodings );
    return table;
}

const std::array<TokenEncoding, 256>& GetGfx12TokenTable()
{
    static const auto table = BuildTokenTable( gfx12Encodings );
    return table;
}

}

TokenEncoding GetTokenEncoding( SqttArch arch, uint8_t prefix )
{
    switch( arch )
    {
    case SqttArch::Gfx10: return GetGfx10TokenEncoding( prefix );
    case SqttArch::Gfx11: return GetGfx11TokenTable()[prefix];
    case SqttArch::Gfx12: return GetGfx12TokenTable()[prefix];
    }
    return TokenEncoding { SqttTokenType::TOKEN_UNKNOWN, 4, 0, 0 };
}

TokenEncoding GetGfx10TokenEncoding( uint8_t prefix )
{
    return GetGfx10TokenTable()[prefix];
}

uint64_t ReadTokenWindow( const uint8_t* data, size_t bytes, size_t bitpos )
{
    const size_t byteIndex = bitpos / 8;
    const size_t bitOffset = bitpos % 8;
    if( !data || byteIndex >= bytes ) return 0;

    uint64_t window = 0;
    const size_t bytesToCopy = (std::min)( sizeof( window ), bytes - byteIndex );
    for( size_t i = 0; i < bytesToCopy; i++ )
    {
        window |= uint64_t( data[byteIndex + i] ) << ( i * 8 );
    }
    window >>= bitOffset;
    if( bitOffset && bytes - byteIndex > sizeof( window ) )
    {
        window |= uint64_t( data[byteIndex + sizeof( window )] ) << ( 64 - bitOffset );
    }
    return window;
}

bool ReadGfx10Token( const uint8_t* data, size_t bytes, size_t bitpos, SqttToken& token )
{
    if( !data || bytes > std::numeric_limits<size_t>::max() / 8 ) return false;
    const size_t totalBits = bytes * 8;
    if( bitpos > totalBits || totalBits - bitpos < 4 ) return false;

    const uint64_t window = ReadTokenWindow( data, bytes, bitpos );
    const auto encoding = GetGfx10TokenEncoding( static_cast<uint8_t>( window ) );
    if( encoding.token_len > totalBits - bitpos ) return false;

    token = SqttToken { window, encoding };
    return true;
}

bool ReadToken( SqttArch arch, const uint8_t* data, size_t bytes, size_t bitpos, SqttToken& token )
{
    if( arch == SqttArch::Gfx10 ) return ReadGfx10Token( data, bytes, bitpos, token );
    if( !data || bytes > std::numeric_limits<size_t>::max() / 8 ) return false;
    const size_t totalBits = bytes * 8;
    if( bitpos > totalBits || totalBits - bitpos < 4 ) return false;

    const uint64_t window = ReadTokenWindow( data, bytes, bitpos );
    const auto encoding = GetTokenEncoding( arch, static_cast<uint8_t>( window ) );
    if( encoding.token_len > totalBits - bitpos ) return false;

    token = SqttToken { window, encoding };
    return true;
}

uint16_t GetGfx10RegAddress( uint64_t window )
{
    return static_cast<uint16_t>( ( window >> 16 ) & 0xFFF );
}

uint16_t GetRegAddress( SqttArch, uint64_t window )
{
    return GetGfx10RegAddress( window );
}

WaveTokenFields GetWaveStartFields( SqttArch arch, uint64_t window )
{
    const bool gfx12 = arch == SqttArch::Gfx12;
    return WaveTokenFields {
        static_cast<uint8_t>( ( window >> 7 ) & 0x1 ),
        static_cast<uint8_t>( ( window >> 8 ) & 0x3 ),
        static_cast<uint8_t>( ( window >> 10 ) & ( gfx12 ? 0xF : 0x7 ) ),
        static_cast<uint8_t>( ( window >> ( gfx12 ? 15 : 13 ) ) & 0x1F ),
    };
}

WaveTokenFields GetWaveEndFields( SqttArch arch, uint64_t window )
{
    const bool gfx12 = arch == SqttArch::Gfx12;
    return WaveTokenFields {
        static_cast<uint8_t>( ( window >> 8 ) & 0x1 ),
        static_cast<uint8_t>( ( window >> 9 ) & 0x3 ),
        static_cast<uint8_t>( ( window >> 11 ) & ( gfx12 ? 0xF : 0x7 ) ),
        static_cast<uint8_t>( ( window >> 15 ) & 0x1F ),
    };
}

uint8_t GetInstOpCode( SqttArch arch, uint64_t window )
{
    return static_cast<uint8_t>( ( window >> ( arch == SqttArch::Gfx12 ? 12 : 13 ) ) & ( arch == SqttArch::Gfx12 ? 0xFF : 0x7F ) );
}

uint32_t NormalizeCommandBufferId( uint32_t encodedId )
{
    return ( encodedId & 1 ) ? ( encodedId >> 8 ) & 0xFFF : ( encodedId >> 1 ) & 0x7FFFF;
}

MarkerDecodeResult DecodeMarkers( const std::vector<UserdataWord>& words )
{
    return DecodeMarkers( SqttArch::Gfx10, words );
}

MarkerDecodeResult DecodeMarkers( SqttArch arch, const std::vector<UserdataWord>& words )
{
    static constexpr std::array<std::array<uint16_t, 2>, 3> markerAddresses = {{
        { SQ_THREAD_TRACE_USERDATA_2, SQ_THREAD_TRACE_USERDATA_3 },
        { SQ_THREAD_TRACE_USERDATA_2, SQ_THREAD_TRACE_USERDATA_3 },
        { SQ_THREAD_TRACE_USERDATA_2, SQ_THREAD_TRACE_USERDATA_3 },
    }};
    const size_t archIndex = static_cast<size_t>( arch );
    const auto& addresses = markerAddresses[archIndex < markerAddresses.size() ? archIndex : 0];
    MarkerDecodeResult result;
    uint32_t currentCbId = 0;
    uint32_t currentRawCbId = 0;
    std::unordered_map<uint32_t, uint64_t> pipelineByCb;
    size_t wordIndex = 0;
    while( wordIndex < words.size() )
    {
        if( words[wordIndex].address != addresses[0] )
        {
            wordIndex++;
            continue;
        }

        const uint32_t header = words[wordIndex].value;
        const uint8_t identifier = static_cast<uint8_t>( header & 0xF );
        if( identifier > MARKER_BIND_PIPELINE )
        {
            result.errors++;
            wordIndex++;
            continue;
        }

        size_t wordCount = 1 + ( ( header >> 4 ) & 0x7 );
        uint32_t userEventType = UINT32_MAX;
        switch( identifier )
        {
        case MARKER_CB_START: wordCount = 4; break;
        case MARKER_CB_END: wordCount = 3; break;
        case MARKER_EVENT: wordCount = ( ( header >> 31 ) & 1 ) ? 6 : 3; break;
        case MARKER_BARRIER_START:
        case MARKER_BARRIER_END:
        case MARKER_LAYOUT_TRANSITION: wordCount = 2; break;
        case MARKER_BIND_PIPELINE: wordCount = 3; break;
        case MARKER_GENERAL_API: wordCount = 1; break;
        case MARKER_USER_EVENT:
        {
            userEventType = ( header >> 12 ) & 0xFF;
            if( userEventType == 2 || userEventType == 3 )
            {
                if( wordIndex + 1 >= words.size() )
                {
                    result.errors++;
                    wordIndex++;
                    continue;
                }
                const uint32_t stringLength = words[wordIndex + 1].value;
                if( stringLength > 1024 * 1024 )
                {
                    result.errors++;
                    wordIndex++;
                    continue;
                }
                wordCount = 2 + ( stringLength + 3 ) / 4;
            }
            else
            {
                wordCount = 1;
            }
            break;
        }
        default: break;
        }
        if( wordCount > words.size() - wordIndex )
        {
            result.errors++;
            wordIndex++;
            continue;
        }

        SqttMarker marker {};
        marker.shaderTimestamp = words[wordIndex].shaderTimestamp;
        marker.identifier = identifier;
        marker.userEventType = userEventType;
        marker.cbId = currentCbId;
        marker.rawCbId = currentRawCbId;

        switch( identifier )
        {
        case MARKER_CB_START:
            marker.rawCbId = ( header >> 7 ) & 0xFFFFF;
            marker.cbId = NormalizeCommandBufferId( marker.rawCbId );
            currentCbId = marker.cbId;
            currentRawCbId = marker.rawCbId;
            break;
        case MARKER_CB_END:
        case MARKER_BARRIER_START:
        case MARKER_BARRIER_END:
            marker.rawCbId = ( header >> 7 ) & 0xFFFFF;
            marker.cbId = NormalizeCommandBufferId( marker.rawCbId );
            break;
        case MARKER_EVENT:
            marker.apiType = ( header >> 7 ) & 0xFFFFFF;
            marker.rawCbId = words[wordIndex + 1].value & 0xFFFFF;
            marker.cbId = NormalizeCommandBufferId( marker.rawCbId );
            marker.pipelineHash = pipelineByCb[marker.cbId];
            break;
        case MARKER_GENERAL_API:
            marker.apiType = ( header >> 7 ) & 0xFFFFF;
            marker.pipelineHash = pipelineByCb[marker.cbId];
            break;
        case MARKER_BIND_PIPELINE:
            marker.rawCbId = ( header >> 9 ) & 0xFFFFF;
            marker.cbId = marker.rawCbId;
            marker.pipelineHash = uint64_t( words[wordIndex + 1].value ) |
                ( uint64_t( words[wordIndex + 2].value ) << 32 );
            pipelineByCb[marker.cbId] = marker.pipelineHash;
            break;
        case MARKER_USER_EVENT:
            if( userEventType == 2 || userEventType == 3 )
            {
                const uint32_t stringLength = words[wordIndex + 1].value;
                marker.userEventName.reserve( stringLength );
                for( size_t byteIndex = 0; byteIndex < stringLength; byteIndex++ )
                {
                    const uint32_t value = words[wordIndex + 2 + byteIndex / 4].value;
                    marker.userEventName.push_back( static_cast<char>( ( value >> ( ( byteIndex % 4 ) * 8 ) ) & 0xFF ) );
                }
                while( !marker.userEventName.empty() && marker.userEventName.back() == '\0' ) marker.userEventName.pop_back();
            }
            break;
        default:
            break;
        }

        result.markers.push_back( std::move( marker ) );
        if( identifier == MARKER_CB_END )
        {
            currentCbId = 0;
            currentRawCbId = 0;
        }
        wordIndex += wordCount;
    }
    return result;
}

}