#include "../src/RgpSqtt.hpp"

#include <cstdio>
#include <cstdlib>
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
    bool ok = true;
    ok &= Check( decoded.errors == 0, "decode valid marker stream without errors" );
    ok &= Check( decoded.markers.size() == 7, "decode command-buffer, pipeline, event, barriers, user event, and end markers" );
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
    const bool ok = TestTokens() & TestMarkers() & TestMalformedMarkers();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}