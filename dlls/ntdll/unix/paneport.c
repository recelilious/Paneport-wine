/*
 * Wine's single-process mode for Paneport
 *
 * Copyright 2026 recelilious and the Paneport contributors
 * Copyright (C) 2002 Andreas Mohr
 * Copyright (C) 2002 Shachar Shemesh
 *
 * The SMBIOS structures and find_smbios_entry come from programs/wineboot/wineboot.c, and the
 * hardware keys of the boot step are adapted from it: the two copyright lines above are that
 * file's.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * In single-process mode, Wine runs inside the process of Paneport's native host, since an iOS
 * app may never create another process: the wineserver runs as a thread of the host, and each
 * Windows process as a group of threads. Nothing forks or execs, and no thread sends another a
 * signal. The host loads this library, finds the entry points of include/wine/paneport.h with
 * dlsym, and passes the table of its paneport/wine_bridge.h, through which Wine reaches the
 * host. This file holds those entry points, the start of a process on a thread of its own, the
 * boot step that takes the place of wineboot's process, and the thread control that takes the
 * place of the wineserver's SIGUSR1 and SIGQUIT.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
# include <mach/mach.h>
# include <TargetConditionals.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "unix_private.h"
#include "wine/asm.h"
#include "wine/debug.h"
#include "wine/paneport.h"
#include "wine/server_paneport.h"

WINE_DEFAULT_DEBUG_CHANNEL(process);

BOOL paneport_mode = FALSE;
int paneport_server_socket = -1;
unsigned int paneport_umask = 022;  /* read once, before the server thread starts */

static const pp_wine_bridge_t *bridge;
static pthread_mutex_t paneport_mutex = PTHREAD_MUTEX_INITIALIZER;
static BOOL session_started;
static BOOL process_launched;
static void *server_library;
static wine_paneport_server_connect_t server_lib_connect;
static wine_paneport_server_stop_t server_lib_stop;
static wine_paneport_server_terminate_t server_lib_terminate;

static uint64_t process_handle;          /* the host's handle of the session's process */
static unsigned int process_id;          /* its Windows id, once it started */
static LONG process_state;               /* its WINE_PANEPORT_PROCESS_ state, or 0 */
static DWORD process_exit_code;          /* its exit code, once it ended */
static int std_fds[3] = { -1, -1, -1 };  /* its standard handles until they are wrapped */
static char *program_path;               /* its program, as a Windows path */

/* The drive letter that the launch maps to the program's folder. */
#define PANEPORT_PROGRAM_DRIVE 'd'

/* The stack size of a process's first thread. The stack carries only the process's start-up
 * code: signal_start_thread then switches to the kernel stack in the thread's data. 8 MiB, the
 * size of a main thread's stack, gives that code the room that it has upstream on Linux. */
#define PANEPORT_FIRST_STACK_SIZE (8 * 1024 * 1024)


/***********************************************************************
 *           paneport_log
 *
 * Hands a line to the host's log. Never in a signal handler.
 */
static void paneport_log( int level, const char *format, ... ) __attribute__((format(printf,2,3)));
static void paneport_log( int level, const char *format, ... )
{
    char buffer[512];
    va_list args;

    if (!bridge) return;
    va_start( args, format );
    vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    bridge->log( level, buffer );
}


/***********************************************************************
 *           paneport_fatal
 *
 * Ends the app after a failure that the session cannot survive, since single-process mode
 * never calls exit: the host's dispatcher writes the crash record. The text also goes to
 * standard error, as upstream's fatal errors do, because the abort may come before the host
 * delivers its log line. The thread stops being a guest thread first, so that the dispatcher
 * does not hand the abort to Wine's own handler, which would make it an exception of the guest.
 */
void paneport_fatal( const char *text )
{
    fprintf( stderr, "wine: %s\n", text );
    if (bridge)
    {
        bridge->log( PP_WINE_LOG_ERROR, text );
        bridge->unmark_thread();
    }
    abort();
}


/***********************************************************************
 *           apply_environment
 *
 * Puts the session's variables into the process's environment, where Wine's Unix side reads
 * its settings with getenv, and from which a process takes its Windows environment.
 */
static BOOL apply_environment( const char *block )
{
    const char *entry, *value;
    char name[256];

    if (!block) return TRUE;
    for (entry = block; *entry; entry += strlen( entry ) + 1)
    {
        if (!(value = strchr( entry, '=' )) || value == entry || value - entry >= sizeof(name))
            return FALSE;
        memcpy( name, entry, value - entry );
        name[value - entry] = 0;
        if (setenv( name, value + 1, 1 )) return FALSE;
    }
    return TRUE;
}


/***********************************************************************
 *           prepare_prefix
 *
 * Makes what the prefix needs before the server starts, as setup_config_dir does for a new
 * one, without a drive for the host's root: drive_c, dosdevices and the c: link. It removes
 * every drive that maps the host's root, and the program's drive of an earlier launch.
 */
static BOOL prepare_prefix( int config_fd )
{
    char link[2048], name[3] = "a:";
    struct stat st;
    int devices_fd;
    ssize_t len;

    if (mkdirat( config_fd, "drive_c", 0777 ) == -1 && errno != EEXIST) return FALSE;
    if (mkdirat( config_fd, "dosdevices", 0777 ) == -1 && errno != EEXIST) return FALSE;
    if ((devices_fd = openat( config_fd, "dosdevices", O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1)
        return FALSE;
    if (fstatat( devices_fd, "c:", &st, AT_SYMLINK_NOFOLLOW ) == -1 &&
        symlinkat( "../drive_c", devices_fd, "c:" ) == -1)
    {
        close( devices_fd );
        return FALSE;
    }
    for (name[0] = 'a'; name[0] <= 'z'; name[0]++)
    {
        if ((len = readlinkat( devices_fd, name, link, sizeof(link) - 1 )) == -1) continue;
        link[len] = 0;
        if (!strcmp( link, "/" ) || name[0] == PANEPORT_PROGRAM_DRIVE) unlinkat( devices_fd, name, 0 );
    }
    close( devices_fd );
    return TRUE;
}


/***********************************************************************
 *           server_log
 */
static void server_log( int level, const char *text )
{
    if (bridge) bridge->log( level, text );
}


/***********************************************************************
 *           read_jit_pool
 *
 * Reads the regions of the host's JIT code pool through the bridge, once, and hands them to
 * the memory manager, which serves ARM64EC code from them from then on. A host without a pool
 * lists no region, and ARM64EC code is then mapped as it is without Paneport; an iOS device
 * has no such way, so a session without a pool does not start there.
 */
static BOOL read_jit_pool(void)
{
    pp_wine_jit_region_t regions[16];
    struct wine_jit_pool_region pool[ARRAY_SIZE(regions)];
    uint32_t i, count = 0;

    if (bridge->get_jit_pool( regions, ARRAY_SIZE(regions), &count ) || count > ARRAY_SIZE(regions))
    {
        paneport_log( PP_WINE_LOG_ERROR, "the host's JIT code pool could not be read (%u regions)", count );
        return FALSE;
    }
    if (!count)
    {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
        paneport_log( PP_WINE_LOG_ERROR, "the host has no JIT code pool, and nothing else can run code here" );
        return FALSE;
#else
        return TRUE;
#endif
    }
    for (i = 0; i < count; i++)
    {
        pool[i].exec  = (void *)(UINT_PTR)regions[i].exec_address;
        pool[i].write = (void *)(UINT_PTR)regions[i].write_address;
        pool[i].size  = regions[i].size;
    }
    if (!virtual_set_jit_pool( pool, count ))
    {
        paneport_log( PP_WINE_LOG_ERROR, "the host's JIT code pool of %u regions cannot be used", count );
        return FALSE;
    }
    return TRUE;
}


/***********************************************************************
 *           log_jit_pool
 *
 * Hands the counters of the JIT code pool to the host's log in one line, when the process
 * ends. Paneport's harness reads the line.
 */
static void log_jit_pool(void)
{
    struct wine_jit_pool_stats stats;

    virtual_get_jit_pool_stats( &stats );
    if (!stats.regions) return;
    paneport_log( PP_WINE_LOG_INFO, "jit pool: regions %llu pool_bytes %llu code_limit %llu "
                  "pool_fex_bytes %llu pool_fex_peak_bytes %llu pool_pe_bytes 0 allocations %llu "
                  "frees %llu refusals %llu exec_pages_outside_pool %llu exec_pages_denied %llu "
                  "image_exec_pages_outside_pool %llu",
                  (unsigned long long)stats.regions, (unsigned long long)stats.pool_bytes,
                  (unsigned long long)stats.code_limit, (unsigned long long)stats.code_bytes,
                  (unsigned long long)stats.code_peak_bytes, (unsigned long long)stats.allocations,
                  (unsigned long long)stats.frees, (unsigned long long)stats.refusals,
                  (unsigned long long)stats.exec_pages_outside, (unsigned long long)stats.exec_pages_denied,
                  (unsigned long long)stats.image_exec_peak_pages );
}


/***********************************************************************
 *           server_process_ended
 *
 * The server's report that a process ended, on the server's thread.
 */
static void server_process_ended( unsigned int id, unsigned int exit_code )
{
    if (!process_launched || (process_id && id != process_id)) return;
    log_jit_pool();
    process_exit_code = exit_code;
    InterlockedExchange( &process_state, WINE_PANEPORT_PROCESS_EXITED );
    bridge->process_exited( process_handle, exit_code );
}


/* The thread control of single-process mode. The wineserver asks for a thread to be
 * interrupted where it would send SIGUSR1 (suspension, context, system APCs) or SIGQUIT
 * (termination). The server's thread suspends the thread with a Mach call, notes what it asks
 * for, and moves the thread's pc to paneport_interrupt_trampoline, with its sp below the old
 * one and its red zone. The trampoline saves every register there and raises SIGUSR2 on its
 * own thread; usr2_handler puts the saved registers into the signal's context, so that Wine's
 * own SIGUSR1 or SIGQUIT handler runs on the state at which the thread stopped, as if that
 * signal had come there, and the handler's return goes back to it. While the thread had
 * SIGUSR2 blocked, in a section of Wine's that must not be interrupted, the SIGUSR2 waits in the
 * kernel until the thread unblocks it, as a real SIGUSR1 would.
 *
 * A thread suspended in a system call returns from it once resumed, and the kernel then
 * writes the call's results into x0, x1 and the flags, keeping the new pc, or moves the new pc
 * back one instruction to restart the call. The trampoline therefore starts one instruction
 * after a slot that tells the restart apart, and the registers it saves already hold the
 * call's results. */

#define PANEPORT_MAX_THREADS 4096

struct paneport_thread
{
    LONG               used;     /* the slot belongs to a thread */
    LONG               tid;      /* the thread's Unix tid as the server knows it, 0 when free */
    LONG               pending;  /* PANEPORT_IRQ_ bits */
    LONG               injected; /* the trampoline is on its way */
    LONG               deferred; /* a SIGUSR2 waits in the kernel for the thread to unblock it */
    ULONG64            pc;       /* the state at which the thread was stopped */
    ULONG64            sp;
    struct paneport_save_area *area; /* where the trampoline saved the registers */
    sigset_t           mask;     /* the thread's signal mask at that point */
};

static struct paneport_thread paneport_threads[PANEPORT_MAX_THREADS];


/***********************************************************************
 *           paneport_init_thread
 *
 * Makes the calling thread, which has just set its signal stack, a guest thread for the host's
 * signal dispatcher, and a thread that the server can interrupt. It runs before the thread
 * tells the server its Unix tid.
 */
void paneport_init_thread( struct thread_data *data, int unix_tid )
{
    struct paneport_thread *thread = NULL;
    unsigned int i;

#ifdef __APPLE__
    /* a send right of our own keeps the port's name from standing for another thread while the
     * server may still use it */
    mach_thread_self();
#endif
    for (i = 0; i < PANEPORT_MAX_THREADS; i++)
    {
        if (InterlockedCompareExchange( &paneport_threads[i].used, 1, 0 )) continue;
        thread = &paneport_threads[i];
        thread->pending = 0;
        thread->injected = 0;
        thread->deferred = 0;
        thread->area = NULL;
        WriteRelease( &thread->tid, unix_tid );
        break;
    }
    data->paneport = thread;
    if (!thread) paneport_log( PP_WINE_LOG_ERROR, "too many threads for thread control" );
    if (bridge && bridge->mark_thread( PP_THREAD_MARK_GUEST ))
        paneport_log( PP_WINE_LOG_ERROR, "the host could not mark a thread as a guest thread" );
}


/***********************************************************************
 *           paneport_exit_thread
 *
 * Undoes paneport_init_thread when the thread ends, before it closes its server pipes.
 */
void paneport_exit_thread( struct thread_data *data )
{
    struct paneport_thread *thread = data->paneport;

    if (thread)
    {
        WriteRelease( &thread->tid, 0 );
        data->paneport = NULL;
        WriteRelease( &thread->used, 0 );
    }
    if (bridge) bridge->unmark_thread();
}


#if defined(__APPLE__) && defined(__aarch64__)

extern void paneport_interrupt_restart(void);
void paneport_interrupt_entry( struct paneport_save_area *area );


/***********************************************************************
 *           find_thread
 */
static struct paneport_thread *find_thread( unsigned int tid )
{
    unsigned int i;

    if (!tid) return NULL;
    for (i = 0; i < PANEPORT_MAX_THREADS; i++)
        if ((unsigned int)ReadAcquire( &paneport_threads[i].tid ) == tid) return &paneport_threads[i];
    return NULL;
}

/* The trampoline's entry is paneport_interrupt_restart + 4; a restarted system call comes back
 * to the slot before it. Each path saves x0 and x1 first, then the flag of the restart. */
__ASM_GLOBAL_FUNC( paneport_interrupt_restart,
                   "b 2f\n\t"
                   "stp x0, x1, [sp]\n\t"
                   "mov x0, #0\n\t"
                   "b 3f\n"
                   "2:\tstp x0, x1, [sp]\n\t"
                   "mov x0, #1\n"
                   "3:\tstr x0, [sp, #0x100]\n\t"
                   "stp x2, x3, [sp, #0x10]\n\t"
                   "stp x4, x5, [sp, #0x20]\n\t"
                   "stp x6, x7, [sp, #0x30]\n\t"
                   "stp x8, x9, [sp, #0x40]\n\t"
                   "stp x10, x11, [sp, #0x50]\n\t"
                   "stp x12, x13, [sp, #0x60]\n\t"
                   "stp x14, x15, [sp, #0x70]\n\t"
                   "stp x16, x17, [sp, #0x80]\n\t"
                   "stp x18, x19, [sp, #0x90]\n\t"
                   "stp x20, x21, [sp, #0xa0]\n\t"
                   "stp x22, x23, [sp, #0xb0]\n\t"
                   "stp x24, x25, [sp, #0xc0]\n\t"
                   "stp x26, x27, [sp, #0xd0]\n\t"
                   "stp x28, x29, [sp, #0xe0]\n\t"
                   "str x30, [sp, #0xf0]\n\t"
                   "mrs x0, nzcv\n\t"
                   "str x0, [sp, #0xf8]\n\t"
                   "mrs x0, fpcr\n\t"
                   "str x0, [sp, #0x108]\n\t"
                   "mrs x0, fpsr\n\t"
                   "str x0, [sp, #0x110]\n\t"
                   "stp q0, q1, [sp, #0x120]\n\t"
                   "stp q2, q3, [sp, #0x140]\n\t"
                   "stp q4, q5, [sp, #0x160]\n\t"
                   "stp q6, q7, [sp, #0x180]\n\t"
                   "stp q8, q9, [sp, #0x1a0]\n\t"
                   "stp q10, q11, [sp, #0x1c0]\n\t"
                   "stp q12, q13, [sp, #0x1e0]\n\t"
                   "stp q14, q15, [sp, #0x200]\n\t"
                   "stp q16, q17, [sp, #0x220]\n\t"
                   "stp q18, q19, [sp, #0x240]\n\t"
                   "stp q20, q21, [sp, #0x260]\n\t"
                   "stp q22, q23, [sp, #0x280]\n\t"
                   "stp q24, q25, [sp, #0x2a0]\n\t"
                   "stp q26, q27, [sp, #0x2c0]\n\t"
                   "stp q28, q29, [sp, #0x2e0]\n\t"
                   "stp q30, q31, [sp, #0x300]\n\t"
                   "mov x0, sp\n\t"
                   "bl " __ASM_NAME("paneport_interrupt_entry") "\n\t"
                   "brk #0xf00f" )


/***********************************************************************
 *           paneport_interrupt_entry
 *
 * Called by the trampoline on the interrupted thread, with every register saved. It raises
 * SIGUSR2, whose handler never comes back here.
 */
void paneport_interrupt_entry( struct paneport_save_area *area )
{
    struct thread_data *data = get_thread_data();
    struct paneport_thread *thread = data ? data->paneport : NULL;
    sigset_t set;

    if (!thread) abort();
    thread->area = area;
    sigemptyset( &set );
    sigaddset( &set, SIGUSR2 );
    pthread_sigmask( SIG_UNBLOCK, &set, &thread->mask );
    raise( SIGUSR2 );
    abort();
}


/***********************************************************************
 *           paneport_interrupt_thread
 *
 * The server's interruption of a thread, on the server's thread. It never waits for the
 * thread, allocates nothing and takes no lock while the thread is suspended.
 */
static int paneport_interrupt_thread( unsigned int tid, int sig )
{
    struct paneport_thread *thread;
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    LONG bit;

    if (sig == SIGQUIT) bit = PANEPORT_IRQ_QUIT;
    else if (sig == SIGUSR1) bit = PANEPORT_IRQ_USR1;
    else return 1;  /* such as SIGINT for a console's Ctrl+C, which has no console here */

    if (!(thread = find_thread( tid ))) return 0;
    if (thread_suspend( tid ) != KERN_SUCCESS) return 0;
    if (find_thread( tid ) != thread)
    {
        thread_resume( tid );
        return 0;
    }
    InterlockedOr( &thread->pending, bit );
    if (!thread->injected)
    {
        thread_abort_safely( tid );
        if (thread_get_state( tid, ARM_THREAD_STATE64, (thread_state_t)&state, &count ))
            goto failed;
        thread->pc = arm_thread_state64_get_pc( state );
        thread->sp = arm_thread_state64_get_sp( state );
        arm_thread_state64_set_sp( state, (thread->sp - 128 - sizeof(struct paneport_save_area)) & ~(ULONG64)15 );
        arm_thread_state64_set_pc_fptr( state, (char *)paneport_interrupt_restart + 4 );
        if (thread_set_state( tid, ARM_THREAD_STATE64, (thread_state_t)&state, ARM_THREAD_STATE64_COUNT ))
            goto failed;
        thread->injected = 1;
    }
    thread_resume( tid );
    return 1;

failed:
    /* a thread whose state cannot be read or written is ending */
    __atomic_fetch_and( &thread->pending, ~bit, __ATOMIC_SEQ_CST );
    thread_resume( tid );
    return 0;
}

#else

static int paneport_interrupt_thread( unsigned int tid, int sig )
{
    return 0;
}

#endif


/***********************************************************************
 *           paneport_take_area
 *
 * Takes the registers that the trampoline saved, with the stopped pc and sp and the signal
 * mask, or returns NULL. On the interrupted thread, in its SIGUSR2 handler.
 */
struct paneport_save_area *paneport_take_area( struct thread_data *data, ULONG64 *pc, ULONG64 *sp,
                                               sigset_t *mask )
{
    struct paneport_thread *thread = data->paneport;
    struct paneport_save_area *area;

    if (!thread || !(area = thread->area)) return NULL;
    thread->area = NULL;
    *pc = thread->pc;
    *sp = thread->sp;
    *mask = thread->mask;
    WriteRelease( &thread->injected, 0 );
    return area;
}


/***********************************************************************
 *           paneport_defer
 *
 * Notes that the calling thread's SIGUSR2 handler raised a SIGUSR2 that waits in the kernel
 * until the thread unblocks it.
 */
void paneport_defer( struct thread_data *data )
{
    struct paneport_thread *thread = data->paneport;

    if (thread) WriteRelease( &thread->deferred, 1 );
}


/***********************************************************************
 *           paneport_take_pending
 *
 * Takes the PANEPORT_IRQ_ bits that wait for the calling thread, and whether the SIGUSR2 that
 * brings them is one that paneport_defer noted, which carries nothing else.
 */
LONG paneport_take_pending( struct thread_data *data, BOOL *deferred )
{
    struct paneport_thread *thread = data->paneport;

    *deferred = FALSE;
    if (!thread) return 0;
    *deferred = InterlockedExchange( &thread->deferred, 0 );
    return InterlockedExchange( &thread->pending, 0 );
}


/***********************************************************************
 *           paneport_set_guest_fault_handler
 */
int paneport_set_guest_fault_handler( pp_guest_fault_handler_t handler )
{
    return bridge->set_guest_fault_handler( handler, NULL );
}


/***********************************************************************
 *           paneport_get_std_fds
 *
 * The descriptors of the process's standard handles, which get_initial_console wraps.
 */
void paneport_get_std_fds( int fds[3] )
{
    memcpy( fds, std_fds, sizeof(std_fds) );
}


/***********************************************************************
 *           paneport_close_std_fds
 *
 * Closes the launch's copies once the server holds its own.
 */
void paneport_close_std_fds(void)
{
    unsigned int i;

    for (i = 0; i < 3; i++)
    {
        if (std_fds[i] != -1) close( std_fds[i] );
        std_fds[i] = -1;
    }
}


/***********************************************************************
 *           paneport_initial_directory
 *
 * The process's first current folder, as an NT path with a final backslash: its program's
 * folder, as a launcher would choose it.
 */
WCHAR *paneport_initial_directory(void)
{
    const char *end;
    WCHAR *ret;
    size_t len;
    DWORD count;

    if (!program_path || !(end = strrchr( program_path, '\\' ))) return NULL;
    len = end - program_path + 1;
    if (!(ret = malloc( (len + 5) * sizeof(WCHAR) ))) return NULL;
    ascii_to_unicode( ret, "\\??\\", 4 );
    /* a character of more than one byte gives one WCHAR, or two, but never more than its bytes */
    count = ntdll_umbstowcs( program_path, len, ret + 4, len );
    ret[count + 4] = 0;
    return ret;
}


/***********************************************************************
 *           paneport_process_started
 *
 * On the process's first thread, once the server knows the process and before its PE side
 * runs.
 */
void paneport_process_started(void)
{
    process_id = HandleToULong( NtCurrentTeb()->ClientId.UniqueProcess );
    InterlockedCompareExchange( &process_state, WINE_PANEPORT_PROCESS_RUNNING, WINE_PANEPORT_PROCESS_STARTING );
    bridge->process_started( process_handle );
}


/***********************************************************************
 *           create_volatile_environment
 *
 * The values of HKCU\Volatile Environment that wineboot writes at every boot, which the
 * process's environment takes: the user's profile folders and name.
 */
static void create_volatile_environment(void)
{
    static const char *names[] = { "APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOMEDRIVE",
                                   "HOMEPATH", "USERNAME", "CLIENTNAME", "SESSIONNAME", "HOMESHARE" };
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    char path[512], value[512];
    WCHAR pathW[512], nameW[32], valueW[512];
    DWORD i, len = sizeof(sid_data);
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE key;
    SID *sid;

    if (!user_name) return;
    if (NtQueryInformationToken( GetCurrentThreadEffectiveToken(), TokenUser, sid_data, len, &len ))
        return;
    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    len = snprintf( path, sizeof(path), "\\Registry\\User\\S-%u-%u", sid->Revision,
                    MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5], sid->IdentifierAuthority.Value[4] ),
                              MAKEWORD( sid->IdentifierAuthority.Value[3], sid->IdentifierAuthority.Value[2] )));
    for (i = 0; i < sid->SubAuthorityCount; i++)
        len += snprintf( path + len, sizeof(path) - len, "-%u", sid->SubAuthority[i] );
    len += snprintf( path + len, sizeof(path) - len, "\\Volatile Environment" );
    ascii_to_unicode( pathW, path, len + 1 );
    init_unicode_string( &name, pathW );
    InitializeObjectAttributes( &attr, &name, OBJ_CASE_INSENSITIVE, 0, NULL );
    if (NtCreateKey( &key, KEY_ALL_ACCESS, &attr, 0, NULL, REG_OPTION_VOLATILE, NULL ))
    {
        WARN( "cannot create the volatile environment key\n" );
        return;
    }

    for (i = 0; i < ARRAY_SIZE(names); i++)
    {
        switch (i)
        {
        case 0: snprintf( value, sizeof(value), "C:\\users\\%s\\AppData\\Roaming", user_name ); break;
        case 1: snprintf( value, sizeof(value), "C:\\users\\%s\\AppData\\Local", user_name ); break;
        case 2: snprintf( value, sizeof(value), "C:\\users\\%s", user_name ); break;
        case 3: snprintf( value, sizeof(value), "C:" ); break;
        case 4: snprintf( value, sizeof(value), "\\users\\%s", user_name ); break;
        case 5: snprintf( value, sizeof(value), "%s", user_name ); break;
        case 6:
        case 7: snprintf( value, sizeof(value), "Console" ); break;
        default: value[0] = 0; break;
        }
        ascii_to_unicode( nameW, names[i], strlen( names[i] ) + 1 );
        init_unicode_string( &name, nameW );
        len = ntdll_umbstowcs( value, strlen( value ) + 1, valueW, ARRAY_SIZE(valueW) );
        NtSetValueKey( key, &name, 0, REG_SZ, valueW, len * sizeof(WCHAR) );
    }
    NtClose( key );
}


/***********************************************************************
 *           create_drive_links
 *
 * Publishes the prefix's drive letters as \??\X: symbolic links, which GetLogicalDrives lists,
 * as mountmgr would. File access goes through dosdevices and needs no link.
 */
static void create_drive_links(void)
{
    char devices[2048], name[3] = "a:", link[16], target[64];
    WCHAR linkW[16], targetW[64];
    UNICODE_STRING link_str, target_str;
    OBJECT_ATTRIBUTES attr;
    unsigned int volume = 0;
    struct stat st;
    HANDLE handle;
    NTSTATUS status;

    snprintf( devices, sizeof(devices) - 3, "%s/dosdevices/", config_dir );
    for (name[0] = 'a'; name[0] <= 'z'; name[0]++)
    {
        size_t len = strlen( devices );

        strcpy( devices + len, name );
        if (lstat( devices, &st ) == -1)
        {
            devices[len] = 0;
            continue;
        }
        devices[len] = 0;
        snprintf( link, sizeof(link), "\\??\\%c:", name[0] - 'a' + 'A' );
        snprintf( target, sizeof(target), "\\Device\\HarddiskVolume%u", ++volume );
        ascii_to_unicode( linkW, link, strlen( link ) + 1 );
        ascii_to_unicode( targetW, target, strlen( target ) + 1 );
        init_unicode_string( &link_str, linkW );
        init_unicode_string( &target_str, targetW );
        InitializeObjectAttributes( &attr, &link_str, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT | OBJ_OPENIF, 0, NULL );
        status = NtCreateSymbolicLinkObject( &handle, SYMBOLIC_LINK_ALL_ACCESS, &attr, &target_str );
        if (!NT_SUCCESS(status)) WARN( "cannot create %s: %08x\n", link, (int)status );
        else NtClose( handle );
    }
}


/***********************************************************************
 *           set_shared_data_version
 *
 * The version fields of the shared user data, which wineboot sets.
 */
static void set_shared_data_version(void)
{
    static const char nameA[] = "\\KernelObjects\\__wine_user_shared_data";
    static const char rootA[] = "C:\\windows";
    WCHAR nameW[ARRAY_SIZE(nameA)];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    KUSER_SHARED_DATA *data;
    HANDLE section;
    int fd, needs_close;

    ascii_to_unicode( nameW, nameA, ARRAY_SIZE(nameA) );
    init_unicode_string( &name, nameW );
    InitializeObjectAttributes( &attr, &name, 0, 0, NULL );
    if (NtOpenSection( &section, SECTION_ALL_ACCESS, &attr )) return;
    if (server_get_unix_fd( section, 0, &fd, &needs_close, NULL, NULL ))
    {
        NtClose( section );
        return;
    }
    data = mmap( NULL, sizeof(*data), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (needs_close) close( fd );
    NtClose( section );
    if (data == MAP_FAILED) return;

    /* Wine's default version, Windows 10 */
    data->NtBuildNumber      = 19045;
    data->NtProductType      = VER_NT_WORKSTATION;
    data->ProductTypeIsValid = TRUE;
    data->NtMajorVersion     = 10;
    data->NtMinorVersion     = 0;
    data->SuiteMask          = VER_SUITE_SINGLEUSERTS;
    ascii_to_unicode( data->NtSystemRoot, rootA, ARRAY_SIZE(rootA) );
    munmap( data, sizeof(*data) );
}


/* The hardware keys that wineboot writes at every boot, from the SMBIOS table that ntdll makes
 * (create_hardware_registry_keys in programs/wineboot/wineboot.c). The keys under
 * HKLM\Hardware are volatile, so a template prefix never holds them, and programs read them:
 * FEX takes the host's processor from CentralProcessor\0 and ends the process without it. */

#define RSMB (('R' << 24) | ('S' << 16) | ('M' << 8) | 'B')

#pragma pack(push,1)

struct smbios_prologue
{
    BYTE  calling_method;
    BYTE  major_version;
    BYTE  minor_version;
    BYTE  revision;
    DWORD length;
};

enum smbios_type
{
    SMBIOS_TYPE_BIOS = 0,
    SMBIOS_TYPE_SYSTEM = 1,
    SMBIOS_TYPE_BASEBOARD = 2,
    SMBIOS_TYPE_PROCESSOR = 4,
    SMBIOS_TYPE_PROCESSOR_ADDITIONAL_INFO = 44,
};

struct smbios_header
{
    BYTE type;
    BYTE length;
    WORD handle;
};

struct smbios_baseboard
{
    struct smbios_header hdr;
    BYTE                 vendor;
    BYTE                 product;
    BYTE                 version;
    BYTE                 serial;
};

struct smbios_bios
{
    struct smbios_header hdr;
    BYTE                 vendor;
    BYTE                 version;
    WORD                 start;
    BYTE                 date;
    BYTE                 size;
    UINT64               characteristics;
    BYTE                 characteristics_ext[2];
    BYTE                 system_bios_major_release;
    BYTE                 system_bios_minor_release;
    BYTE                 ec_firmware_major_release;
    BYTE                 ec_firmware_minor_release;
};

struct smbios_system
{
    struct smbios_header hdr;
    BYTE                 vendor;
    BYTE                 product;
    BYTE                 version;
    BYTE                 serial;
    BYTE                 uuid[16];
    BYTE                 wake_up_type;
    BYTE                 sku;
    BYTE                 family;
};

struct smbios_processor
{
    struct smbios_header hdr;
    BYTE                 socket;
    BYTE                 type;
    BYTE                 family;
    BYTE                 vendor;
    ULONGLONG            id;
    BYTE                 version;
    BYTE                 voltage;
    WORD                 clock;
    WORD                 max_speed;
    WORD                 cur_speed;
    BYTE                 status;
    BYTE                 upgrade;
    WORD                 l1cache;
    WORD                 l2cache;
    WORD                 l3cache;
    BYTE                 serial;
    BYTE                 asset_tag;
    BYTE                 part_number;
    BYTE                 core_count;
    BYTE                 core_enabled;
    BYTE                 thread_count;
    WORD                 characteristics;
    WORD                 family2;
    WORD                 core_count2;
    WORD                 core_enabled2;
    WORD                 thread_count2;
};

struct smbios_processor_specific_block
{
    BYTE length;
    BYTE processor_type;
    BYTE data[];
};

struct smbios_processor_additional_info
{
    struct smbios_header hdr;
    WORD ref_handle;
    struct smbios_processor_specific_block info_block;
};

struct smbios_wine_core_id_regs_arm64
{
    WORD num_regs;
    struct smbios_wine_id_reg_value_arm64
    {
        WORD reg;
        UINT64 value;
    } regs[];
};

#pragma pack(pop)


/***********************************************************************
 *           get_smbios_table
 *
 * The SMBIOS table with its prologue, as GetSystemFirmwareTable returns it, or NULL.
 */
static char *get_smbios_table( UINT *len )
{
    SYSTEM_FIRMWARE_TABLE_INFORMATION *info;
    ULONG size = sizeof(*info), needed = 0;
    NTSTATUS status;
    char *ret;

    for (;;)
    {
        if (!(info = calloc( 1, size ))) return NULL;
        info->ProviderSignature = RSMB;
        info->Action = SystemFirmwareTable_Get;
        info->TableID = 0;
        info->TableBufferLength = size - FIELD_OFFSET( SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer );
        status = NtQuerySystemInformation( SystemFirmwareTableInformation, info, size, &needed );
        if (status != STATUS_BUFFER_TOO_SMALL || needed <= size) break;
        free( info );
        size = needed;
    }
    if (status || !(ret = malloc( info->TableBufferLength )))
    {
        free( info );
        return NULL;
    }
    memcpy( ret, info->TableBuffer, info->TableBufferLength );
    *len = info->TableBufferLength;
    free( info );
    return ret;
}


/***********************************************************************
 *           find_smbios_entry
 */
static const struct smbios_header *find_smbios_entry( enum smbios_type type, unsigned int index,
                                                      const char *buf, UINT len )
{
    const char *ptr, *start;
    const struct smbios_prologue *prologue;
    const struct smbios_header *hdr;

    if (len < sizeof(struct smbios_prologue)) return NULL;
    prologue = (const struct smbios_prologue *)buf;
    if (prologue->length > len - sizeof(*prologue) || prologue->length < sizeof(*hdr)) return NULL;

    start = (const char *)(prologue + 1);
    hdr = (const struct smbios_header *)start;

    for (;;)
    {
        if ((const char *)hdr - start >= prologue->length - sizeof(*hdr)) return NULL;
        if (!hdr->length) return NULL;

        if (hdr->type == type)
        {
            if ((const char *)hdr - start + hdr->length > prologue->length) return NULL;
            if (!index--) return hdr;
        }
        /* skip other entries and their strings */
        for (ptr = (const char *)hdr + hdr->length; ptr - buf < len && *ptr; ptr++)
        {
            for (; ptr - buf < len; ptr++) if (!*ptr) break;
        }
        if (ptr == (const char *)hdr + hdr->length) ptr++;
        hdr = (const struct smbios_header *)(ptr + 1);
    }
}


/***********************************************************************
 *           get_smbios_string
 *
 * The string with this number of the entry whose strings start at offset, or NULL.
 */
static const char *get_smbios_string( BYTE id, const char *buf, UINT offset, UINT buflen )
{
    const char *ptr;
    UINT i = 0;

    if (!id || offset >= buflen) return NULL;
    for (ptr = buf + offset; ptr - buf < buflen && *ptr; ptr++)
    {
        if (++i == id) return ptr;
        for (; ptr - buf < buflen; ptr++) if (!*ptr) break;
    }
    return NULL;
}


/***********************************************************************
 *           create_one_key
 */
static HANDLE create_one_key( HANDLE root, const char *nameA, ULONG options )
{
    WCHAR nameW[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE key;

    if (strlen( nameA ) >= ARRAY_SIZE(nameW)) return 0;
    ascii_to_unicode( nameW, nameA, strlen( nameA ) + 1 );
    init_unicode_string( &name, nameW );
    InitializeObjectAttributes( &attr, &name, OBJ_CASE_INSENSITIVE, root, NULL );
    if (NtCreateKey( &key, KEY_ALL_ACCESS, &attr, 0, NULL, options, NULL )) return 0;
    return key;
}


/***********************************************************************
 *           create_key
 *
 * Creates or opens a key below root, or below HKLM for a root of 0. The server makes only the
 * last level of a path, so a path whose parents are missing is made one level at a time, as
 * RegCreateKeyExW does; each level that this makes is volatile unless told otherwise.
 */
static HANDLE create_key( HANDLE root, const char *path, BOOL is_volatile )
{
    ULONG options = is_volatile ? REG_OPTION_VOLATILE : 0;
    HANDLE machine = 0, key, parent;
    const char *start = path, *end;
    char part[64];
    size_t len;

    if (!root && !(root = machine = create_one_key( 0, "\\Registry\\Machine", 0 ))) return 0;

    if (!(key = create_one_key( root, path, options )))
    {
        parent = root;
        for (;;)
        {
            end = strchr( start, '\\' );
            len = end ? (size_t)(end - start) : strlen( start );
            key = 0;
            if (len < sizeof(part))
            {
                memcpy( part, start, len );
                part[len] = 0;
                key = create_one_key( parent, part, options );
            }
            if (parent != root) NtClose( parent );
            if (!key || !end) break;
            parent = key;
            start = end + 1;
        }
    }
    if (machine) NtClose( machine );
    return key;
}


static void set_value( HANDLE key, const char *nameA, ULONG type, const void *data, ULONG size )
{
    WCHAR nameW[64];
    UNICODE_STRING name;

    ascii_to_unicode( nameW, nameA, strlen( nameA ) + 1 );
    init_unicode_string( &name, nameW );
    NtSetValueKey( key, &name, 0, type, data, size );
}


static void set_string_value( HANDLE key, const char *name, const char *value )
{
    WCHAR valueW[256];
    DWORD len = min( strlen( value ), ARRAY_SIZE(valueW) - 1 );

    len = ntdll_umbstowcs( value, len, valueW, ARRAY_SIZE(valueW) - 1 );
    valueW[len] = 0;
    set_value( key, name, REG_SZ, valueW, (len + 1) * sizeof(WCHAR) );
}


static void set_dword_value( HANDLE key, const char *name, DWORD value )
{
    set_value( key, name, REG_DWORD, &value, sizeof(value) );
}


static void set_smbios_string( HANDLE key, const char *name, BYTE id, const char *buf, UINT offset,
                               UINT len )
{
    const char *str = get_smbios_string( id, buf, offset, len );

    set_string_value( key, name, str ? str : "" );
}


/***********************************************************************
 *           create_bios_values
 */
static void create_bios_values( HANDLE bios_key, const char *buf, UINT len )
{
    const struct smbios_header *hdr;
    UINT offset;

    if ((hdr = find_smbios_entry( SMBIOS_TYPE_BASEBOARD, 0, buf, len )))
    {
        const struct smbios_baseboard *baseboard = (const struct smbios_baseboard *)hdr;

        offset = (const char *)baseboard - buf + baseboard->hdr.length;
        set_smbios_string( bios_key, "BaseBoardManufacturer", baseboard->vendor, buf, offset, len );
        set_smbios_string( bios_key, "BaseBoardProduct", baseboard->product, buf, offset, len );
        set_smbios_string( bios_key, "BaseBoardVersion", baseboard->version, buf, offset, len );
    }
    if ((hdr = find_smbios_entry( SMBIOS_TYPE_BIOS, 0, buf, len )))
    {
        const struct smbios_bios *bios = (const struct smbios_bios *)hdr;
        BOOL releases = bios->hdr.length >= 0x18;

        offset = (const char *)bios - buf + bios->hdr.length;
        set_smbios_string( bios_key, "BIOSVendor", bios->vendor, buf, offset, len );
        set_smbios_string( bios_key, "BIOSVersion", bios->version, buf, offset, len );
        set_smbios_string( bios_key, "BIOSReleaseDate", bios->date, buf, offset, len );
        set_dword_value( bios_key, "BiosMajorRelease", releases ? bios->system_bios_major_release : 0xff );
        set_dword_value( bios_key, "BiosMinorRelease", releases ? bios->system_bios_minor_release : 0xff );
        set_dword_value( bios_key, "ECFirmwareMajorVersion", releases ? bios->ec_firmware_major_release : 0xff );
        set_dword_value( bios_key, "ECFirmwareMinorVersion", releases ? bios->ec_firmware_minor_release : 0xff );
    }
    if ((hdr = find_smbios_entry( SMBIOS_TYPE_SYSTEM, 0, buf, len )))
    {
        const struct smbios_system *system = (const struct smbios_system *)hdr;
        BOOL sku = system->hdr.length >= 0x1b;

        offset = (const char *)system - buf + system->hdr.length;
        set_smbios_string( bios_key, "SystemManufacturer", system->vendor, buf, offset, len );
        set_smbios_string( bios_key, "SystemProductName", system->product, buf, offset, len );
        set_smbios_string( bios_key, "SystemVersion", system->version, buf, offset, len );
        set_smbios_string( bios_key, "SystemSKU", sku ? system->sku : 0, buf, offset, len );
        set_smbios_string( bios_key, "SystemFamily", sku ? system->family : 0, buf, offset, len );
    }
}


#ifdef __aarch64__

/***********************************************************************
 *           create_id_reg_values_arm64
 *
 * The ARM64 ID registers of a core as "CP xxxx" values, where the host's table holds them.
 */
static void create_id_reg_values_arm64( HANDLE core_key, UINT core, const char *buf, UINT len )
{
    const struct smbios_header *hdr;
    const struct smbios_processor_additional_info *additional_info;
    const struct smbios_wine_core_id_regs_arm64 *core_id_regs;
    char name[9];
    UINT i;

    if (!(hdr = find_smbios_entry( SMBIOS_TYPE_PROCESSOR_ADDITIONAL_INFO, core, buf, len ))) return;

    additional_info = (const struct smbios_processor_additional_info *)hdr;
    core_id_regs = (const struct smbios_wine_core_id_regs_arm64 *)additional_info->info_block.data;
    for (i = 0; i < core_id_regs->num_regs; i++)
    {
        snprintf( name, sizeof(name), "CP %04X", core_id_regs->regs[i].reg );
        set_value( core_key, name, REG_QWORD, &core_id_regs->regs[i].value, sizeof(UINT64) );
    }
}

#endif


/***********************************************************************
 *           create_processor_values
 */
static void create_processor_values( HANDLE system_key, const char *buf, UINT len )
{
    const struct smbios_header *hdr;
    const struct smbios_processor *proc;
    unsigned int pkg, core, offset, i, thread_count;
    HANDLE key, cpu_key, fpu_key = 0, env_key;
    SYSTEM_BASIC_INFORMATION sbi;
    SYSTEM_CPU_INFORMATION sci;
    PROCESSOR_POWER_INFORMATION *power_info = NULL;
    const char *arch, *version, *vendorid;
    char id[60], buffer[128];
    ULONG count = 1;

    if (NtQuerySystemInformation( SystemCpuInformation, &sci, sizeof(sci), NULL )) return;
    if (!NtQuerySystemInformation( SystemBasicInformation, &sbi, sizeof(sbi), NULL ) &&
        sbi.NumberOfProcessors > 0)
        count = sbi.NumberOfProcessors;
    if (!(power_info = calloc( count, sizeof(*power_info) ))) return;
    if (NtPowerInformation( ProcessorInformation, NULL, 0, power_info, count * sizeof(*power_info) ))
        memset( power_info, 0, count * sizeof(*power_info) );

    switch (sci.ProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_ARM:
    case PROCESSOR_ARCHITECTURE_ARM64:
        set_string_value( system_key, "Identifier", "ARM processor family" );
        break;
    default:
        set_string_value( system_key, "Identifier", "AT compatible" );
        fpu_key = create_key( system_key, "FloatingPointProcessor", TRUE );
        break;
    }
    set_string_value( system_key, "SystemBiosDate", "01/01/70" );

    cpu_key = create_key( system_key, "CentralProcessor", TRUE );

    for (pkg = core = 0; ; pkg++)
    {
        if (!(hdr = find_smbios_entry( SMBIOS_TYPE_PROCESSOR, pkg, buf, len ))) break;
        if (hdr->length < 0x28) break; /* version 2.5 is required */
        proc = (const struct smbios_processor *)hdr;
        offset = (const char *)proc - buf + proc->hdr.length;
        version = get_smbios_string( proc->version, buf, offset, len );
        vendorid = get_smbios_string( proc->vendor, buf, offset, len );

        switch (sci.ProcessorArchitecture)
        {
        case PROCESSOR_ARCHITECTURE_ARM:
            arch = "ARM";
            snprintf( id, sizeof(id), "ARM Family 7 Model %X Revision %X",
                      sci.ProcessorLevel, sci.ProcessorRevision );
            break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            arch = "ARM64";
            snprintf( id, sizeof(id), "ARMv8 (64-bit) Family 8 Model %X Revision %X",
                      sci.ProcessorLevel, sci.ProcessorRevision );
            break;
        case PROCESSOR_ARCHITECTURE_AMD64:
            arch = "AMD64";
            snprintf( id, sizeof(id), "%s Family %u Model %u Stepping %u",
                      vendorid && !strcmp( vendorid, "AuthenticAMD" ) ? "AMD64" : "Intel64",
                      sci.ProcessorLevel, HIBYTE(sci.ProcessorRevision), LOBYTE(sci.ProcessorRevision) );
            break;
        default:
            arch = "x86";
            snprintf( id, sizeof(id), "x86 Family %u Model %u Stepping %u",
                      sci.ProcessorLevel, HIBYTE(sci.ProcessorRevision), LOBYTE(sci.ProcessorRevision) );
            break;
        }

        thread_count = (proc->hdr.length >= 0x30) ? proc->thread_count2 : proc->thread_count;
        for (i = 0; i < thread_count; i++, core++)
        {
            snprintf( buffer, sizeof(buffer), "%u", core );
            if (cpu_key && (key = create_key( cpu_key, buffer, TRUE )))
            {
                /* wineboot reads the TSC's frequency on x86 only */
                DWORD mhz = core < count ? power_info[core].MaxMhz : 0;

                set_dword_value( key, "FeatureSet", sci.ProcessorFeatureBits );
                set_string_value( key, "Identifier", id );
                if (vendorid) set_string_value( key, "VendorIdentifier", vendorid );
                if (version) set_string_value( key, "ProcessorNameString", version );
                set_dword_value( key, "~MHz", mhz );
#ifdef __aarch64__
                create_id_reg_values_arm64( key, core, buf, len );
#endif
                NtClose( key );
            }
            if (fpu_key && (key = create_key( fpu_key, buffer, TRUE )))
            {
                set_string_value( key, "Identifier", id );
                NtClose( key );
            }
        }

        /* the environment's values are saved, but they follow the host as wineboot keeps them */
        if (!pkg && (env_key = create_key( 0, "System\\CurrentControlSet\\Control\\"
                                              "Session Manager\\Environment", FALSE )))
        {
            set_string_value( env_key, "PROCESSOR_ARCHITECTURE", arch );
            snprintf( buffer, sizeof(buffer), "%s, %s", id, vendorid ? vendorid : "Unknown" );
            set_string_value( env_key, "PROCESSOR_IDENTIFIER", buffer );
            snprintf( buffer, sizeof(buffer), "%u", sci.ProcessorLevel );
            set_string_value( env_key, "PROCESSOR_LEVEL", buffer );
            snprintf( buffer, sizeof(buffer), "%04x", sci.ProcessorRevision );
            set_string_value( env_key, "PROCESSOR_REVISION", buffer );
            snprintf( buffer, sizeof(buffer), "%u", (unsigned int)count );
            set_string_value( env_key, "NUMBER_OF_PROCESSORS", buffer );
            NtClose( env_key );
        }
    }

    if (fpu_key) NtClose( fpu_key );
    if (cpu_key) NtClose( cpu_key );
    free( power_info );
}


/***********************************************************************
 *           create_hardware_keys
 */
static void create_hardware_keys(void)
{
    HANDLE system_key, bios_key;
    UINT len = 0;
    char *buf;

    if (!(buf = get_smbios_table( &len ))) return;
    if ((system_key = create_key( 0, "Hardware\\Description\\System", TRUE )))
    {
        if ((bios_key = create_key( system_key, "BIOS", TRUE )))
        {
            create_bios_values( bios_key, buf, len );
            NtClose( bios_key );
        }
        create_processor_values( system_key, buf, len );
        NtClose( system_key );
    }
    free( buf );
}


/***********************************************************************
 *           paneport_boot
 *
 * Takes the place of run_wineboot for the first process of a session: the prefix comes from a
 * template that wineboot made, and the steps of a boot that are not saved in it run here, in
 * the process, without wineboot's process or the services it starts.
 */
void paneport_boot(void)
{
    static const char eventA[] = "\\KernelObjects\\__wineboot_event";
    WCHAR eventW[ARRAY_SIZE(eventA)];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE event;

    virtual_init_user_shared_data();
    set_shared_data_version();
    create_drive_links();
    create_volatile_environment();
    create_hardware_keys();

    /* a later process that runs run_wineboot finds the boot done */
    ascii_to_unicode( eventW, eventA, ARRAY_SIZE(eventA) );
    init_unicode_string( &name, eventW );
    InitializeObjectAttributes( &attr, &name, OBJ_OPENIF, 0, NULL );
    if (!NtCreateEvent( &event, EVENT_ALL_ACCESS, &attr, NotificationEvent, TRUE ))
        NtSetEvent( event, NULL );  /* the handle stays open for the session */
}


/***********************************************************************
 *           process_main
 *
 * The body of a process's first thread.
 */
static void *process_main( void *arg )
{
    char **argv = arg;
    int argc = 0;

    while (argv[argc]) argc++;
    paneport_start_process( argc, argv );
    return NULL;
}


/***********************************************************************
 *           duplicate_std_fd
 */
static int duplicate_std_fd( int fd, int flags )
{
    if (fd < 0) return open( "/dev/null", flags | O_CLOEXEC );
    return fcntl( fd, F_DUPFD_CLOEXEC, 3 );
}


/***********************************************************************
 *           make_program_path
 *
 * The program as a Windows path: on the launch's own drive when it names a folder.
 */
static char *make_program_path( const struct wine_paneport_launch *launch )
{
    char *ret, *p, link[] = "dosdevices/?:";
    int config_fd;

    if (!launch->program_folder) return strdup( launch->program );

    link[11] = PANEPORT_PROGRAM_DRIVE;
    if ((config_fd = open( config_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1) return NULL;
    unlinkat( config_fd, link, 0 );
    if (symlinkat( launch->program_folder, config_fd, link ) == -1)
    {
        close( config_fd );
        return NULL;
    }
    close( config_fd );
    if (!(ret = malloc( strlen( launch->program ) + 4 ))) return NULL;
    sprintf( ret, "%c:\\%s", PANEPORT_PROGRAM_DRIVE - 'a' + 'A', launch->program );
    for (p = ret; *p; p++) if (*p == '/') *p = '\\';
    return ret;
}


/***********************************************************************
 *           make_argv
 */
static char **make_argv( const char *program, const char *arguments )
{
    const char *arg;
    char **argv;
    int count = 2, i = 0;

    if (arguments) for (arg = arguments; *arg; arg += strlen( arg ) + 1) count++;
    if (!(argv = calloc( count + 1, sizeof(*argv) ))) return NULL;
    argv[i++] = strdup( wineloader );
    argv[i++] = strdup( program );
    if (arguments) for (arg = arguments; *arg; arg += strlen( arg ) + 1) argv[i++] = strdup( arg );
    argv[i] = NULL;
    for (i = 0; i < count; i++) if (!argv[i]) return NULL;
    return argv;
}


#ifdef __aarch64__

/***********************************************************************
 *           check_tsd_thread
 */
static void *check_tsd_thread( void *arg )
{
    return signal_check_tsd( NULL ) ? arg : NULL;
}


/***********************************************************************
 *           check_tsd
 *
 * Whether a new thread's TSD can hold the TEB pointer: PE code takes the TEB from TSD slot 6
 * through TPIDRRO_EL0, since the kernel zeroes x18 (NtCurrentTeb in winnt.h). Neither is a
 * documented interface of the system, so a session checks both on a thread of its own before
 * any PE code runs: the slot must be empty there, and slot 0 must hold the thread.
 */
static BOOL check_tsd(void)
{
    pthread_t thread;
    void *ret = NULL;

    if (pthread_create( &thread, NULL, check_tsd_thread, &thread )) return FALSE;
    if (pthread_join( thread, &ret )) return FALSE;
    return ret != NULL;
}

#endif  /* __aarch64__ */


/***********************************************************************
 *           __wine_paneport_session_start
 */
DECLSPEC_EXPORT int32_t __wine_paneport_session_start( const pp_wine_bridge_t *host_bridge,
                                                       const struct wine_paneport_session *session )
{
    struct wine_paneport_server_args args;
    wine_paneport_server_init_t server_init;
    wine_paneport_server_run_t server_run;
    char *library, *nls_dir;
    int config_fd, server_fd;
    int32_t ret;

    if (!host_bridge || host_bridge->struct_size < sizeof(*host_bridge) ||
        host_bridge->version < PP_WINE_BRIDGE_VERSION)
        return PP_WINE_E_INVALID_ARGUMENT;
    if (!session || session->struct_size < sizeof(*session) ||
        session->architecture != WINE_PANEPORT_ARCH_X86_64 ||
        !session->prefix_folder || session->prefix_folder[0] != '/')
        return PP_WINE_E_INVALID_ARGUMENT;

    pthread_mutex_lock( &paneport_mutex );
    if (session_started)
    {
        pthread_mutex_unlock( &paneport_mutex );
        return PP_WINE_E_BUSY;
    }
    session_started = TRUE;
    bridge = host_bridge;
    paneport_mode = TRUE;
    pthread_mutex_unlock( &paneport_mutex );

#ifdef __aarch64__
    if (!check_tsd())
    {
        paneport_log( PP_WINE_LOG_ERROR, "a new thread's TSD cannot hold the TEB pointer "
                      "(TPIDRRO_EL0, or TSD slot 0 or 6, is not as expected)" );
        return PP_WINE_E_SYSTEM;
    }
#endif

    if (!read_jit_pool()) return PP_WINE_E_SYSTEM;

    if (!apply_environment( session->environment ) || setenv( "WINEPREFIX", session->prefix_folder, 1 ))
    {
        paneport_log( PP_WINE_LOG_ERROR, "the session's environment could not be set" );
        return PP_WINE_E_OUT_OF_MEMORY;
    }
    paneport_init_paths();

    /* umask can only be read by setting it, which must not race with the server thread */
    paneport_umask = umask( 0777 );
    umask( paneport_umask );

    if ((config_fd = open( config_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1 ||
        !prepare_prefix( config_fd ))
    {
        paneport_log( PP_WINE_LOG_ERROR, "the prefix could not be prepared (%s)", strerror( errno ));
        return PP_WINE_E_IO;
    }
    if ((mkdirat( config_fd, ".paneport-server", 0700 ) == -1 && errno != EEXIST) ||
        (server_fd = openat( config_fd, ".paneport-server", O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1)
    {
        paneport_log( PP_WINE_LOG_ERROR, "the server's folder could not be made (%s)", strerror( errno ));
        return PP_WINE_E_IO;
    }

    library = paneport_server_library_path();
    server_library = library ? dlopen( library, RTLD_NOW | RTLD_LOCAL ) : NULL;
    free( library );
    if (!server_library)
    {
        paneport_log( PP_WINE_LOG_ERROR, "the wineserver library could not be loaded: %s", dlerror() );
        return PP_WINE_E_IO;
    }
    server_init = dlsym( server_library, WINE_PANEPORT_SERVER_INIT );
    server_run = dlsym( server_library, WINE_PANEPORT_SERVER_RUN );
    if (!server_init || !server_run ||
        !(server_lib_connect = dlsym( server_library, WINE_PANEPORT_SERVER_CONNECT )) ||
        !(server_lib_terminate = dlsym( server_library, WINE_PANEPORT_SERVER_TERMINATE )))
    {
        paneport_log( PP_WINE_LOG_ERROR, "the wineserver library lacks an entry point" );
        return PP_WINE_E_IO;
    }
    server_lib_stop = dlsym( server_library, WINE_PANEPORT_SERVER_STOP );
    if (!server_lib_stop) return PP_WINE_E_IO;

    nls_dir = paneport_nls_dir();
    memset( &args, 0, sizeof(args) );
    args.size             = sizeof(args);
    args.config_dir_fd    = config_fd;
    args.server_dir_fd    = server_fd;
    args.nls_dir          = nls_dir;
    args.debug_level      = 0;
    args.interrupt_thread = paneport_interrupt_thread;
    args.log              = server_log;
    args.process_ended    = server_process_ended;
    ret = server_init( &args ) ? PP_WINE_E_SYSTEM : PP_WINE_OK;
    free( nls_dir );
    if (ret)
    {
        paneport_log( PP_WINE_LOG_ERROR, "the wineserver could not be set up" );
        return ret;
    }
    return bridge->start_thread( "wine.server", server_run, NULL );
}


/***********************************************************************
 *           __wine_paneport_session_stop
 */
DECLSPEC_EXPORT int32_t __wine_paneport_session_stop(void)
{
    if (!server_lib_stop) return PP_WINE_E_INVALID_ARGUMENT;
    server_lib_stop();
    return PP_WINE_OK;
}


/***********************************************************************
 *           __wine_paneport_process_launch
 */
DECLSPEC_EXPORT int32_t __wine_paneport_process_launch( const struct wine_paneport_launch *launch )
{
    pthread_attr_t attr;
    pthread_t thread;
    char **argv;
    int sockets[2];

    if (!launch || launch->struct_size < sizeof(*launch) || !launch->program || !launch->program[0] ||
        (launch->program_folder && launch->program_folder[0] != '/'))
        return PP_WINE_E_INVALID_ARGUMENT;

    pthread_mutex_lock( &paneport_mutex );
    if (!server_lib_connect || process_launched)
    {
        pthread_mutex_unlock( &paneport_mutex );
        return server_lib_connect ? PP_WINE_E_UNSUPPORTED : PP_WINE_E_SYSTEM;
    }
    process_launched = TRUE;
    process_handle = launch->process;
    pthread_mutex_unlock( &paneport_mutex );

    if ((std_fds[0] = duplicate_std_fd( launch->stdin_fd, O_RDONLY )) == -1 ||
        (std_fds[1] = duplicate_std_fd( launch->stdout_fd, O_WRONLY )) == -1 ||
        (std_fds[2] = duplicate_std_fd( launch->stderr_fd, O_WRONLY )) == -1 ||
        !(program_path = make_program_path( launch )) ||
        !(argv = make_argv( program_path, launch->arguments )))
    {
        paneport_log( PP_WINE_LOG_ERROR, "the launch could not be prepared (%s)", strerror( errno ));
        paneport_close_std_fds();
        return PP_WINE_E_OUT_OF_MEMORY;
    }

    if (socketpair( AF_UNIX, SOCK_STREAM, 0, sockets ) == -1)
    {
        paneport_close_std_fds();
        return PP_WINE_E_SYSTEM;
    }
    fcntl( sockets[0], F_SETFD, FD_CLOEXEC );
    fcntl( sockets[1], F_SETFD, FD_CLOEXEC );
    if (server_lib_connect( sockets[1] ))
    {
        close( sockets[0] );
        close( sockets[1] );
        paneport_close_std_fds();
        paneport_log( PP_WINE_LOG_ERROR, "the wineserver has ended" );
        return PP_WINE_E_SYSTEM;
    }
    paneport_server_socket = sockets[0];
    /* the server reports the process's end from now on; a stop may already have reported it */
    InterlockedCompareExchange( &process_state, WINE_PANEPORT_PROCESS_STARTING, 0 );

    /* nothing joins the thread: it ends through pthread_exit when the process ends */
    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, PANEPORT_FIRST_STACK_SIZE );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    if (pthread_create( &thread, &attr, process_main, argv ))
    {
        pthread_attr_destroy( &attr );
        /* the host waits for its pipes to close, and the server for the client to hang up */
        paneport_server_socket = -1;
        close( sockets[0] );
        paneport_close_std_fds();
        paneport_log( PP_WINE_LOG_ERROR, "the process's thread could not be created" );
        return PP_WINE_E_SYSTEM;
    }
    pthread_attr_destroy( &attr );
    return PP_WINE_OK;
}


/***********************************************************************
 *           __wine_paneport_process_terminate
 */
DECLSPEC_EXPORT int32_t __wine_paneport_process_terminate( uint64_t process, uint32_t exit_code )
{
    if (!server_lib_terminate || !process_launched || process != process_handle)
        return PP_WINE_E_INVALID_ARGUMENT;
    return server_lib_terminate( process_id, exit_code ) ? PP_WINE_E_SYSTEM : PP_WINE_OK;
}


/***********************************************************************
 *           __wine_paneport_process_query
 *
 * Reads the state that the reports of the bridge set, without a lock, since the host may hold
 * one of its own: process_handle is set before the state, and the exit code before _EXITED.
 */
DECLSPEC_EXPORT int32_t __wine_paneport_process_query( uint64_t process,
                                                       struct wine_paneport_process_state *state )
{
    LONG current = InterlockedCompareExchange( &process_state, 0, 0 );

    if (!state || state->struct_size < sizeof(*state)) return PP_WINE_E_INVALID_ARGUMENT;
    if (!current || process != process_handle) return PP_WINE_E_INVALID_ARGUMENT;
    state->state = current;
    state->exit_code = current == WINE_PANEPORT_PROCESS_EXITED ? process_exit_code : 0;
    return PP_WINE_OK;
}
