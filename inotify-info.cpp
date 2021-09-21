/*
 * Copyright 2021 Michael Sartain
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>
#include <sys/sysmacros.h>

#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#include "inotify-info.h"
#include "lfqueue/lfqueue.h"

/*
 * TODO
 *  - Comments
 *  - Disable color
 *  - Check aarch64
 */

static int g_verbose = 0;
static size_t g_numthreads = 32;

/*
 * filename info
 */
struct filename_info_t
{
    ino64_t inode;  // Inode number
    dev_t dev;      // Device ID containing file
    std::string filename;
};

/*
 * inotify process info
 */
struct procinfo_t
{
    pid_t pid = 0;

    // Count of inotify watches and instances
    uint32_t watches = 0;
    uint32_t instances = 0;

    // This appname or pid found in command line?
    bool in_cmd_line = false;

    // Full executable path
    std::string executable;

    // Executable basename
    std::string appname;

    // Inotify fdset filenames
    std::vector< std::string > fdset_filenames;

    // Device id map -> set of inodes for that device id
    std::unordered_map< dev_t, std::unordered_set< ino64_t > > dev_map;
};

/*
 * thread info
 */
typedef std::unordered_map< ino64_t, std::unordered_set< dev_t > > inode_set_t;
typedef std::vector< class thread_info_t > thread_info_array_t;

class thread_info_t
{
public:
    thread_info_t();
    ~thread_info_t();

    void init( size_t thread_id, thread_info_array_t &thread_infos, inode_set_t &inode_set );

    void queue_directory( char *path );
    char *dequeue_directory();

    // Returns -1: queue empty, 0: open error, > 0 success
    int parse_dirqueue_entry();

    void add_filename( ino64_t inode, const char *path, const char *d_name, bool is_dir );

public:
    uint32_t num = 0;           // Thread number (0 for main thread)
    pthread_t pthread_id = 0;
    uint32_t scanned_dirs = 0;  // Total dirs scanned by this thread

    lfqueue_t dirqueue;

    // Array of threads. Index 0 is main thread.
    thread_info_array_t *pthread_infos = nullptr;

    // Map of all inotify inodes watched to the set of devices they are on
    const inode_set_t *pinode_set = nullptr;

    // Files found by this thread
    std::vector< filename_info_t > found_files;
};

/*
 * getdents64 syscall
 */
struct linux_dirent64
{
    uint64_t d_ino;          // Inode number
    int64_t d_off;           // Offset to next linux_dirent
    unsigned short d_reclen; // Length of this linux_dirent
    unsigned char d_type;    // File type
    char d_name[];           // Filename (null-terminated)
};

int sys_getdents64( int fd, char *dirp, int count )
{
    return syscall( SYS_getdents64, fd, dirp, count );
}

static double gettime()
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ( double )ts.tv_sec + ( double )ts.tv_nsec / 1e9;
}

void print_separator()
{
    printf( "%s%s%s\n", YELLOW, std::string( 78, '-' ).c_str(), RESET );
}

std::string string_formatv( const char *fmt, va_list ap )
{
    std::string str;
    int size = 512;

    for ( ;; )
    {
        str.resize( size );
        int n = vsnprintf( ( char * )str.c_str(), size, fmt, ap );

        if ( ( n > -1 ) && ( n < size ) )
        {
            str.resize( n );
            return str;
        }

        size = ( n > -1 ) ? ( n + 1 ) : ( size * 2 );
    }
}

std::string string_format( const char *fmt, ... )
{
    va_list ap;
    std::string str;

    va_start( ap, fmt );
    str = string_formatv( fmt, ap );
    va_end( ap );

    return str;
}

static std::string get_link_name( const char *Pathname )
{
    std::string Result;
    char Filename[ PATH_MAX + 1 ];

    ssize_t ret = readlink( Pathname, Filename, sizeof( Filename ) );
    if ( ( ret > 0 ) && ( ret < ( ssize_t )sizeof( Filename ) ) )
    {
        Filename[ ret ] = 0;
        Result = Filename;
    }
    return Result;
}

static uint64_t get_token_val( const char *line, const char *token )
{
    char *endptr;
    const char *str = strstr( line, token );

    return str ? strtoull( str + strlen( token ), &endptr, 16 ) : 0;
}

static uint32_t inotify_parse_fdinfo_file( procinfo_t &procinfo, const char *fdset_name )
{
    uint32_t watch_count = 0;

    FILE *fp = fopen( fdset_name, "r" );
    if ( fp )
    {
        char line_buf[ 256 ];

        procinfo.fdset_filenames.push_back( fdset_name );

        for ( ;; )
        {
            if ( !fgets( line_buf, sizeof( line_buf ), fp ) )
                break;

            if ( !strncmp( line_buf, "inotify ", 8 ) )
            {
                watch_count++;

                uint64_t inode_val = get_token_val( line_buf, "ino:" );
                uint64_t sdev_val = get_token_val( line_buf, "sdev:" );

                if ( inode_val )
                {
                    // https://unix.stackexchange.com/questions/645937/listing-the-files-that-are-being-watched-by-inotify-instances
                    //   Assuming that the sdev field is encoded according to Linux's so-called "huge
                    //   encoding", which uses 20 bits (instead of 8) for minor numbers, in bitwise
                    //   parlance the major number is sdev >> 20 while the minor is sdev & 0xfffff.
                    unsigned int major = sdev_val >> 20;
                    unsigned int minor = sdev_val & 0xfffff;

                    // Add inode to this device map
                    procinfo.dev_map[ makedev( major, minor ) ].insert( inode_val );
                }
            }
        }

        fclose( fp );
    }

    return watch_count;
}

static void inotify_parse_fddir( procinfo_t &procinfo )
{
    std::string filename = string_format( "/proc/%d/fd", procinfo.pid );

    DIR *dir_fd = opendir( filename.c_str() );
    if ( !dir_fd )
        return;

    for ( ;; )
    {
        struct dirent *dp_fd = readdir( dir_fd );
        if ( !dp_fd )
            break;

        if ( ( dp_fd->d_type == DT_LNK ) && isdigit( dp_fd->d_name[ 0 ] ) )
        {
            filename = string_format( "/proc/%d/fd/%s", procinfo.pid, dp_fd->d_name );
            filename = get_link_name( filename.c_str() );

            if ( filename == "anon_inode:inotify" )
            {
                filename = string_format( "/proc/%d/fdinfo/%s", procinfo.pid, dp_fd->d_name );

                uint32_t count = inotify_parse_fdinfo_file( procinfo, filename.c_str() );
                if ( count )
                {
                    procinfo.instances++;
                    procinfo.watches += count;
                }
            }
        }
    }

    closedir( dir_fd );
}

static void print_usage( const char *appname )
{
    printf( "Usage: %s [--threads=##] [appname | pid...]\n", appname );
    printf( "    [-vv]\n" );
    printf( "    [-?|-h|--help]\n" );

    exit( -1 );
}

static void parse_cmdline( int argc, char **argv, std::vector< std::string > &cmdline_applist )
{
    static struct option long_opts[] =
    {
        { "verbose", no_argument, 0, 0 },
        { "threads", required_argument, 0, 0 },
        { 0, 0, 0, 0 }
    };

    // Let's pick the number of processors online (with a max of 32) for a default.
    g_numthreads = std::min< uint32_t >( g_numthreads, sysconf( _SC_NPROCESSORS_ONLN ) );

    int c;
    int opt_ind = 0;
    while ( ( c = getopt_long( argc, argv, "m:s:?hv", long_opts, &opt_ind ) ) != -1 )
    {
        switch ( c )
        {
        case 0:
            if ( !strcasecmp( "verbose", long_opts[ opt_ind ].name ) )
                g_verbose++;
            else if ( !strcasecmp( "threads", long_opts[ opt_ind ].name ) )
                g_numthreads = atoi( optarg );
            break;
        case 'v':
            g_verbose++;
            break;
        case 'h':
        case '?':
        default:
            print_usage( argv[ 0 ] );
            break;
        }
    }

    for ( ; optind < argc; optind++ )
    {
        cmdline_applist.push_back( argv[ optind ] );
    }
}

thread_info_t::thread_info_t()
{
    lfqueue_init( &dirqueue );
}

thread_info_t::~thread_info_t()
{
    lfqueue_destroy( &dirqueue );
}

void thread_info_t::init( size_t thread_num, thread_info_array_t &thread_infos, inode_set_t &inode_set  )
{
    num = thread_num;

    pthread_infos = &thread_infos;
    pinode_set = &inode_set;
}

void thread_info_t::queue_directory( char *path )
{
    lfqueue_enq( &dirqueue, path );
}

char *thread_info_t::dequeue_directory()
{
    char *path = ( char * )lfqueue_deq( &dirqueue );

    if ( !path )
    {
        // Nothing on our queue, check queues on other threads
        for ( thread_info_t &thread_info : *pthread_infos )
        {
            path = ( char * )lfqueue_deq( &thread_info.dirqueue );
            if ( path )
                break;
        }
    }

    return path;
}

struct statx mystatx( const char *filename, unsigned int mask = 0 )
{
    struct statx statxbuf;
    int flags = AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC;

    if ( statx( 0, filename, flags, mask, &statxbuf ) == -1 )
    {
        printf( "ERROR: statx-ino( %s ) failed. Errno: %d\n", filename, errno );
        memset( &statxbuf, 0, sizeof( statxbuf ) );
    }

    return statxbuf;
}

void thread_info_t::add_filename( ino64_t inode, const char *path, const char *d_name, bool is_dir )
{
    auto it = pinode_set->find( inode );

    if ( it != pinode_set->end() )
    {
        const std::unordered_set< dev_t > &dev_set = it->second;

        std::string filename = std::string( path ) + d_name;
        struct statx statxbuf = mystatx( filename.c_str() );
        dev_t dev = makedev( statxbuf.stx_dev_major, statxbuf.stx_dev_minor );

        // Make sure the inode AND device ID match before adding.
        if ( dev_set.find( dev ) != dev_set.end() )
        {
            filename_info_t fname;

            fname.filename = is_dir ? filename + "/" : filename;
            fname.inode = inode;
            fname.dev = dev;

            found_files.push_back( fname );
        }
    }
}

static bool is_dot_dir( const char *dname )
{
    if ( dname[ 0 ] == '.' )
    {
        if ( !dname[ 1 ] )
            return true;

        if ( ( dname[ 1 ] == '.' ) && !dname[ 2 ] )
            return true;
    }

    return false;
}

// Returns -1: queue empty, 0: open error, > 0 success
int thread_info_t::parse_dirqueue_entry()
{
    char __attribute__( ( aligned( 16 ) ) ) buf[ 1024 ];

    char *path = dequeue_directory();
    if ( !path )
    {
        return -1;
    }

    int fd = open( path, O_RDONLY | O_DIRECTORY, 0 );
    if ( fd < 0 )
    {
        free( path );
        return 0;
    }

    scanned_dirs++;

    size_t pathlen = strlen( path );

    for ( ;; )
    {
        int ret = sys_getdents64( fd, buf, sizeof( buf ) );
        if ( ret < 0 )
        {
            printf( "ERROR: sys_getdents64 failed on '%s': %d errno:%d\n", path, ret, errno );
            break;
        }
        if ( ret == 0 )
            break;

        for ( int bpos = 0; bpos < ret; )
        {
            struct linux_dirent64 *dirp = ( struct linux_dirent64 * )( buf + bpos );
            const char *d_name = dirp->d_name;

            // DT_BLK      This is a block device.
            // DT_CHR      This is a character device.
            // DT_FIFO     This is a named pipe (FIFO).
            // DT_SOCK     This is a UNIX domain socket.
            // DT_UNKNOWN  The file type could not be determined.

            // DT_REG      This is a regular file.
            // DT_LNK      This is a symbolic link.
            if ( dirp->d_type == DT_REG || dirp->d_type == DT_LNK )
            {
                add_filename( dirp->d_ino, path, d_name, false );
            }
            // DT_DIR      This is a directory.
            else if ( dirp->d_type == DT_DIR )
            {
                if ( !is_dot_dir( d_name ) )
                {
                    add_filename( dirp->d_ino, path, d_name, true );

                    size_t len = strlen( d_name );
                    char *newpath = ( char * )malloc( pathlen + len + 2 );

                    if ( newpath )
                    {
                        strcpy( newpath, path );
                        strcpy( newpath + pathlen, d_name );
                        newpath[ pathlen + len ] = '/';
                        newpath[ pathlen + len + 1 ] = 0;

                        queue_directory( newpath );
                    }
                }
            }

            bpos += dirp->d_reclen;
        }
    }

    close( fd );
    free( path );
    return 1;
}

static bool is_proc_in_cmdline_applist( const procinfo_t &procinfo, std::vector< std::string > &cmdline_applist )
{
    for ( const std::string &str : cmdline_applist )
    {
        // Check if our command line string is a subset of this appname
        if ( strstr( procinfo.appname.c_str(), str.c_str() ) )
            return true;

        // Check if the PIDs match
        if ( atoll( str.c_str() ) == procinfo.pid )
            return true;
    }

    return false;
}

static bool init_inotify_proclist( std::vector< procinfo_t > &inotify_proclist )
{
    DIR *dir_proc = opendir( "/proc" );

    if ( !dir_proc )
    {
        printf( "ERROR: opendir /proc failed: %d\n", errno );
        return false;
    }

    for ( ;; )
    {
        struct dirent *dp_proc = readdir( dir_proc );
        if ( !dp_proc )
            break;

        if ( ( dp_proc->d_type == DT_DIR ) && isdigit( dp_proc->d_name[ 0 ] ) )
        {
            procinfo_t procinfo;

            procinfo.pid = atoll( dp_proc->d_name );

            std::string executable = string_format( "/proc/%d/exe", procinfo.pid );

            procinfo.executable = get_link_name( executable.c_str() );
            if ( !procinfo.executable.empty() )
            {
                procinfo.appname = basename( procinfo.executable.c_str() );

                inotify_parse_fddir( procinfo );

                if ( procinfo.watches )
                {
                    inotify_proclist.push_back( procinfo );
                }
            }
        }
    }

    closedir( dir_proc );
    return true;
}

static void print_inotify_proclist( std::vector< procinfo_t > &inotify_proclist )
{
    printf( "%s     Pid  App                        Watches   Instances%s\n", BCYAN, RESET );

    for ( procinfo_t &procinfo : inotify_proclist )
    {
        printf( "  % 7d %s%-30s%s %3u %3u\n", procinfo.pid, BYELLOW, procinfo.appname.c_str(), RESET, procinfo.watches, procinfo.instances );

        if ( g_verbose > 1 )
        {
            for ( std::string &fname : procinfo.fdset_filenames )
            {
                printf( "    %s%s%s\n", CYAN, fname.c_str(), RESET );
            }
        }

        if ( procinfo.in_cmd_line  )
        {
            for ( const auto &it1 : procinfo.dev_map )
            {
                dev_t dev = it1.first;

                printf( "%s[%u.%u]:%s", BGRAY, major( dev ), minor( dev ), RESET );
                for ( const auto &it2 : it1.second )
                {
                    std::string inode_device_str = string_format( "%lu", it2 );

                    printf( " %s%s%s", BGRAY, inode_device_str.c_str(), RESET );
                }
                printf( "\n" );
            }
        }
    }
}

static void *parse_dirqueue_threadproc( void *arg )
{
    thread_info_t *pthread_info = ( thread_info_t * )arg;

    for ( ;; )
    {
        // Loop until all the dequeue(s) fail
        if ( pthread_info->parse_dirqueue_entry() == -1 )
            break;
    }

    return nullptr;
}

static uint32_t find_files_in_inode_set( std::vector< procinfo_t > &inotify_proclist, std::vector< filename_info_t > &all_found_files )
{
    // Map of all inotify inodes watched to the set of devices they are on
    inode_set_t inode_set;
    // Array of threads. Index 0 is main thread.
    thread_info_array_t thread_infos;

    for ( procinfo_t &procinfo : inotify_proclist )
    {
        if ( !procinfo.in_cmd_line  )
            continue;

        for ( const auto &it1 : procinfo.dev_map )
        {
            dev_t dev = it1.first;
            for ( const auto &inode : it1.second )
            {
                inode_set[ inode ].insert( dev );
            }
        }
    }

    if ( inode_set.empty() )
        return 0;

    g_numthreads = std::max< size_t >( 1, g_numthreads );
    thread_infos.resize( g_numthreads );

    printf( "\n%sSearching '/' for listed inodes...%s (%lu threads)\n", BCYAN, RESET, g_numthreads );

    // Init main thread
    thread_infos[ 0 ].init( 0, thread_infos, inode_set );

    // Add root dir in case someone is watching it
    // Parse root to add some dirs for threads to chew on
    thread_infos[ 0 ].add_filename( mystatx( "/", STATX_INO ).stx_ino, "/", "", false );
    thread_infos[ 0 ].queue_directory( strdup( "/" ) );
    thread_infos[ 0 ].parse_dirqueue_entry();

    for ( size_t i = 1; i < g_numthreads; i++ )
    {
        thread_infos[ i ].init( i, thread_infos, inode_set );

        if ( pthread_create( &thread_infos[ i ].pthread_id, NULL, &parse_dirqueue_threadproc, &thread_infos[ i ] ) )
        {
            thread_infos[ i ].pthread_id = 0;
        }
    }

    // Put main thread to work
    parse_dirqueue_threadproc( &thread_infos[ 0 ] );

    for ( const thread_info_t &thread_info : thread_infos )
    {
        if ( g_verbose > 1 )
        {
            printf( "Waiting for thread #%u\n", thread_info.num );
        }

        if ( thread_info.pthread_id )
        {
            void *status = NULL;
            int rc = pthread_join( thread_info.pthread_id, &status );

            if ( g_verbose > 1 )
            {
                printf( "Thread #%u rc=%d status=%d\n", thread_info.num, rc, ( int )( intptr_t )status );
            }
        }
    }

    // Coalesce data from all our threads
    uint32_t total_scanned_dirs = 0;
    for ( const thread_info_t &thread_info : thread_infos )
    {
        total_scanned_dirs += thread_info.scanned_dirs;

        all_found_files.insert( all_found_files.end(),
                thread_info.found_files.begin(), thread_info.found_files.end() );

        if ( g_verbose > 1 )
        {
            printf( "Thread #%u: %u dirs, %zu files found\n",
                    thread_info.num, thread_info.scanned_dirs, thread_info.found_files.size() );
        }
    }

    struct
    {
        bool operator()( const filename_info_t &a, const filename_info_t &b ) const
        {
            if ( a.dev == b.dev )
                return a.inode < b.inode;
            return a.dev < b.dev;
        }
    } filename_info_less_func;

    std::sort( all_found_files.begin(), all_found_files.end(), filename_info_less_func );

    return total_scanned_dirs;
}

static uint32_t get_inotify_procfs_value( const char *basename )
{
    uint32_t interface_val = 0;
    std::string Filename = string_format( "/proc/sys/fs/inotify/%s", basename );

    FILE *fp = fopen( Filename.c_str(), "r" );
    if ( fp )
    {
        if ( fscanf( fp, "%u", &interface_val ) != 1 )
        {
            interface_val = 0;
        }
        fclose( fp );
    }

    return interface_val;
}

static void print_inotify_limits()
{
    uint32_t max_queued_events = get_inotify_procfs_value( "max_queued_events" );
    uint32_t max_user_instances = get_inotify_procfs_value( "max_user_instances" );
    uint32_t max_user_watches = get_inotify_procfs_value( "max_user_watches" );

    printf( "%sINotify Limits:%s\n", BCYAN, RESET );
    printf( "  max_queued_events:  %s%u%s\n", BGREEN, max_queued_events, RESET );
    printf( "  max_user_instances: %s%u%s\n", BGREEN, max_user_instances, RESET );
    printf( "  max_user_watches:   %s%u%s\n", BGREEN, max_user_watches, RESET );
}

int main( int argc, char *argv[] )
{
    std::vector< std::string > cmdline_applist;
    std::vector< procinfo_t > inotify_proclist;

    parse_cmdline( argc, argv, cmdline_applist );
    print_separator();

    print_inotify_limits();
    print_separator();

    if ( init_inotify_proclist( inotify_proclist ) )
    {
        uint32_t total_watches = 0;
        uint32_t total_instances = 0;
        std::vector< filename_info_t > all_found_files;

        for ( procinfo_t &procinfo : inotify_proclist )
        {
            procinfo.in_cmd_line = is_proc_in_cmdline_applist( procinfo, cmdline_applist );

            total_watches += procinfo.watches;
            total_instances += procinfo.instances;
        }

        print_inotify_proclist( inotify_proclist );
        print_separator();

        printf( "Total inotify Watches:   %s%u%s\n", BGREEN, total_watches, RESET );
        printf( "Total inotify Instances: %s%u%s\n", BGREEN, total_instances, RESET );
        print_separator();

        double search_time = gettime();
        uint32_t total_scanned_dirs = find_files_in_inode_set( inotify_proclist, all_found_files );
        if ( total_scanned_dirs )
        {
            search_time = gettime() - search_time;

            for ( const filename_info_t &fname_info : all_found_files )
            {
                printf( "%s%9lu%s [%u:%u] %s\n", BGREEN, fname_info.inode, RESET,
                        major( fname_info.dev ), minor( fname_info.dev ),
                        fname_info.filename.c_str() );
            }

            setlocale( LC_NUMERIC, "" );
            printf( "\n%'u dirs scanned (%.2f seconds)\n", total_scanned_dirs, search_time );
        }
    }

    return 0;
}
