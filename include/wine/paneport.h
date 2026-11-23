/*
 * The entry points of Wine's single-process mode for Paneport
 *
 * Copyright 2026 recelilious and the Paneport contributors
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
 * In single-process mode, the wineserver runs as a thread of the host's process and each
 * Windows process as a group of threads of it; nothing forks or execs. The native host of
 * Paneport loads ntdll's Unix library, finds the entry points below with dlsym, and calls them,
 * passing the table of paneport/wine_bridge.h, through which Wine reaches the host. This header
 * holds declarations, constants and struct layouts only, and it is the only header of Wine that
 * the host includes.
 *
 * Every entry point returns PP_WINE_OK or a negative PP_WINE_E_ status of
 * paneport/wine_bridge.h, and copies what it keeps of its arguments.
 */

#ifndef __WINE_WINE_PANEPORT_H
#define __WINE_WINE_PANEPORT_H

#include <stdint.h>

#include <paneport/wine_bridge.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of these declarations. */
#define WINE_PANEPORT_ENTRY_VERSION 1

/* The architecture of a session's programs. */
#define WINE_PANEPORT_ARCH_X86_64 1

/* A session. */
struct wine_paneport_session
{
    uint32_t    struct_size;    /* sizeof(struct wine_paneport_session) */
    uint32_t    architecture;   /* WINE_PANEPORT_ARCH_X86_64 */
    const char *prefix_folder;  /* absolute Unix path of the prefix's folder */
    const char *environment;    /* "NAME=value" strings, each NUL-terminated, ending with an
                                   empty string; or NULL */
};

/* A launch of a Windows program as a process of the session. */
struct wine_paneport_launch
{
    uint32_t    struct_size;    /* sizeof(struct wine_paneport_launch) */
    int32_t     stdin_fd;       /* the program's standard input, or -1 for /dev/null */
    int32_t     stdout_fd;      /* its standard output, or -1 for /dev/null */
    int32_t     stderr_fd;      /* its standard error, or -1 for /dev/null */
    uint64_t    process;        /* the host's handle of the process, for the bridge's reports */
    const char *program_folder; /* absolute Unix path of the folder that a drive letter of its
                                   own maps for this process; or NULL */
    const char *program;        /* relative to program_folder, or a Windows path */
    const char *arguments;      /* strings, each NUL-terminated, ending with an empty string;
                                   or NULL */
};

/* The names under which ntdll's Unix library exports the entry points. */
#define WINE_PANEPORT_SESSION_START     "__wine_paneport_session_start"
#define WINE_PANEPORT_SESSION_STOP      "__wine_paneport_session_stop"
#define WINE_PANEPORT_PROCESS_LAUNCH    "__wine_paneport_process_launch"
#define WINE_PANEPORT_PROCESS_TERMINATE "__wine_paneport_process_terminate"

/*
 * Starts the session on the host's main thread: keeps the bridge, prepares the prefix, has the
 * host create the wineserver's thread through the bridge, and returns once the server accepts
 * its first client. It runs once per process.
 */
typedef int32_t (*wine_paneport_session_start_t)( const pp_wine_bridge_t *bridge,
                                                  const struct wine_paneport_session *session );

/*
 * Stops the session on the host's main thread: terminates its processes, tells the wineserver
 * to save the registry and end its loop, and returns once that is under way. The host then
 * joins the threads that it created for the session.
 */
typedef int32_t (*wine_paneport_session_stop_t)( void );

/*
 * Launches a program as the session's process on a thread of its own, on any thread of the
 * host. It duplicates the descriptors that it keeps, and returns once the process's thread
 * runs; the bridge's process_started or process_exited follows.
 */
typedef int32_t (*wine_paneport_process_launch_t)( const struct wine_paneport_launch *launch );

/*
 * Terminates a process of the session with an exit code, on any thread of the host, as
 * TerminateProcess does, and returns once the termination is under way.
 */
typedef int32_t (*wine_paneport_process_terminate_t)( uint64_t process, uint32_t exit_code );

#ifdef __cplusplus
}
#endif

#endif /* __WINE_WINE_PANEPORT_H */
