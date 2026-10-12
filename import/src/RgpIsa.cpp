#include "RgpIsa.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace tracy::rgp_isa
{

namespace
{

struct Section
{
    uint32_t type;
    uint64_t address;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint64_t entrySize;
};

struct FunctionRange
{
    uint64_t start;
    uint64_t end;
};

struct ElfText
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t address = 0;
    std::vector<FunctionRange> functions;
};

struct PendingInst
{
    IsaInst instruction;
};

uint16_t Read16( const uint8_t* data )
{
    return uint16_t( data[0] ) | ( uint16_t( data[1] ) << 8 );
}

uint32_t Read32( const uint8_t* data )
{
    return uint32_t( data[0] ) | ( uint32_t( data[1] ) << 8 ) | ( uint32_t( data[2] ) << 16 ) | ( uint32_t( data[3] ) << 24 );
}

uint64_t Read64( const uint8_t* data )
{
    return uint64_t( Read32( data ) ) | ( uint64_t( Read32( data + 4 ) ) << 32 );
}

bool IsRangeValid( uint64_t offset, uint64_t size, size_t totalSize )
{
    return offset <= totalSize && size <= totalSize - offset;
}

bool GetSectionName( const uint8_t* names, size_t namesSize, uint32_t nameOffset, std::string& name )
{
    if( nameOffset >= namesSize ) return false;
    const char* begin = reinterpret_cast<const char*>( names + nameOffset );
    const size_t remaining = namesSize - nameOffset;
    const void* terminator = memchr( begin, '\0', remaining );
    if( !terminator ) return false;
    name.assign( begin, static_cast<const char*>( terminator ) - begin );
    return true;
}

bool ParseElfText( const uint8_t* elf, size_t elfSize, ElfText& text, std::string& error )
{
    if( !elf || elfSize < 64 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 2 || elf[5] != 1 )
    {
        error = "invalid ELF64 little-endian AMDGPU code object";
        return false;
    }

    const uint64_t sectionOffset = Read64( elf + 40 );
    const uint16_t sectionEntrySize = Read16( elf + 58 );
    const uint16_t sectionCount = Read16( elf + 60 );
    const uint16_t sectionNamesIndex = Read16( elf + 62 );
    if( sectionEntrySize < 64 || sectionCount == 0 || sectionNamesIndex >= sectionCount ||
        !IsRangeValid( sectionOffset, uint64_t( sectionEntrySize ) * sectionCount, elfSize ) )
    {
        error = "invalid ELF section table";
        return false;
    }

    const uint8_t* sectionNamesHeader = elf + sectionOffset + uint64_t( sectionNamesIndex ) * sectionEntrySize;
    const uint64_t namesOffset = Read64( sectionNamesHeader + 24 );
    const uint64_t namesSize = Read64( sectionNamesHeader + 32 );
    if( !IsRangeValid( namesOffset, namesSize, elfSize ) )
    {
        error = "invalid ELF section-name table";
        return false;
    }

    std::vector<Section> sections;
    sections.reserve( sectionCount );
    size_t textSectionIndex = sectionCount;
    for( uint16_t i = 0; i < sectionCount; i++ )
    {
        const uint8_t* header = elf + sectionOffset + uint64_t( i ) * sectionEntrySize;
        Section section {
            Read32( header + 4 ), Read64( header + 16 ), Read64( header + 24 ), Read64( header + 32 ),
            Read32( header + 40 ), Read64( header + 56 )
        };
        sections.push_back( section );

        std::string name;
        if( GetSectionName( elf + namesOffset, static_cast<size_t>( namesSize ), Read32( header ), name ) && name == ".text" )
        {
            textSectionIndex = i;
        }
    }

    if( textSectionIndex == sectionCount )
    {
        error = "ELF code object has no .text section";
        return false;
    }

    const Section& textSection = sections[textSectionIndex];
    if( textSection.size == 0 || !IsRangeValid( textSection.offset, textSection.size, elfSize ) )
    {
        error = "invalid ELF .text section";
        return false;
    }
    text.data = elf + textSection.offset;
    text.size = static_cast<size_t>( textSection.size );
    text.address = textSection.address;

    for( const auto& symbols : sections )
    {
        if( ( symbols.type != 2 && symbols.type != 11 ) || symbols.link >= sections.size() ||
            symbols.entrySize < 24 || symbols.size / symbols.entrySize == 0 ||
            !IsRangeValid( symbols.offset, symbols.size, elfSize ) ) continue;

        const Section& strings = sections[symbols.link];
        if( !IsRangeValid( strings.offset, strings.size, elfSize ) ) continue;

        const uint64_t count = symbols.size / symbols.entrySize;
        for( uint64_t i = 0; i < count; i++ )
        {
            const uint8_t* symbol = elf + symbols.offset + i * symbols.entrySize;
            if( ( symbol[4] & 0x0F ) != 2 || Read16( symbol + 6 ) != textSectionIndex ) continue;
            const uint64_t address = Read64( symbol + 8 );
            const uint64_t size = Read64( symbol + 16 );
            if( address < text.address || address - text.address >= text.size || size == 0 ) continue;
            const uint64_t start = address - text.address;
            const uint64_t end = start + (std::min)( size, uint64_t( text.size ) - start );
            if( end > start ) text.functions.push_back( FunctionRange { start, end } );
        }
    }

    std::sort( text.functions.begin(), text.functions.end(), []( const auto& lhs, const auto& rhs )
    {
        return lhs.start < rhs.start || ( lhs.start == rhs.start && lhs.end < rhs.end );
    } );
    text.functions.erase( std::unique( text.functions.begin(), text.functions.end(), []( const auto& lhs, const auto& rhs )
    {
        return lhs.start == rhs.start && lhs.end == rhs.end;
    } ), text.functions.end() );
    return true;
}

std::string Trim( std::string value )
{
    const auto begin = value.find_first_not_of( " \t\r\n" );
    if( begin == std::string::npos ) return {};
    const auto end = value.find_last_not_of( " \t\r\n" );
    return value.substr( begin, end - begin + 1 );
}

bool ParseHexAddress( const std::string& line, size_t start, uint64_t& address, size_t& afterColon )
{
    while( start < line.size() && ( line[start] == ' ' || line[start] == '\t' ) ) start++;
    const size_t digitsStart = start;
    address = 0;
    while( start < line.size() )
    {
        const char c = line[start];
        uint8_t digit;
        if( c >= '0' && c <= '9' ) digit = c - '0';
        else if( c >= 'a' && c <= 'f' ) digit = c - 'a' + 10;
        else if( c >= 'A' && c <= 'F' ) digit = c - 'A' + 10;
        else break;
        if( address > ( UINT64_MAX - digit ) / 16 ) return false;
        address = address * 16 + digit;
        start++;
    }
    if( start == digitsStart || start >= line.size() || line[start] != ':' ) return false;
    afterColon = start + 1;
    return true;
}

bool ParseInstructionLine( const std::string& line, uint64_t textAddress, size_t textSize, IsaInst& instruction )
{
    uint64_t address = 0;
    size_t afterColon = 0;
    const size_t comment = line.find( "//" );
    const size_t codeEnd = comment == std::string::npos ? line.size() : comment;
    bool hasAddress = false;
    if( comment != std::string::npos ) hasAddress = ParseHexAddress( line, comment + 2, address, afterColon );
    if( !hasAddress )
    {
        size_t codeAddressEnd = 0;
        hasAddress = ParseHexAddress( line, 0, address, codeAddressEnd );
        if( hasAddress )
        {
            instruction.mnemonic = Trim( line.substr( codeAddressEnd, codeEnd - codeAddressEnd ) );
            if( instruction.mnemonic.empty() || instruction.mnemonic[0] == '<' || instruction.mnemonic.find_first_of( "\\/" ) != std::string::npos ) return false;
        }
        else
        {
            return false;
        }
    }
    else
    {
        instruction.mnemonic = Trim( line.substr( 0, codeEnd ) );
    }

    if( address >= textAddress ) address -= textAddress;
    else if( address >= textSize ) return false;
    if( address >= textSize || instruction.mnemonic.empty() ) return false;
    instruction.offset = address;
    const size_t separator = instruction.mnemonic.find_first_of( " \t" );
    if( separator == std::string::npos )
    {
        if( instruction.mnemonic[0] == '<' ) return false;
        instruction.operands.clear();
    }
    else
    {
        instruction.operands = Trim( instruction.mnemonic.substr( separator + 1 ) );
        instruction.mnemonic.resize( separator );
    }
    return true;
}

size_t FindFunction( const ElfText& text, uint64_t offset )
{
    for( size_t i = 0; i < text.functions.size(); i++ )
    {
        if( offset >= text.functions[i].start && offset < text.functions[i].end ) return i;
    }
    return text.functions.size();
}

std::string MakeTempBase()
{
    static std::atomic<uint64_t> counter { 0 };
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    return "tracy-rgp-isa-" + std::to_string( pid ) + "-" + std::to_string( tick ) + "-" + std::to_string( counter++ );
}

struct TempFiles
{
    std::filesystem::path elf;
    std::filesystem::path output;
    std::filesystem::path errors;

    ~TempFiles()
    {
        std::error_code ec;
        std::filesystem::remove( elf, ec );
        std::filesystem::remove( output, ec );
        std::filesystem::remove( errors, ec );
    }
};

#ifdef _WIN32
std::string QuoteWindowsArgument( const std::string& argument )
{
    std::string quoted = "\"";
    size_t slashes = 0;
    for( char c : argument )
    {
        if( c == '\\' )
        {
            slashes++;
        }
        else if( c == '"' )
        {
            quoted.append( slashes * 2 + 1, '\\' );
            quoted += '"';
            slashes = 0;
        }
        else
        {
            quoted.append( slashes, '\\' );
            quoted += c;
            slashes = 0;
        }
    }
    quoted.append( slashes * 2, '\\' );
    quoted += '"';
    return quoted;
}
#endif

bool RunObjdump( const char* tool, const char* mcpu, const uint8_t* elf, size_t elfSize,
    uint64_t textAddress, size_t textSize,
    std::vector<PendingInst>& decoded, std::string& error )
{
    if( !tool || !*tool )
    {
        error = "llvm-objdump is not configured";
        return false;
    }

    std::error_code ec;
    const auto tempDirectory = std::filesystem::temp_directory_path( ec );
    if( ec )
    {
        error = "cannot locate temporary directory: " + ec.message();
        return false;
    }
    const auto base = tempDirectory / MakeTempBase();
    TempFiles files { base.string() + ".elf", base.string() + ".out", base.string() + ".err" };
    {
        std::ofstream object( files.elf, std::ios::binary );
        object.write( reinterpret_cast<const char*>( elf ), static_cast<std::streamsize>( elfSize ) );
        if( !object )
        {
            error = "cannot write temporary ELF for llvm-objdump";
            return false;
        }
    }

    int exitCode = -1;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security { sizeof( SECURITY_ATTRIBUTES ), nullptr, TRUE };
    HANDLE stdoutFile = CreateFileA( files.output.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr );
    HANDLE stderrFile = CreateFileA( files.errors.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr );
    if( stdoutFile == INVALID_HANDLE_VALUE || stderrFile == INVALID_HANDLE_VALUE )
    {
        if( stdoutFile != INVALID_HANDLE_VALUE ) CloseHandle( stdoutFile );
        if( stderrFile != INVALID_HANDLE_VALUE ) CloseHandle( stderrFile );
        error = "cannot create temporary llvm-objdump output files";
        return false;
    }

    const std::string cpuArgument = std::string( "--mcpu=" ) + ( mcpu && *mcpu ? mcpu : "gfx1030" );
    std::string command = QuoteWindowsArgument( tool ) + " --disassemble --section=.text " + QuoteWindowsArgument( cpuArgument ) +
        " " + QuoteWindowsArgument( files.elf.string() );
    STARTUPINFOA startup {};
    startup.cb = sizeof( startup );
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    startup.hStdOutput = stdoutFile;
    startup.hStdError = stderrFile;
    PROCESS_INFORMATION process {};
    const BOOL created = CreateProcessA( nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process );
    CloseHandle( stdoutFile );
    CloseHandle( stderrFile );
    if( !created )
    {
        error = "cannot start llvm-objdump (Windows error " + std::to_string( GetLastError() ) + ")";
        return false;
    }
    WaitForSingleObject( process.hProcess, INFINITE );
    DWORD processExitCode = 1;
    GetExitCodeProcess( process.hProcess, &processExitCode );
    exitCode = static_cast<int>( processExitCode );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
#else
    const int outputFile = open( files.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    const int errorFile = open( files.errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    if( outputFile < 0 || errorFile < 0 )
    {
        if( outputFile >= 0 ) close( outputFile );
        if( errorFile >= 0 ) close( errorFile );
        error = "cannot create temporary llvm-objdump output files";
        return false;
    }
    const pid_t child = fork();
    if( child == 0 )
    {
        dup2( outputFile, STDOUT_FILENO );
        dup2( errorFile, STDERR_FILENO );
        close( outputFile );
        close( errorFile );
        const std::string cpuArgument = std::string( "--mcpu=" ) + ( mcpu && *mcpu ? mcpu : "gfx1030" );
        execlp( tool, tool, "--disassemble", "--section=.text", cpuArgument.c_str(), files.elf.c_str(), static_cast<char*>( nullptr ) );
        _exit( 127 );
    }
    close( outputFile );
    close( errorFile );
    if( child < 0 )
    {
        error = "cannot start llvm-objdump";
        return false;
    }
    int status = 0;
    while( waitpid( child, &status, 0 ) < 0 ) {}
    if( WIFEXITED( status ) ) exitCode = WEXITSTATUS( status );
#endif

    if( exitCode != 0 )
    {
        std::ifstream diagnostic( files.errors, std::ios::binary );
        std::ostringstream contents;
        contents << diagnostic.rdbuf();
        error = "llvm-objdump failed";
        const std::string detail = Trim( contents.str() );
        if( !detail.empty() ) error += ": " + detail;
        return false;
    }

    std::ifstream output( files.output, std::ios::binary );
    if( !output )
    {
        error = "cannot read llvm-objdump output";
        return false;
    }
    std::string line;
    while( std::getline( output, line ) )
    {
        IsaInst instruction;
        if( ParseInstructionLine( line, textAddress, textSize, instruction ) ) decoded.push_back( PendingInst { std::move( instruction ) } );
    }
    if( decoded.empty() )
    {
        error = "llvm-objdump returned no instructions";
        return false;
    }
    return true;
}

void DisassembleRawDwords( const ElfText& text, std::vector<IsaInst>& instructions )
{
    const auto ranges = text.functions.empty() ? std::vector<FunctionRange> { { 0, text.size } } : text.functions;
    for( const auto& range : ranges )
    {
        const size_t begin = static_cast<size_t>( range.start & ~uint64_t( 3 ) );
        const size_t end = static_cast<size_t>( range.end & ~uint64_t( 3 ) );
        for( size_t offset = begin; offset + 4 <= end; offset += 4 )
        {
            const uint32_t word = Read32( text.data + offset );
            if( word == 0xBF9F0000 ) break;
            char raw[32];
            snprintf( raw, sizeof( raw ), "0x%08x", word );
            instructions.push_back( IsaInst { offset, 4, "unknown", raw } );
        }
    }
}

}

bool DisassembleCodeObject( const uint8_t* elf, size_t elfSize, const char* mcpu, const char* llvmObjdump,
    std::vector<IsaInst>& instructions, std::string& error )
{
    instructions.clear();
    error.clear();
    ElfText text;
    if( !ParseElfText( elf, elfSize, text, error ) ) return false;

    std::vector<PendingInst> decoded;
    std::string backendError;
    if( !RunObjdump( llvmObjdump, mcpu, elf, elfSize, text.address, text.size, decoded, backendError ) )
    {
        error = backendError + "; using raw unknown dwords";
        DisassembleRawDwords( text, instructions );
        return true;
    }

    std::sort( decoded.begin(), decoded.end(), []( const auto& lhs, const auto& rhs )
    {
        return lhs.instruction.offset < rhs.instruction.offset;
    } );
    decoded.erase( std::unique( decoded.begin(), decoded.end(), []( const auto& lhs, const auto& rhs )
    {
        return lhs.instruction.offset == rhs.instruction.offset;
    } ), decoded.end() );

    std::vector<uint64_t> codeEnds( text.functions.empty() ? 1 : text.functions.size(), text.size );
    std::vector<bool> ended( text.functions.empty() ? 1 : text.functions.size(), false );
    for( auto& pending : decoded )
    {
        auto& instruction = pending.instruction;
        if( instruction.offset >= text.size ) continue;
        const size_t function = FindFunction( text, instruction.offset );
        if( !text.functions.empty() && function == text.functions.size() ) continue;
        const size_t rangeIndex = text.functions.empty() ? 0 : function;
        if( ended[rangeIndex] ) continue;
        if( instruction.mnemonic == "s_code_end" )
        {
            codeEnds[rangeIndex] = instruction.offset;
            ended[rangeIndex] = true;
            continue;
        }
        instructions.push_back( std::move( instruction ) );
    }

    for( size_t i = 0; i < instructions.size(); i++ )
    {
        auto& instruction = instructions[i];
        const size_t function = FindFunction( text, instruction.offset );
        const uint64_t functionEnd = text.functions.empty() ? codeEnds[0] : text.functions[function].end;
        const uint64_t end = text.functions.empty() ? codeEnds[0] : codeEnds[function];
        uint64_t nextOffset = end;
        if( i + 1 < instructions.size() && ( text.functions.empty() || FindFunction( text, instructions[i + 1].offset ) == function ) )
        {
            nextOffset = instructions[i + 1].offset;
        }
        else if( end == text.size )
        {
            nextOffset = functionEnd;
        }
        const uint64_t size = nextOffset > instruction.offset ? nextOffset - instruction.offset : 4;
        instruction.size = static_cast<uint8_t>( size >= 4 && size <= 12 && size % 4 == 0 ? size : 4 );
    }
    return true;
}

}