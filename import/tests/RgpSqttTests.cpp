#include "../src/RgpSqtt.hpp"
#include "data/RgpSqttReferenceData.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

using namespace tracy::rgp_sqtt;

static bool Check( bool condition, const char* message )
{
    if( condition ) return true;
    fprintf( stderr, "FAILED: %s\n", message );
    return false;
}

static std::vector<uint8_t> MakeToken( uint8_t prefix, size_t bits )
{
    std::vector<uint8_t> bytes( ( bits + 7 ) / 8 );
    bytes[0] = prefix;
    return bytes;
}

static void AddWord( std::vector<UserdataWord>& words, uint16_t address, uint32_t value, uint64_t tick )
{
    words.push_back( UserdataWord { address, value, tick } );
}

static bool TestTokens()
{
    bool ok = true;
    const struct
    {
        uint8_t prefix;
        SqttTokenType type;
        uint8_t length;
    } cases[] = {
        { 0x01, SqttTokenType::TOKEN_TIMESTAMP, 64 },
        { 0x0C, SqttTokenType::TOKEN_WAVE_START, 32 },
        { 0x15, SqttTokenType::TOKEN_WAVE_END, 20 },
        { 0x09, SqttTokenType::TOKEN_REG, 64 },
    };

    for( const auto& test : cases )
    {
        auto bytes = MakeToken( test.prefix, test.length );
        SqttToken token {};
        ok &= Check( ReadGfx10Token( bytes.data(), bytes.size(), 0, token ), "read complete GFX10 token" );
        ok &= Check( token.encoding.type == test.type, "classify GFX10 token prefix" );
        ok &= Check( token.encoding.token_len == test.length, "report GFX10 token length" );
    }

    uint64_t regWindow = 0x09 | ( uint64_t( 0xA5B ) << 16 ) | ( uint64_t( 0xDEADBEEF ) << 32 );
    auto regBytes = MakeToken( 0x09, 64 );
    for( size_t i = 0; i < regBytes.size(); i++ ) regBytes[i] = static_cast<uint8_t>( regWindow >> ( i * 8 ) );
    SqttToken regToken {};
    ok &= Check( ReadGfx10Token( regBytes.data(), regBytes.size(), 0, regToken ), "read REG token" );
    ok &= Check( GetGfx10RegAddress( regToken.window ) == 0xA5B, "extract REG address bits 16..27" );

    const uint8_t unaligned[] = { 0xF0, 0x0F };
    ok &= Check( ReadTokenWindow( unaligned, sizeof( unaligned ), 4 ) == 0xFF, "read unaligned bit window" );

    auto truncated = MakeToken( 0x01, 56 );
    SqttToken token {};
    ok &= Check( !ReadGfx10Token( truncated.data(), truncated.size(), 0, token ), "reject truncated timestamp token" );
    const uint8_t garbage[] = { 0x07 };
    ok &= Check( ReadGfx10Token( garbage, sizeof( garbage ), 0, token ), "read unknown token prefix safely" );
    ok &= Check( token.encoding.type == SqttTokenType::TOKEN_UNKNOWN, "classify garbage prefix as unknown" );
    ok &= Check( !ReadGfx10Token( nullptr, 0, 0, token ), "reject null token input" );

    for( unsigned int prefix = 0; prefix < 256; prefix++ )
    {
        const auto legacyEncoding = GetGfx10TokenEncoding( static_cast<uint8_t>( prefix ) );
        const auto archEncoding = GetTokenEncoding( SqttArch::Gfx10, static_cast<uint8_t>( prefix ) );
        ok &= Check( archEncoding.type == legacyEncoding.type && archEncoding.token_len == legacyEncoding.token_len &&
            archEncoding.time_begin == legacyEncoding.time_begin && archEncoding.time_end == legacyEncoding.time_end,
            "preserve GFX10 token encoding through architecture API" );

        auto bytes = MakeToken( static_cast<uint8_t>( prefix ), legacyEncoding.token_len );
        SqttToken legacyToken {};
        SqttToken archToken {};
        const bool legacyRead = ReadGfx10Token( bytes.data(), bytes.size(), 0, legacyToken );
        const bool archRead = ReadToken( SqttArch::Gfx10, bytes.data(), bytes.size(), 0, archToken );
        ok &= Check( archRead == legacyRead, "preserve GFX10 token read result through architecture API" );
        if( legacyRead && archRead )
        {
            ok &= Check( archToken.window == legacyToken.window && archToken.encoding.type == legacyToken.encoding.type &&
                archToken.encoding.token_len == legacyToken.encoding.token_len &&
                archToken.encoding.time_begin == legacyToken.encoding.time_begin &&
                archToken.encoding.time_end == legacyToken.encoding.time_end,
                "preserve GFX10 decoded token through architecture API" );
        }
    }
    return ok;
}

static bool TestArchTokens()
{
    bool ok = true;
    const struct
    {
        SqttArch arch;
        uint8_t prefix;
        SqttTokenType type;
        uint8_t length;
    } cases[] = {
        { SqttArch::Gfx11, 0x02, SqttTokenType::TOKEN_INST, 20 },
        { SqttArch::Gfx11, 0x0E, SqttTokenType::TOKEN_ALU_EXEC, 8 },
        { SqttArch::Gfx11, 0x0F, SqttTokenType::TOKEN_VMEM_EXEC, 8 },
        { SqttArch::Gfx11, 0x04, SqttTokenType::TOKEN_IMMED, 24 },
        { SqttArch::Gfx11, 0x21, SqttTokenType::TOKEN_NEW_PC, 72 },
        { SqttArch::Gfx11, 0x1C, SqttTokenType::TOKEN_SHADER_DATA_SHORT, 48 },
        { SqttArch::Gfx11, 0x05, SqttTokenType::TOKEN_WAVE_ALLOC, 20 },
        { SqttArch::Gfx11, 0x06, SqttTokenType::TOKEN_SHADER_DATA, 52 },
        { SqttArch::Gfx11, 0x16, SqttTokenType::TOKEN_PERF, 28 },
        { SqttArch::Gfx11, 0x01, SqttTokenType::TOKEN_TIMESTAMP, 48 },
        { SqttArch::Gfx12, 0x02, SqttTokenType::TOKEN_INST, 20 },
        { SqttArch::Gfx12, 0x1C, SqttTokenType::TOKEN_SHADER_DATA_SHORT, 40 },
        { SqttArch::Gfx12, 0x05, SqttTokenType::TOKEN_WAVE_ALLOC, 24 },
        { SqttArch::Gfx12, 0x06, SqttTokenType::TOKEN_SHADER_DATA, 56 },
        { SqttArch::Gfx12, 0x16, SqttTokenType::TOKEN_PERF, 32 },
        { SqttArch::Gfx12, 0x01, SqttTokenType::TOKEN_TIMESTAMP, 64 },
        { SqttArch::Gfx12, 0x09, SqttTokenType::TOKEN_REG, 64 },
    };

    for( const auto& test : cases )
    {
        auto bytes = MakeToken( test.prefix, test.length );
        SqttToken token {};
        ok &= Check( ReadToken( test.arch, bytes.data(), bytes.size(), 0, token ), "read complete architecture token" );
        ok &= Check( token.encoding.type == test.type, "classify architecture token prefix" );
        ok &= Check( token.encoding.token_len == test.length, "report architecture token length" );
    }

    auto truncated = MakeToken( 0x02, 16 );
    SqttToken token {};
    ok &= Check( !ReadToken( SqttArch::Gfx11, truncated.data(), truncated.size(), 0, token ), "reject truncated GFX11 instruction token" );
    ok &= Check( GetTokenEncoding( SqttArch::Gfx12, 0x06 ).token_len == 56, "select GFX12 token table" );

    const uint64_t gfx11Start = 0x0C | ( uint64_t( 1 ) << 7 ) | ( uint64_t( 2 ) << 8 ) |
        ( uint64_t( 5 ) << 10 ) | ( uint64_t( 0x13 ) << 13 );
    const auto gfx10StartFields = GetWaveStartFields( SqttArch::Gfx10, gfx11Start );
    const auto gfx11StartFields = GetWaveStartFields( SqttArch::Gfx11, gfx11Start );
    ok &= Check( gfx10StartFields.sa == 1 && gfx10StartFields.simd == 2 && gfx10StartFields.wgp == 5 &&
        gfx10StartFields.wave == 0x13, "extract GFX10 wave-start fields" );
    ok &= Check( gfx11StartFields.sa == 1 && gfx11StartFields.simd == 2 && gfx11StartFields.wgp == 5 &&
        gfx11StartFields.wave == 0x13, "extract GFX11 wave-start fields" );

    const uint64_t gfx11End = 0x15 | ( uint64_t( 1 ) << 8 ) | ( uint64_t( 3 ) << 9 ) |
        ( uint64_t( 6 ) << 11 ) | ( uint64_t( 0x1B ) << 15 );
    const auto gfx11EndFields = GetWaveEndFields( SqttArch::Gfx11, gfx11End );
    ok &= Check( gfx11EndFields.sa == 1 && gfx11EndFields.simd == 3 && gfx11EndFields.wgp == 6 &&
        gfx11EndFields.wave == 0x1B, "extract GFX11 wave-end fields" );

    const uint64_t gfx12Start = 0x0C | ( uint64_t( 1 ) << 7 ) | ( uint64_t( 3 ) << 8 ) |
        ( uint64_t( 0xD ) << 10 ) | ( uint64_t( 0x1A ) << 15 );
    const auto gfx12StartFields = GetWaveStartFields( SqttArch::Gfx12, gfx12Start );
    ok &= Check( gfx12StartFields.sa == 1 && gfx12StartFields.simd == 3 && gfx12StartFields.wgp == 0xD &&
        gfx12StartFields.wave == 0x1A, "extract four-bit GFX12 wave-start WGP" );

    const uint64_t gfx12End = 0x15 | ( uint64_t( 1 ) << 8 ) | ( uint64_t( 2 ) << 9 ) |
        ( uint64_t( 0xE ) << 11 ) | ( uint64_t( 0x1D ) << 15 );
    const auto gfx12EndFields = GetWaveEndFields( SqttArch::Gfx12, gfx12End );
    ok &= Check( gfx12EndFields.sa == 1 && gfx12EndFields.simd == 2 && gfx12EndFields.wgp == 0xE &&
        gfx12EndFields.wave == 0x1D, "extract four-bit GFX12 wave-end WGP" );

    ok &= Check( GetInstOpCode( SqttArch::Gfx10, uint64_t( 0x53 ) << 13 ) == 0x53, "extract GFX10 INST opcode" );
    ok &= Check( GetInstOpCode( SqttArch::Gfx11, uint64_t( 0x6A ) << 13 ) == 0x6A, "extract GFX11 INST opcode" );
    ok &= Check( GetInstOpCode( SqttArch::Gfx12, uint64_t( 0xA5 ) << 12 ) == 0xA5, "extract shifted eight-bit GFX12 INST opcode" );

    constexpr uint64_t regWindow = 0x09 | ( uint64_t( 0xA5B ) << 16 );
    ok &= Check( GetRegAddress( SqttArch::Gfx11, regWindow ) == 0xA5B, "extract GFX11 REG address at bit 16" );
    ok &= Check( GetRegAddress( SqttArch::Gfx12, regWindow ) == 0xA5B, "extract GFX12 REG address at bit 16" );
    return ok;
}

struct ExpectedTokenCount
{
    SqttTokenType type;
    size_t count;
};

static bool TestReferenceCapture( SqttArch arch, const uint8_t* data, size_t bytes,
    std::initializer_list<ExpectedTokenCount> expected )
{
    constexpr size_t tokenTypeCount = static_cast<size_t>( SqttTokenType::TOKEN_PERF ) + 1;
    std::array<size_t, tokenTypeCount> counts {};
    size_t bitpos = 0;
    size_t packetCount = 0;
    bool ok = true;
    while( bitpos < bytes * 8 )
    {
        SqttToken token {};
        if( !ReadToken( arch, data, bytes, bitpos, token ) ) break;
        counts[static_cast<size_t>( token.encoding.type )]++;
        packetCount++;
        bitpos += token.encoding.token_len;
    }

    size_t total = 0;
    for( const auto& test : expected )
    {
        const auto count = counts[static_cast<size_t>( test.type )];
        ok &= Check( count == test.count, "match tinygrad reference packet type count" );
        total += count;
    }
    ok &= Check( total == ( arch == SqttArch::Gfx11 ? 148 : 139 ) && packetCount == total,
        "match tinygrad reference packet count and types" );
    ok &= Check( bitpos == bytes * 8, "consume complete tinygrad reference excerpt" );
    if( !ok )
    {
        fprintf( stderr, "GFX%u reference decode: %zu packets, %zu bits\n", static_cast<unsigned>( arch ) + 10,
            packetCount, bitpos );
        for( size_t i = 0; i < counts.size(); i++ )
        {
            if( counts[i] ) fprintf( stderr, "  token type %zu: %zu\n", i, counts[i] );
        }
    }
    return ok;
}

static bool TestReferenceCaptures()
{
    bool ok = true;
    ok &= TestReferenceCapture( SqttArch::Gfx11, sqtt_test_data::gfx1100Gemm, 252, {
        { SqttTokenType::TOKEN_HEADER, 1 },
        { SqttTokenType::TOKEN_MISC, 16 },
        { SqttTokenType::TOKEN_REG, 14 },
        { SqttTokenType::TOKEN_NOP, 106 },
        { SqttTokenType::TOKEN_TIMESTAMP, 4 },
        { SqttTokenType::TOKEN_TIME, 7 },
    } );
    ok &= TestReferenceCapture( SqttArch::Gfx12, sqtt_test_data::gfx1200Gemm, 187, {
        { SqttTokenType::TOKEN_HEADER, 1 },
        { SqttTokenType::TOKEN_TIMESTAMP, 4 },
        { SqttTokenType::TOKEN_REG, 3 },
        { SqttTokenType::TOKEN_NOP, 108 },
        { SqttTokenType::TOKEN_MISC, 23 },
    } );
    return ok;
}

static bool TestMarkers()
{
    constexpr uint32_t cbId = 0x12345;
    constexpr uint64_t pipelineHash = 0x1122334455667788ull;
    std::vector<UserdataWord> words;

    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_CB_START | ( 3u << 4 ) | ( cbId << 7 ), 10 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x11111111, 11 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x22222222, 12 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x33333333, 13 );

    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_BIND_PIPELINE | ( 2u << 4 ) | ( cbId << 9 ), 20 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, static_cast<uint32_t>( pipelineHash ), 21 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, static_cast<uint32_t>( pipelineHash >> 32 ), 22 );

    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_EVENT | ( 2u << 4 ) | ( 6u << 7 ), 30 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, cbId, 31 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0xAABBCCDD, 32 );

    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_BARRIER_START | ( 1u << 4 ) | ( cbId << 7 ), 40 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x1234, 41 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_BARRIER_END | ( 1u << 4 ) | ( cbId << 7 ), 50 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x5678, 51 );

    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_USER_EVENT | ( 2u << 12 ), 60 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 4, 61 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x6B726F57, 62 );

    AddWord( words, SQ_THREAD_TRACE_USERDATA_2, MARKER_CB_END | ( 2u << 4 ) | ( cbId << 7 ), 70 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x44444444, 71 );
    AddWord( words, SQ_THREAD_TRACE_USERDATA_3, 0x55555555, 72 );

    const auto decoded = DecodeMarkers( words );
    const auto decodedGfx10 = DecodeMarkers( SqttArch::Gfx10, words );
    const auto decodedGfx11 = DecodeMarkers( SqttArch::Gfx11, words );
    const auto decodedGfx12 = DecodeMarkers( SqttArch::Gfx12, words );
    bool ok = true;
    ok &= Check( decoded.errors == 0, "decode valid marker stream without errors" );
    ok &= Check( decoded.markers.size() == 7, "decode command-buffer, pipeline, event, barriers, user event, and end markers" );
    ok &= Check( decodedGfx10.markers.size() == decoded.markers.size() &&
        decodedGfx11.markers.size() == decoded.markers.size() && decodedGfx12.markers.size() == decoded.markers.size(),
        "decode marker stream for all supported architectures" );
    if( decoded.markers.size() != 7 ) return false;

    ok &= Check( decoded.markers[0].identifier == MARKER_CB_START && decoded.markers[0].cbId == cbId,
        "decode command-buffer start" );
    ok &= Check( decoded.markers[1].identifier == MARKER_BIND_PIPELINE && decoded.markers[1].cbId == cbId &&
        decoded.markers[1].pipelineHash == pipelineHash, "decode 64-bit pipeline hash" );
    ok &= Check( decoded.markers[2].identifier == MARKER_EVENT && decoded.markers[2].cbId == cbId &&
        decoded.markers[2].apiType == 6 && decoded.markers[2].pipelineHash == pipelineHash,
        "decode event and associated pipeline" );
    ok &= Check( decoded.markers[3].identifier == MARKER_BARRIER_START && decoded.markers[3].cbId == cbId,
        "decode barrier start" );
    ok &= Check( decoded.markers[4].identifier == MARKER_BARRIER_END && decoded.markers[4].cbId == cbId,
        "decode barrier end" );
    ok &= Check( decoded.markers[5].identifier == MARKER_USER_EVENT && decoded.markers[5].userEventType == 2 &&
        decoded.markers[5].userEventName == "Work", "decode user-event string" );
    ok &= Check( decoded.markers[6].identifier == MARKER_CB_END && decoded.markers[6].cbId == cbId,
        "decode command-buffer end" );
    return ok;
}

static bool TestMalformedMarkers()
{
    const auto garbage = DecodeMarkers( { UserdataWord { SQ_THREAD_TRACE_USERDATA_2, 0xD, 0 } } );
    const auto truncated = DecodeMarkers( {
        UserdataWord { SQ_THREAD_TRACE_USERDATA_2, MARKER_CB_START | ( 3u << 4 ) | ( 17u << 7 ), 1 },
        UserdataWord { SQ_THREAD_TRACE_USERDATA_3, 0, 2 },
    } );
    const auto oversized = DecodeMarkers( {
        UserdataWord { SQ_THREAD_TRACE_USERDATA_2, MARKER_USER_EVENT | ( 2u << 12 ), 3 },
        UserdataWord { SQ_THREAD_TRACE_USERDATA_3, 1024 * 1024 + 1, 4 },
    } );
    bool ok = true;
    ok &= Check( garbage.errors == 1 && garbage.markers.empty(), "count unknown marker identifier" );
    ok &= Check( truncated.errors == 1 && truncated.markers.empty(), "count truncated marker record" );
    ok &= Check( oversized.errors == 1 && oversized.markers.empty(), "count oversized user-event string" );
    return ok;
}

int main()
{
    const bool ok = TestTokens() & TestArchTokens() & TestReferenceCaptures() & TestMarkers() & TestMalformedMarkers();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}