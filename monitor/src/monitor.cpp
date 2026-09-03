// tracy-monitor: profile a process that was not built with the Tracy client.
// Launch mode: tracy-monitor [OPTIONS] program [args...]; attach mode:
// tracy-monitor [OPTIONS] -p PID | -n NAME. Runs the standard Tracy client in
// "external mode". With TRACY_ON_DEMAND, samples are captured only while a
// Tracy server is connected. See --help for usage and permission requirements.
//
#include <memory>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
#  include <process.h>
#  include <getopt.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <getopt.h>
#  include <sys/ioctl.h>
#  include <linux/perf_event.h>
#  include <netinet/in.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#  include <sys/ptrace.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "../public/tracy/Tracy.hpp"
#include "../public/common/TracyVersion.hpp"
#include "../public/client/TracyCallstack.hpp"
#include "GitRef.hpp"

#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyPrint.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../util/CaptureConsole.hpp"
#include "../../util/CaptureFileBackup.hpp"

// The monitor is only meaningful with the client's full sampling +
// symbolication stack. These configurations would either fail to build
// (TRACY_NO_CALLSTACK: the external-target API lives in the callstack
// code) or silently produce an empty capture (IsSystemTracingFailed()
// cannot see them), so reject them here rather than in the client.
#if defined TRACY_NO_CALLSTACK
#  error "tracy-monitor requires callstack support: TRACY_NO_CALLSTACK is not supported"
#endif
#if defined TRACY_NO_SYSTEM_TRACING
#  error "tracy-monitor requires system tracing: TRACY_NO_SYSTEM_TRACING is not supported"
#endif
#if defined TRACY_NO_SAMPLING
#  error "tracy-monitor requires callstack sampling: TRACY_NO_SAMPLING is not supported"
#endif
#if defined TRACY_SAMPLING_PROFILER_MANUAL_START
#  error "tracy-monitor starts sampling itself via StartupProfiler(): TRACY_SAMPLING_PROFILER_MANUAL_START is not supported"
#endif

static void SetEnv( const char* key, const char* val )
{
#ifdef _WIN32
    _putenv_s( key, val );
#else
    setenv( key, val, 1 );
#endif
}

static void UnsetEnv( const char* key )
{
#ifdef _WIN32
    _putenv_s( key, "" );
#else
    unsetenv( key );
#endif
}

static void SleepMs( uint32_t ms )
{
#ifdef _WIN32
    Sleep( ms );
#else
    usleep( ms * 1000 );
#endif
}

static void CloseSocket( int s )
{
#ifdef _WIN32
    closesocket( s );
#else
    close( s );
#endif
}

// --- signal and shutdown handling --------------------------------------------

#ifdef _WIN32
static volatile int s_shouldQuit = 0;
static HANDLE s_childProcess = NULL;

static BOOL WINAPI ConsoleCtrlHandler( DWORD ctrlType )
{
    switch( ctrlType )
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        s_shouldQuit = 1;
        return TRUE;
    default:
        return FALSE;
    }
}
#else
static volatile sig_atomic_t s_shouldQuit = 0;
static pid_t s_targetPid = 0;
static bool s_isForked = false;

static void SignalHandler( int /*sig*/ )
{
    s_shouldQuit = 1;
    if( s_isForked && s_targetPid != 0 )
    {
        // We launched the target under ptrace, so forward the signal to wake
        // a blocking waitpid and let the child exit. kill() is async-signal-safe.
        kill( s_targetPid, SIGINT );
    }
}
#endif

// --- process liveness --------------------------------------------------------

#ifdef _WIN32
static bool ProcessIsAlive( uint32_t pid )
{
    HANDLE h = OpenProcess( SYNCHRONIZE, FALSE, pid );
    if( !h ) return false;
    const DWORD res = WaitForSingleObject( h, 0 );
    CloseHandle( h );
    return res == WAIT_TIMEOUT;
}
#else
// kill( pid, 0 ) succeeds even for zombies, so liveness also checks the
// /proc state: a zombie is dead - nothing left to sample, and in attach
// mode we are not the parent, so we can neither reap it nor learn its exit.
static bool ProcessIsAlive( pid_t pid )
{
    if( kill( pid, 0 ) != 0 ) return false;
    char path[32];
    snprintf( path, sizeof( path ), "/proc/%d/stat", (int)pid );
    FILE* f = fopen( path, "r" );
    if( !f ) return false;
    char buf[512];
    const size_t n = fread( buf, 1, sizeof( buf ) - 1, f );
    fclose( f );
    buf[n] = 0;
    // the state char follows the last ')' (comm may contain spaces/parens)
    const char* state = strrchr( buf, ')' );
    return state && state[1] == ' ' && state[2] != 'Z' && state[2] != '\0';
}
#endif

// --- configuration helpers ---------------------------------------------------

static const char* FormatHz( int hz )
{
    static char buf[32];
    if( hz >= 1000 && hz % 1000 == 0 )
    {
        snprintf( buf, sizeof( buf ), "%d kHz", hz / 1000 );
    }
    else
    {
        snprintf( buf, sizeof( buf ), "%d Hz", hz );
    }
    return buf;
}

static int EffectiveSamplingHz()
{
    const char* env = getenv( "TRACY_SAMPLING_HZ" );
    int hz = 8000;
#ifdef __linux__
    hz = 10000; // client default on Linux
#endif
    if( env )
    {
        const int parsed = atoi( env );
        if( parsed > 0 ) hz = ( parsed > 1000000 ) ? 1000000 : parsed;
    }
#ifdef __linux__
    // mirror the client's clamp: the kernel caps the rate at perf_event_max_sample_rate
    FILE* f = fopen( "/proc/sys/kernel/perf_event_max_sample_rate", "r" );
    if( f )
    {
        int sysMax = 0;
        if( fscanf( f, "%d", &sysMax ) == 1 && sysMax > 0 && sysMax < hz ) hz = sysMax;
        fclose( f );
    }
#elif defined _WIN32
    if( hz > 8000 ) hz = 8000; // client clamp on Windows
#endif
    return hz;
}

static int EffectivePort()
{
    const char* env = getenv( "TRACY_PORT" );
    if( env )
    {
        const int port = atoi( env );
        if( port > 0 && port <= 65535 ) return port;
    }
    return 8086;
}

static bool VerifyClientListening()
{
    for( int i = 0; i < 40; i++ )
    {
        if( tracy::IsDataPortListening() ) return true;
        SleepMs( 125 );
    }
    return false;
}

// Reserve the first free port in the client's 8086..8105 scan range and hand the
// probe socket to the client (SetReservedListenSocket) so the reservation is atomic.
static bool ProbeClientBind( int port, bool ipv4Only, bool onlyLocalhost, int& fdOut )
{
#ifdef _WIN32
    WSADATA wsaData;
    if( WSAStartup( MAKEWORD( 2, 2 ), &wsaData ) != 0 ) return false;
#endif
    fdOut = -1;
    int s = -1;
    int family = AF_INET6;
    if( !ipv4Only ) s = (int)socket( AF_INET6, SOCK_STREAM, 0 );
    if( s < 0 )
    {
        family = AF_INET;
        s = (int)socket( AF_INET, SOCK_STREAM, 0 );
    }
    if( s < 0 ) return false;
#ifndef _WIN32
    fcntl( s, F_SETFD, FD_CLOEXEC );
#endif
    int val = 1;
    setsockopt( s, SOL_SOCKET, SO_REUSEADDR, (const char*)&val, sizeof( val ) );
    bool ok = false;
    if( family == AF_INET6 )
    {
#ifdef _WIN32
        unsigned long v6only = 0;
        setsockopt( s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof( v6only ) );
#elif defined __APPLE__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__ || defined __DragonFly__
        int v6only = 0;
        setsockopt( s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof( v6only ) );
#endif
        sockaddr_in6 addr = {};
        addr.sin6_family = AF_INET6;
        if( onlyLocalhost ) addr.sin6_addr = in6addr_loopback;
        addr.sin6_port = htons( (uint16_t)port );
        ok = bind( s, (sockaddr*)&addr, sizeof( addr ) ) == 0 && listen( s, 4 ) == 0;
    }
    else
    {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = onlyLocalhost ? htonl( INADDR_LOOPBACK ) : INADDR_ANY;
        addr.sin_port = htons( (uint16_t)port );
        ok = bind( s, (sockaddr*)&addr, sizeof( addr ) ) == 0 && listen( s, 4 ) == 0;
    }
    if( ok )
    {
        fdOut = s;
    }
    else
    {
        CloseSocket( s );
    }
    return ok;
}

static void ReservePortIfUnpinned()
{
    if( getenv( "TRACY_PORT" ) ) return;
    const char* onlyIPv4 = getenv( "TRACY_ONLY_IPV4" );
    const char* onlyLocalhost = getenv( "TRACY_ONLY_LOCALHOST" );
    const bool ipv4Only = onlyIPv4 && onlyIPv4[0] == '1';
    const bool localhost = onlyLocalhost && onlyLocalhost[0] == '1';
    for( int i=0; i<20; i++ )
    {
        const int port = 8086 + i;
        int fd = -1;
        if( ProbeClientBind( port, ipv4Only, localhost, fd ) )
        {
            char buf[8];
            snprintf( buf, sizeof( buf ), "%d", port );
            SetEnv( "TRACY_PORT", buf );
            tracy::SetReservedListenSocket( fd );
            return;
        }
    }
}

// --- trace saving ---------------------------------------------------------------

// No-op until the worker's first rate sample arrives and when stdout is not a terminal.
static void PrintProgress( tracy::Worker* worker )
{
    if( !worker || !worker->HasData() ) return;
    PrintCaptureProgress( *worker, worker->GetFirstTime(), -1 );
}

// True when the status line actually occupies the current line, so that a
// newline is needed to terminate it. PrintCaptureProgress skips rendering
// until the first rate sample arrives, and printing a bare "\n" then would
// add a stray blank line.
static bool ProgressLineActive( tracy::Worker* worker )
{
    if( !worker || !IsTerminal() ) return false;
    auto& lock = worker->GetMbpsDataLock();
    lock.lock();
    const bool active = !worker->GetMbpsData().empty();
    lock.unlock();
    return active;
}

static bool WaitForWorker( tracy::Worker& worker, const char** reasonOut )
{
    *reasonOut = nullptr;
    for( int i = 0; i < 100; i++ )
    {
        if( worker.HasData() ) return true;
        if( s_shouldQuit )
        {
            *reasonOut = "interrupted before the capture started";
            return false;
        }
        const auto hs = worker.GetHandshakeStatus();
        if( hs != tracy::HandshakePending )
        {
            // the first connection was made and rejected or dropped; the worker
            // never reconnects, so this outcome is final
            switch( (tracy::HandshakeStatus)hs )
            {
            case tracy::HandshakeProtocolMismatch:
                *reasonOut = "the client rejected the protocol version (the client and the worker come from the same build; this should not happen)";
                break;
            case tracy::HandshakeNotAvailable:
                *reasonOut = "another server (e.g. the Tracy profiler GUI) is already connected to the client, and the client serves only one server at a time";
                break;
            default:
                *reasonOut = "the client dropped the connection during the handshake";
                break;
            }
            return false;
        }
        PrintProgress( &worker );
        SleepMs( 100 );
    }
    *reasonOut = "the worker did not connect to the client within 10 seconds";
    return false;
}

// All of the client's data has reached the worker by the time this runs.
static int SaveTrace( tracy::Worker& worker, const char* output )
{
    // IsConnected() flips only after the last data was processed, so this
    // also bounds the worker's own teardown.
    while( worker.IsConnected() )
    {
        SleepMs( 100 );
    }

    const auto failure = worker.GetFailureType();
    if( failure != tracy::Worker::Failure::None )
    {
        fprintf( stderr, "Capture failure: %s\n", tracy::Worker::GetFailureString( failure ) );
        const auto& fd = worker.GetFailureData();
        if( !fd.message.empty() )
        {
            fprintf( stderr, "Context: %s\n", fd.message.c_str() );
        }
    }

    if( !worker.HasData() )
    {
        fprintf( stderr, "No trace saved: the worker received no data from the client.\n" );
        tracy::RestoreOutputBackup();
        return 1;
    }

    printf( "Time span: %s, callstack samples: %s\n",
        tracy::TimeToString( worker.GetLastTime() - worker.GetFirstTime() ),
        tracy::RealToString( worker.GetCallstackSampleCount() ) );
    printf( "Saving trace to %s...", output );
    fflush( stdout );

    auto f = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open( output, tracy::FileCompression::Zstd, 3, 4 ) );
    if( !f )
    {
        fprintf( stderr, "\nCannot open output file %s for writing!\n", output );
        tracy::RestoreOutputBackup();
        return 5;
    }
    worker.Write( *f, false );
    f->Finish();
    const auto stats = f->GetCompressionStatistics();
    printf( " done\nTrace size %s (%.2f%% ratio)\n", tracy::MemSizeToString( stats.second ), 100.f * stats.second / stats.first );
    tracy::DiscardOutputBackup();
    return 0;
}

// =============================================================================
// Linux Implementation
// =============================================================================
#ifdef __linux__

static const char* GetTraceFsPath()
{
    static char path[ 4096 ];
    static char debugPath[ 4096 ];
    bool haveDebug = false;
    FILE* f = fopen( "/proc/mounts", "r" );
    if( !f ) return nullptr;
    char line[ 4096 ];
    while( fgets( line, sizeof( line ), f ) )
    {
        char dir[ 4096 ];
        char type[ 64 ];
        if( sscanf( line, "%*s %4095s %63s", dir, type ) != 2 ) continue;
        if( strcmp( type, "tracefs" ) == 0 )
        {
            snprintf( path, sizeof( path ), "%s", dir );
            fclose( f );
            return path;
        }
        if( !haveDebug && strcmp( type, "debugfs" ) == 0 )
        {
            snprintf( debugPath, sizeof( debugPath ), "%s/tracing", dir );
            haveDebug = true;
        }
    }
    fclose( f );
    if( haveDebug )
    {
        snprintf( path, sizeof( path ), "%s", debugPath );
        return path;
    }
    return nullptr;
}

enum class TracepointStatus { Open, NoTraceFs, IdNotReadable, OpenDenied };

static TracepointStatus CheckSystemWideTracepoint( const char* eventPath )
{
    const char* traceFs = GetTraceFsPath();
    if( !traceFs ) return TracepointStatus::NoTraceFs;

    char path[512];
    snprintf( path, sizeof( path ), "%s%s/id", traceFs, eventPath );
    FILE* f = fopen( path, "r" );
    if( !f ) return TracepointStatus::IdNotReadable;
    int id = -1;
    if( fscanf( f, "%d", &id ) != 1 ) id = -1;
    fclose( f );
    if( id < 0 ) return TracepointStatus::IdNotReadable;

    perf_event_attr pe = {};
    pe.type = PERF_TYPE_TRACEPOINT;
    pe.size = sizeof( pe );
    pe.config = (uint64_t)id;
    pe.sample_period = 1;
    pe.sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_RAW;
    pe.disabled = 1;
    pe.inherit = 1;

    const long fd = syscall( __NR_perf_event_open, &pe, -1, 0, -1, 0 );
    if( fd < 0 ) return TracepointStatus::OpenDenied;
    close( (int)fd );
    return TracepointStatus::Open;
}

static bool CanReadRapl()
{
    const char* base = "/sys/devices/virtual/powercap/intel-rapl";
    DIR* dir = opendir( base );
    if( !dir ) return false;
    struct dirent* ent;
    bool ok = false;
    while( ( ent = readdir( dir ) ) )
    {
        if( ent->d_type != DT_DIR || strncmp( ent->d_name, "intel-rapl:", 11 ) != 0 ) continue;
        char path[512];
        snprintf( path, sizeof( path ), "%s/%s/energy_uj", base, ent->d_name );
        FILE* f = fopen( path, "r" );
        if( f )
        {
            fclose( f );
            ok = true;
            break;
        }
    }
    closedir( dir );
    return ok;
}

static bool PreflightOutputMechanism( pid_t pid )
{
    perf_event_attr pe = {};
    pe.type = PERF_TYPE_SOFTWARE;
    pe.size = sizeof( pe );
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    pe.sample_freq = 100;
    pe.freq = 1;
    pe.disabled = 1;
    pe.inherit = 1;
    pe.exclude_kernel = 1;
    pe.exclude_callchain_kernel = 1;

    const long fd = syscall( __NR_perf_event_open, &pe, pid, 0, -1, 0 );
    if( fd < 0 )
    {
        fprintf( stderr, "Warning: preflight per-CPU event open failed: %s\n", strerror( errno ) );
        return false;
    }
    const size_t mapSize = 64 * 1024 + 4096;
    void* map = mmap( nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0 );
    bool ok = map != MAP_FAILED;
    if( !ok ) fprintf( stderr, "Warning: preflight ring mmap refused: %s (kernel ABI mismatch?)\n", strerror( errno ) );
    if( map != MAP_FAILED ) munmap( map, mapSize );
    close( (int)fd );
    return ok;
}

static bool ProbeHwCounters( pid_t pid, int& errOut )
{
    perf_event_attr pe = {};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof( pe );
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    pe.disabled = 1;
    pe.precise_ip = 3;
    long fd = syscall( __NR_perf_event_open, &pe, pid, -1, -1, 0 );
    while( fd < 0 && pe.precise_ip > 0 )
    {
        pe.precise_ip--;
        fd = syscall( __NR_perf_event_open, &pe, pid, -1, -1, 0 );
    }
    if( fd < 0 && ( errno == EACCES || errno == EPERM ) )
    {
        pe.exclude_kernel = 1;
        fd = syscall( __NR_perf_event_open, &pe, pid, -1, -1, 0 );
    }
    if( fd >= 0 )
    {
        close( (int)fd );
        return true;
    }
    errOut = errno;
    return false;
}

static bool PreflightSamplingEvent( pid_t pid, bool& kernelFrames, bool& hwStats, int& hwErrno )
{
    perf_event_attr pe = {};
    pe.type = PERF_TYPE_SOFTWARE;
    pe.size = sizeof( pe );
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CALLCHAIN;
    pe.sample_freq = 1000;
    pe.freq = 1;
    pe.disabled = 1;
    pe.inherit = 1;

    long dataFd = syscall( __NR_perf_event_open, &pe, pid, 0, -1, 0 );
    if( dataFd < 0 )
    {
        const int err = errno;
        if( err == ESRCH )
        {
            fprintf( stderr, "Target process %d no longer exists.\n", (int)pid );
            return false;
        }
        if( err == EACCES || err == EPERM )
        {
            pe.exclude_kernel = 1;
            pe.exclude_callchain_kernel = 1;
            dataFd = syscall( __NR_perf_event_open, &pe, pid, 0, -1, 0 );
            if( dataFd >= 0 )
            {
                close( (int)dataFd );
                kernelFrames = false;
            }
        }
        else
        {
            fprintf( stderr, "Cannot open perf events for pid %d: %s\n", (int)pid, strerror( err ) );
            return false;
        }
    }
    else
    {
        close( (int)dataFd );
        kernelFrames = true;
    }

    if( dataFd < 0 )
    {
        fprintf( stderr, "Cannot open perf events for pid %d: %s\n", (int)pid, strerror( errno ) );
        fprintf( stderr, "Profiling another process requires one of:\n" );
        fprintf( stderr, "  - running as root (or CAP_PERFMON + CAP_SYS_PTRACE), or\n" );
        fprintf( stderr, "  - /proc/sys/kernel/perf_event_paranoid <= 1 for kernel frames,\n" );
        fprintf( stderr, "    <= -1 for system-wide events (context switches / per-thread\n" );
        fprintf( stderr, "    CPU / wait stacks), or\n" );
        fprintf( stderr, "  - in attach mode: the target must belong to the same user\n" );
        fprintf( stderr, "    (uid and gid) and be dumpable, or the monitor must hold\n" );
        fprintf( stderr, "    CAP_SYS_PTRACE (yama ptrace_scope does not apply: sampling\n" );
        fprintf( stderr, "    uses ptrace READ access, and the monitor never attaches).\n" );
        return false;
    }

    if( !PreflightOutputMechanism( pid ) )
    {
        fprintf( stderr, "Per-CPU sample rings are unavailable on this kernel; external sampling will not work.\n" );
        return false;
    }
    hwStats = ProbeHwCounters( pid, hwErrno );
    return true;
}

static const char* TracepointReason( TracepointStatus status )
{
    switch( status )
    {
    case TracepointStatus::NoTraceFs: return "tracefs is not readable — running as root is required";
    case TracepointStatus::IdNotReadable: return "the event id is not readable — running as root is required";
    case TracepointStatus::OpenDenied: return "system-wide events are denied — needs perf_event_paranoid <= -1 or CAP_PERFMON";
    default: return nullptr;
    }
}

static void PrintStartupReport( bool kernelFrames, bool hwStats, int hwErrno, const char* output )
{
    const TracepointStatus ctxSwitches = CheckSystemWideTracepoint( "/events/sched/sched_switch" );
    const TracepointStatus vsync = CheckSystemWideTracepoint( "/events/drm/drm_vblank_event" );
    const bool power = CanReadRapl();

    printf( "tracy-monitor %i.%i.%i / %s\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef );
    printf( "Profiling '%s' (pid %d) on port %d\n", tracy::GetExternalTargetName(), (int)tracy::GetExternalTargetPid(), EffectivePort() );
    printf( "  sampling:                      %s, %s callchain — leaf-only stacks for -fomit-frame-pointer targets\n", FormatHz( EffectiveSamplingHz() ), kernelFrames ? "kernel" : "user-space" );
    printf( "  kernel frames:                 %s\n", kernelFrames ? "yes" : "no (kernel sampling denied — perf_event_paranoid > 1 without CAP_PERFMON)" );
    if( hwStats )
    {
        printf( "  hardware sampling statistics:  yes\n" );
    }
    else
    {
        printf( "  hardware sampling statistics:  no (PMU counters unavailable: %s)\n", strerror( hwErrno ) );
    }
    if( ctxSwitches == TracepointStatus::Open )
    {
        printf( "  ctx switches / per-thread CPU: yes\n" );
    }
    else
    {
        printf( "  ctx switches / per-thread CPU: no (%s)\n", TracepointReason( ctxSwitches ) );
    }
    printf( "  power (RAPL):                  %s\n", power ? "yes" : "no (needs read access to /sys/devices/virtual/powercap/intel-rapl)" );
    if( vsync == TracepointStatus::Open )
    {
        printf( "  vsync:                         yes\n" );
    }
    else
    {
        printf( "  vsync:                         no (%s)\n", TracepointReason( vsync ) );
    }
    if( output )
    {
        printf( "  output:                        %s\n", output );
    }
#ifdef TRACY_ON_DEMAND
    printf( "  recording:                     on-demand: samples are captured only while a Tracy server is connected; pre-connection samples are discarded\n" );
#endif
    printf( "\n" );
    if( !output )
    {
        printf( "Open the Tracy profiler and connect to this host:port (or it will auto-discover).\n" );
    }
    fflush( stdout );
}

// --- attach mode ---------------------------------------------------------------
static int FindPidsByComm( const char* name, pid_t* outPids, int maxPids )
{
    int count = 0;
    DIR* proc = opendir( "/proc" );
    if( !proc ) return 0;
    struct dirent* ent;
    while( ( ent = readdir( proc ) ) )
    {
        if( ent->d_name[0] < '0' || ent->d_name[0] > '9' ) continue;
        char path[64];
        snprintf( path, sizeof( path ), "/proc/%s/comm", ent->d_name );
        FILE* f = fopen( path, "r" );
        if( !f ) continue;
        char comm[32] = {};
        if( fgets( comm, sizeof( comm ), f ) )
        {
            size_t len = strlen( comm );
            while( len > 0 && ( comm[len-1] == '\n' || comm[len-1] == '\r' ) ) comm[--len] = '\0';
            if( strcmp( comm, name ) == 0 && count < maxPids )
            {
                char exe[256] = {};
                snprintf( path, sizeof( path ), "/proc/%s/exe", ent->d_name );
                if( readlink( path, exe, sizeof( exe ) - 1 ) >= 0 )
                {
                    outPids[count++] = (pid_t)atoi( ent->d_name );
                }
            }
        }
        fclose( f );
    }
    closedir( proc );
    return count;
}

static int RunAttached( pid_t pid, const char* output )
{
    if( kill( pid, 0 ) != 0 )
    {
        fprintf( stderr, "Process %d does not exist or is not accessible.\n", (int)pid );
        return 1;
    }
    if( !ProcessIsAlive( pid ) )
    {
        fprintf( stderr, "Process %d is a zombie (it has already exited); nothing to attach to.\n", (int)pid );
        return 1;
    }

    s_targetPid = pid;

    printf( "Attaching to process %d", (int)pid );
    fflush( stdout );

    if( !tracy::InitExternalTarget( (uint32_t)pid ) ) return 1;

    printf( " (%s)...\n", tracy::GetExternalTargetName() );
    fflush( stdout );

    bool kernelFrames = false;
    bool hwStats = false;
    int hwErrno = 0;
    if( !PreflightSamplingEvent( pid, kernelFrames, hwStats, hwErrno ) ) return 1;

    if( !GetTraceFsPath() )
    {
        fprintf( stderr, "No tracefs mount found: the client's system tracing cannot start without it, so no samples would be captured.\n" );
        return 1;
    }

    tracy::StartupProfiler();
    if( tracy::IsSystemTracingFailed() )
    {
        fprintf( stderr, "The client failed to start sampling although the preflight passed; the capture would contain no samples (the target may have exited between checks, or the kernel rejected the event setup).\n" );
        tracy::ShutdownProfiler();
        return 1;
    }

    if( !VerifyClientListening() )
    {
        fprintf( stderr, "The client could not listen on port %d.\n", EffectivePort() );
        tracy::ShutdownProfiler();
        return 1;
    }

    PrintStartupReport( kernelFrames, hwStats, hwErrno, output );

    // The worker is created only after the client started: the worker's threads
    // name themselves via the client's SetThreadName(), which requires the client
    // to be up (the monitor runs it with TRACY_MANUAL_LIFETIME).
    std::unique_ptr<tracy::Worker> worker;
    if( output )
    {
        worker = std::make_unique<tracy::Worker>( "127.0.0.1", (uint16_t)EffectivePort(), -1 );
        const char* reason = nullptr;
        if( !WaitForWorker( *worker, &reason ) )
        {
            fprintf( stderr, "Cannot capture to %s: %s.\n", output, reason );
            tracy::RestoreOutputBackup();
            tracy::ShutdownProfiler();
            return 1;
        }
    }

    while( !s_shouldQuit && ProcessIsAlive( pid ) )
    {
        PrintProgress( worker.get() );
        SleepMs( 100 );  // 100ms poll
    }

    if( ProgressLineActive( worker.get() ) ) printf( "\n" );  // terminate the status line

    if( s_shouldQuit )
    {
        printf( "\nShutting down profiler...\n" );
    }
    else
    {
        printf( "Target exited.\n" );
    }

    tracy::ShutdownProfiler();
    if( worker ) return SaveTrace( *worker, output );
    return 0;
}

// --- launch mode ---------------------------------------------------------------

static int RunForked( int argc, char** argv, const char* output )
{
    pid_t childPid = fork();
    if( childPid < 0 )
    {
        fprintf( stderr, "Unable to fork: %s\n", strerror( errno ) );
        return 2;
    }

    if( childPid == 0 )
    {
        UnsetEnv( "TRACY_SAMPLING_HZ" );
        UnsetEnv( "TRACY_PORT" );
        if( ptrace( PTRACE_TRACEME, 0, nullptr, nullptr ) < 0 )
        {
            fprintf( stderr, "ptrace(TRACEME) failed: %s\n", strerror( errno ) );
            _exit( 2 );
        }
        execvp( argv[0], argv );
        fprintf( stderr, "Unable to exec '%s': %s\n", argv[0], strerror( errno ) );
        _exit( 2 );
    }

    s_targetPid = childPid;
    s_isForked = true;

    int status;
    for(;;)
    {
        if( waitpid( childPid, &status, 0 ) >= 0 ) break;
        if( errno == EINTR ) continue;
        fprintf( stderr, "waitpid failed: %s\n", strerror( errno ) );
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        return 2;
    }

    while( WIFSTOPPED( status ) && WSTOPSIG( status ) != SIGTRAP )
    {
        const int rawStopSig = WSTOPSIG( status );
        const int stopSig = rawStopSig & 0x7f;
        const int resumeSig = ( rawStopSig & 0x80 || stopSig == SIGSTOP || stopSig == SIGTSTP ) ? 0 : stopSig;
        if( ptrace( PTRACE_CONT, childPid, 0, (void*)(unsigned long)resumeSig ) != 0 )
        {
            fprintf( stderr, "ptrace failed: %s\n", strerror( errno ) );
            kill( childPid, SIGKILL );
            waitpid( childPid, nullptr, 0 );
            return 2;
        }
        for(;;)
        {
            if( waitpid( childPid, &status, 0 ) >= 0 ) break;
            if( errno == EINTR ) continue;
            fprintf( stderr, "waitpid failed: %s\n", strerror( errno ) );
            kill( childPid, SIGKILL );
            waitpid( childPid, nullptr, 0 );
            return 2;
        }
    }

    if( !WIFSTOPPED( status ) )
    {
        if( s_shouldQuit )
        {
            fprintf( stderr, "\nInterrupted before target started.\n" );
        }
        else if( WIFEXITED( status ) )
        {
            fprintf( stderr, "Target exited before profiling began (status %d) -- exec failed?\n", WEXITSTATUS( status ) );
        }
        else if( WIFSIGNALED( status ) )
        {
            fprintf( stderr, "Target killed by signal %d before profiling began.\n", WTERMSIG( status ) );
        }
        else
        {
            fprintf( stderr, "Child process did not stop as expected (status=0x%x).\n", status );
        }
        return 2;
    }

    if( !tracy::InitExternalTarget( (uint32_t)childPid ) )
    {
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        return 1;
    }

    printf( "Profiling '%s' (pid %d)...\n", tracy::GetExternalTargetName(), (int)childPid );
    fflush( stdout );

    bool kernelFrames = false;
    bool hwStats = false;
    int hwErrno = 0;
    if( !PreflightSamplingEvent( childPid, kernelFrames, hwStats, hwErrno ) )
    {
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        return 1;
    }

    if( !GetTraceFsPath() )
    {
        fprintf( stderr, "No tracefs mount found: the client's system tracing cannot start without it, so no samples would be captured.\n" );
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        return 1;
    }

    tracy::StartupProfiler();
    if( tracy::IsSystemTracingFailed() )
    {
        fprintf( stderr, "The client failed to start sampling although the preflight passed; the capture would contain no samples (the target may have exited between checks, or the kernel rejected the event setup).\n" );
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        tracy::ShutdownProfiler();
        return 1;
    }

    if( ptrace( PTRACE_DETACH, childPid, nullptr, nullptr ) < 0 )
    {
        fprintf( stderr, "ptrace(DETACH) failed: %s -- killing child.\n", strerror( errno ) );
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        tracy::ShutdownProfiler();
        return 2;
    }

    if( !VerifyClientListening() )
    {
        fprintf( stderr, "The client could not listen on port %d.\n", EffectivePort() );
        kill( childPid, SIGKILL );
        waitpid( childPid, nullptr, 0 );
        tracy::ShutdownProfiler();
        return 1;
    }

    PrintStartupReport( kernelFrames, hwStats, hwErrno, output );

    std::unique_ptr<tracy::Worker> worker;
    if( output )
    {
        worker = std::make_unique<tracy::Worker>( "127.0.0.1", (uint16_t)EffectivePort(), -1 );
        const char* reason = nullptr;
        if( !WaitForWorker( *worker, &reason ) )
        {
            fprintf( stderr, "Cannot capture to %s: %s.\n", output, reason );
            tracy::RestoreOutputBackup();
            kill( childPid, SIGKILL );
            waitpid( childPid, nullptr, 0 );
            tracy::ShutdownProfiler();
            return 1;
        }
    }

    for(;;)
    {
        if( s_shouldQuit ) break;

        int wstatus;
        pid_t ret = waitpid( childPid, &wstatus, WNOHANG );
        if( ret > 0 )
        {
            if( WIFEXITED( wstatus ) )
            {
                printf( "Target exited with status %d.\n", WEXITSTATUS( wstatus ) );
            }
            else if( WIFSIGNALED( wstatus ) )
            {
                printf( "Target exited (signal %d).\n", WTERMSIG( wstatus ) );
            }
            break;
        }
        else if( ret < 0 && errno != EINTR )
        {
            break;
        }
        PrintProgress( worker.get() );
        SleepMs( 100 );
    }

    if( ProgressLineActive( worker.get() ) ) printf( "\n" );  // terminate the status line

    if( s_shouldQuit && ProcessIsAlive( childPid ) )
    {
        printf( "\nForwarding signal to child and shutting down...\n" );
        kill( childPid, SIGINT );
        SleepMs( 500 );
        if( ProcessIsAlive( childPid ) )
        {
            kill( childPid, SIGKILL );
        }
        waitpid( childPid, nullptr, 0 );
    }
    else if( s_shouldQuit )
    {
        waitpid( childPid, nullptr, WNOHANG );
    }

    tracy::ShutdownProfiler();
    if( worker ) return SaveTrace( *worker, output );
    return 0;
}

#endif // __linux__

// =============================================================================
// Windows Implementation
// =============================================================================
#ifdef _WIN32

static bool CheckAdminPrivileges()
{
    HANDLE hToken = NULL;
    if( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &hToken ) ) return false;
    TOKEN_ELEVATION_TYPE elevationType = TokenElevationTypeDefault;
    DWORD returnLength = 0;
    const BOOL ok = GetTokenInformation( hToken, TokenElevationType, &elevationType, sizeof( elevationType ), &returnLength );
    CloseHandle( hToken );
    return ok && ( elevationType == TokenElevationTypeFull );
}

static void PrintStartupReport( const char* output = nullptr )
{
    printf( "tracy-monitor %i.%i.%i / %s\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef );
    printf( "Profiling '%s' (pid %u) on port %d\n", tracy::GetExternalTargetName(), tracy::GetExternalTargetPid(), EffectivePort() );
    printf( "  sampling:                      %s (ETW kernel profiling)\n", FormatHz( EffectiveSamplingHz() ) );
    printf( "  ctx switches / per-thread CPU: yes (ETW context switch monitoring)\n" );
    printf( "  vsync:                         yes (ETW DxgKrnl)\n" );
#ifdef TRACY_ON_DEMAND
    printf( "  recording:                     on-demand: samples are captured only while a Tracy server is connected; pre-connection samples are discarded\n" );
#endif
    if( output )
    {
        printf( "  output:                        %s\n", output );
    }
    printf( "\n" );
    if( !output )
    {
        printf( "Open the Tracy profiler and connect to this host:port (or it will auto-discover).\n" );
    }
    fflush( stdout );
}
static int FindPidsByName( const char* name, uint32_t* outPids, int maxPids )
{
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
    if( snap == INVALID_HANDLE_VALUE ) return 0;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof( pe );

    const size_t nameLen = strlen( name );
    const bool hasExe = nameLen > 4 && ( _stricmp( name + nameLen - 4, ".exe" ) == 0 );

    if( Process32First( snap, &pe ) )
    {
        do
        {
            bool match = ( _stricmp( pe.szExeFile, name ) == 0 );
            if( !match && !hasExe )
            {
                char withExe[MAX_PATH];
                snprintf( withExe, sizeof( withExe ), "%s.exe", name );
                match = ( _stricmp( pe.szExeFile, withExe ) == 0 );
            }
            if( match && count < maxPids )
            {
                outPids[count++] = pe.th32ProcessID;
            }
        } while( Process32Next( snap, &pe ) );
    }
    CloseHandle( snap );
    return count;
}

static int RunAttached( uint32_t pid, const char* output )
{
    if( !ProcessIsAlive( pid ) )
    {
        fprintf( stderr, "Process %u does not exist or is not accessible.\n", pid );
        return 1;
    }

    if( !CheckAdminPrivileges() )
    {
        fprintf( stderr, "Warning: tracy-monitor is not running as Administrator.\n"
                         "ETW profiling requires Administrator privileges. If profiling fails, please re-run as Administrator.\n\n" );
    }

    printf( "Attaching to process %u", pid );
    fflush( stdout );

    if( !tracy::InitExternalTarget( pid ) ) return 1;

    printf( " (%s)...\n", tracy::GetExternalTargetName() );
    fflush( stdout );

    tracy::StartupProfiler();
    if( tracy::IsSystemTracingFailed() )
    {
        fprintf( stderr, "The client failed to start ETW system tracing (ensure you are running as Administrator).\n" );
        tracy::ShutdownProfiler();
        return 1;
    }

    if( !VerifyClientListening() )
    {
        fprintf( stderr, "The client could not listen on port %d.\n", EffectivePort() );
        tracy::ShutdownProfiler();
        return 1;
    }

    PrintStartupReport( output );

    std::unique_ptr<tracy::Worker> worker;
    if( output )
    {
        worker = std::make_unique<tracy::Worker>( "127.0.0.1", (uint16_t)EffectivePort(), -1 );
        const char* reason = nullptr;
        if( !WaitForWorker( *worker, &reason ) )
        {
            fprintf( stderr, "Cannot capture to %s: %s.\n", output, reason );
            tracy::RestoreOutputBackup();
            tracy::ShutdownProfiler();
            return 1;
        }
    }

    while( !s_shouldQuit && ProcessIsAlive( pid ) )
    {
        PrintProgress( worker.get() );
        SleepMs( 100 );
    }

    if( ProgressLineActive( worker.get() ) ) printf( "\n" );

    if( s_shouldQuit )
    {
        printf( "\nShutting down profiler...\n" );
    }
    else
    {
        printf( "Target exited.\n" );
    }

    tracy::ShutdownProfiler();
    if( worker ) return SaveTrace( *worker, output );
    return 0;
}

static std::string BuildCommandLine( int argc, char** argv )
{
    std::string cmd;
    for( int i = 0; i < argc; i++ )
    {
        if( i > 0 ) cmd += ' ';
        const char* arg = argv[i];
        const bool needQuotes = strchr( arg, ' ' ) != nullptr || strchr( arg, '\t' ) != nullptr || *arg == '\0';
        if( needQuotes ) cmd += '"';
        for( const char* p = arg; *p; p++ )
        {
            if( *p == '"' ) cmd += "\\\"";
            else if( *p == '\\' && ( *(p + 1) == '"' || *(p + 1) == '\0' ) ) cmd += "\\\\";
            else cmd += *p;
        }
        if( needQuotes ) cmd += '"';
    }
    return cmd;
}

static int RunForked( int argc, char** argv, const char* output )
{
    if( !CheckAdminPrivileges() )
    {
        fprintf( stderr, "Warning: tracy-monitor is not running as Administrator.\n"
                         "ETW profiling requires Administrator privileges. If profiling fails, please re-run as Administrator.\n\n" );
    }

    std::string cmdLine = BuildCommandLine( argc, argv );

    STARTUPINFOA si = {};
    si.cb = sizeof( si );
    PROCESS_INFORMATION pi = {};

    std::vector<char> cmdBuf( cmdLine.begin(), cmdLine.end() );
    cmdBuf.push_back( '\0' );

    if( !CreateProcessA( nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi ) )
    {
        fprintf( stderr, "Unable to create process '%s': error %lu\n", argv[0], GetLastError() );
        return 2;
    }

    s_childProcess = pi.hProcess;

    if( !tracy::InitExternalTarget( pi.dwProcessId ) )
    {
        TerminateProcess( pi.hProcess, 1 );
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
        return 1;
    }

    printf( "Profiling '%s' (pid %u)...\n", tracy::GetExternalTargetName(), pi.dwProcessId );
    fflush( stdout );

    tracy::StartupProfiler();
    if( tracy::IsSystemTracingFailed() )
    {
        fprintf( stderr, "The client failed to start ETW system tracing (ensure you are running as Administrator).\n" );
        TerminateProcess( pi.hProcess, 1 );
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
        tracy::ShutdownProfiler();
        return 1;
    }

    if( !VerifyClientListening() )
    {
        fprintf( stderr, "The client could not listen on port %d.\n", EffectivePort() );
        TerminateProcess( pi.hProcess, 1 );
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
        tracy::ShutdownProfiler();
        return 1;
    }

    ResumeThread( pi.hThread );
    CloseHandle( pi.hThread );

    PrintStartupReport( output );

    std::unique_ptr<tracy::Worker> worker;
    if( output )
    {
        worker = std::make_unique<tracy::Worker>( "127.0.0.1", (uint16_t)EffectivePort(), -1 );
        const char* reason = nullptr;
        if( !WaitForWorker( *worker, &reason ) )
        {
            fprintf( stderr, "Cannot capture to %s: %s.\n", output, reason );
            tracy::RestoreOutputBackup();
            TerminateProcess( pi.hProcess, 1 );
            CloseHandle( pi.hProcess );
            tracy::ShutdownProfiler();
            return 1;
        }
    }

    for(;;)
    {
        if( s_shouldQuit ) break;
        PrintProgress( worker.get() );
        const DWORD res = WaitForSingleObject( pi.hProcess, 100 );
        if( res == WAIT_OBJECT_0 )
        {
            DWORD exitCode = 0;
            GetExitCodeProcess( pi.hProcess, &exitCode );
            printf( "Target exited with status %lu.\n", exitCode );
            break;
        }
    }

    if( ProgressLineActive( worker.get() ) ) printf( "\n" );

    if( s_shouldQuit && ProcessIsAlive( pi.dwProcessId ) )
    {
        printf( "\nStopping target and shutting down...\n" );
        TerminateProcess( pi.hProcess, 1 );
        WaitForSingleObject( pi.hProcess, 1000 );
    }

    CloseHandle( pi.hProcess );
    s_childProcess = NULL;
    tracy::ShutdownProfiler();
    if( worker ) return SaveTrace( *worker, output );
    return 0;
}

#endif // _WIN32

// --- CLI --------------------------------------------------------------------

static void PrintUsage( const char* progName )
{
    printf( "tracy-monitor %i.%i.%i / %s\n\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef );
    printf( "Profiles a process that was not built with the Tracy client.\n" );
    printf( "\n" );
    printf( "Usage: %s [OPTIONS] program [arguments...]\n", progName );
    printf( "       %s [OPTIONS] -p PID\n", progName );
    printf( "       %s [OPTIONS] -n NAME\n", progName );
    printf( "\n" );
    printf( "Options:\n" );
    printf( "  -p, --pid PID      Attach to an existing process (PID)\n" );
    printf( "  -n, --name NAME    Attach to an existing process by name; if several processes\n" );
    printf( "                     match, their PIDs are listed; use -p.\n" );
    printf( "  --hz N             Sampling frequency in Hz (default %d, range 1..1000000)\n", EffectiveSamplingHz() );
    printf( "  --port N           Listen port for Tracy servers (default 8086)\n" );
    printf( "  -o, --output FILE  Save the trace to FILE (.tracy) when the capture\n" );
    printf( "                     ends. While capturing, the monitor acts as the Tracy\n" );
    printf( "                     server itself, so the profiler GUI cannot connect\n" );
    printf( "  -f, --force        Replace the output file if it exists\n" );
    printf( "  -h, --help         Show this help message\n" );
    printf( "\n" );
    printf( "Exit codes: 0 on success; 1 when the target cannot be profiled\n" );
    printf( "(bad option, no matching process, permission or kernel refusal) or\n" );
    printf( "the trace cannot be saved; 2 when the monitor itself fails to start the\n" );
    printf( "target; 4 when the output file already exists (use -f to overwrite);\n" );
    printf( "5 when the output file cannot be opened or used.\n" );
    printf( "\n" );
#ifdef _WIN32
    printf( "Permission requirements:\n" );
    printf( "  ETW kernel sampling and context switch monitoring require running\n" );
    printf( "  tracy-monitor as Administrator.\n" );
#else
    printf( "Permission requirements (perf_event_paranoid / capabilities):\n" );
    printf( "  sampling + symbolication               ptrace READ access to the target:\n" );
    printf( "                                         same user (uid and gid) and dumpable,\n" );
    printf( "                                         or CAP_SYS_PTRACE (yama ptrace_scope\n" );
    printf( "                                         does not apply; launch mode's\n" );
    printf( "                                         PTRACE_TRACEME needs CAP_SYS_PTRACE at\n" );
    printf( "                                         scope 2 and is blocked at scope 3)\n" );
    printf( "  kernel frames in callchains            paranoid <= 1 or CAP_PERFMON\n" );
    printf( "  IPC / cache / branch statistics        hardware PMU counters must be\n" );
    printf( "                                         available (often restricted in VMs\n" );
    printf( "                                         and containers)\n" );
    printf( "  context switches / wait stacks         tracefs readable (root on most\n" );
    printf( "                                         systems) AND paranoid <= -1 or\n" );
    printf( "                                         CAP_PERFMON for the event open\n" );
    printf( "  power (RAPL) plot                      read access to\n" );
    printf( "                                         /sys/devices/virtual/powercap/intel-rapl\n" );
#endif
    printf( "\n" );
    printf( "Examples:\n" );
    printf( "  %s ./my_program arg1 arg2\n", progName );
    printf( "  %s -o trace.tracy -n my_program\n", progName );
    printf( "\n" );
    printf( "In launch mode the target is started controlled so profiling begins before\n" );
    printf( "its first instruction; quitting the monitor terminates the target. In attach\n" );
    printf( "mode (-p/-n) the target must already be running and is left running when\n" );
    printf( "the monitor quits.\n" );
}

int main( int argc, char** argv )
{
    auto progName = argv[0];
    InitTerminalDetection();

    if( argc < 2 )
    {
        PrintUsage( progName );
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler( ConsoleCtrlHandler, TRUE );
#else
    struct sigaction sa = {};
    sa.sa_handler = SignalHandler;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = 0;
    sigaction( SIGINT, &sa, nullptr );
    sigaction( SIGTERM, &sa, nullptr );
    sigaction( SIGHUP, &sa, nullptr );
    sigaction( SIGQUIT, &sa, nullptr );

    // external sampling opens one event per CPU for each existing thread
    // (attach) or per CPU (launch) per event type; raise the soft fd limit
    // so multithreaded targets fit (failing opens degrade gracefully).
    {
        struct rlimit rl = {};
        if( getrlimit( RLIMIT_NOFILE, &rl ) == 0 && rl.rlim_cur < 65536 )
        {
            const rlim_t want = rl.rlim_max > 65536 ? 65536 : rl.rlim_max;
            if( want > rl.rlim_cur )
            {
                rl.rlim_cur = want;
                if( setrlimit( RLIMIT_NOFILE, &rl ) != 0 )
                {
                    fprintf( stderr, "tracy-monitor: warning: could not raise RLIMIT_NOFILE (%s); very multithreaded targets may be only partially sampled.\n", strerror( errno ) );
                }
            }
        }
    }
#endif

    // TRACY_NO_SYS_TRACE / TRACY_NO_SAMPLING would silently disable the monitor's
    // primary data sources (IsSystemTracingFailed() stays false); refuse them up front.
    const char* noSysTrace = getenv( "TRACY_NO_SYS_TRACE" );
    const char* noSampling = getenv( "TRACY_NO_SAMPLING" );
    if( ( noSysTrace && noSysTrace[0] == '1' ) || ( noSampling && noSampling[0] == '1' ) )
    {
        if( noSysTrace && noSysTrace[0] == '1' )
        {
            fprintf( stderr, "tracy-monitor: TRACY_NO_SYS_TRACE=1 is set; the monitor requires system tracing. Unset the variable and retry.\n" );
        }
        if( noSampling && noSampling[0] == '1' )
        {
            fprintf( stderr, "tracy-monitor: TRACY_NO_SAMPLING=1 is set; the monitor requires callstack sampling. Unset the variable and retry.\n" );
        }
        return 1;
    }

    uint32_t attachPid = 0;
    char attachName[128] = {};
    bool wantAttach = false;
    const char* output = nullptr;
    bool overwrite = false;

    enum LongOption
    {
        OptHz = 256,
        OptPort,
    };
    static const struct option longOptions[] = {
        { "pid",    required_argument, nullptr, 'p' },
        { "name",   required_argument, nullptr, 'n' },
        { "hz",     required_argument, nullptr, OptHz },
        { "port",   required_argument, nullptr, OptPort },
        { "output", required_argument, nullptr, 'o' },
        { "force",  no_argument,       nullptr, 'f' },
        { "help",   no_argument,       nullptr, 'h' },
        { nullptr, 0, nullptr, 0 }
    };

    int c;
    while( ( c = getopt_long( argc, argv, "+p:n:o:fh", longOptions, nullptr ) ) != -1 )
    {
        switch( c )
        {
        case 'p':
            attachPid = (uint32_t)atoi( optarg );
            wantAttach = true;
            break;
        case 'n':
            if( strlen( optarg ) >= sizeof( attachName ) )
            {
                fprintf( stderr, "Process name too long (max %zu characters).\n", sizeof( attachName ) - 1 );
                return 1;
            }
            snprintf( attachName, sizeof( attachName ), "%s", optarg );
            wantAttach = true;
            break;
        case OptHz:
        {
            const int hz = atoi( optarg );
            if( hz < 1 || hz > 1000000 )
            {
                fprintf( stderr, "Invalid sample rate %s (range 1..1000000 Hz).\n", optarg );
                return 1;
            }
            char buf[16];
            snprintf( buf, sizeof( buf ), "%d", hz );
            SetEnv( "TRACY_SAMPLING_HZ", buf );
            break;
        }
        case OptPort:
        {
            const int port = atoi( optarg );
            if( port < 1 || port > 65535 )
            {
                fprintf( stderr, "Invalid port %s (range 1..65535).\n", optarg );
                return 1;
            }
            char buf[8];
            snprintf( buf, sizeof( buf ), "%d", port );
            SetEnv( "TRACY_PORT", buf );
            break;
        }
        case 'o':
            output = optarg;
            break;
        case 'f':
            overwrite = true;
            break;
        case 'h':
            PrintUsage( argv[0] );
            return 0;
        case '?':
            fprintf( stderr, "Unknown option. Use -h for help.\n" );
            return 1;
        }
    }

    // validate a pinned TRACY_PORT like --port: the client pins to any nonzero value
    // (single listen, no fallback), so a bad value would report a port never used.
    {
        const char* portEnv = getenv( "TRACY_PORT" );
        if( portEnv )
        {
            char* end = nullptr;
            const long port = strtol( portEnv, &end, 10 );
            if( end == portEnv || *end != '\0' || port < 1 || port > 65535 )
            {
                fprintf( stderr, "Invalid TRACY_PORT '%s' (expected a number in 1..65535); unset it or use --port.\n", portEnv );
                return 1;
            }
        }
    }

    ReservePortIfUnpinned();

    if( output )
    {
        const char* prepError = nullptr;
        const auto prep = tracy::PrepareOutputFile( output, overwrite, &prepError );
        if( prep == tracy::OutputPrep::Exists )
        {
            fprintf( stderr, "Output file %s already exists! Use -f to force overwrite.\n", output );
            return 4;
        }
        if( prep == tracy::OutputPrep::Unusable )
        {
            fprintf( stderr, "Cannot use output file: %s!\n", prepError );
            return 5;
        }
    }
    if( wantAttach )
    {
        if( attachName[0] )
        {
#ifdef _WIN32
            uint32_t pids[32] = {};
            const int numPids = FindPidsByName( attachName, pids, 32 );
#else
            pid_t pids[32] = {};
            const int numPids = FindPidsByComm( attachName, pids, 32 );
#endif
            if( numPids == 0 )
            {
                fprintf( stderr, "No process named '%s'.\n", attachName );
                return 1;
            }
            if( numPids > 1 )
            {
                fprintf( stderr, "Several processes named '%s':", attachName );
                for( int i=0; i<numPids; i++ ) fprintf( stderr, " %u", (uint32_t)pids[i] );
                fprintf( stderr, "\nUse -p PID to disambiguate.\n" );
                return 1;
            }
            return RunAttached( pids[0], output );
        }

        if( attachPid == 0 )
        {
            fprintf( stderr, "Invalid PID specified.\n" );
            return 1;
        }
        return RunAttached( attachPid, output );
    }

    argv += optind;
    argc -= optind;

    if( argc < 1 )
    {
        PrintUsage( progName );
        return 1;
    }

    return RunForked( argc, argv, output );
}
