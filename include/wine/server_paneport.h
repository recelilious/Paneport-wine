/*
 * The wineserver as a library, for Wine's single-process mode for Paneport
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
 * In single-process mode the wineserver runs as a thread of the host's process. The build
 * makes it a shared library, wineserver.so, beside the wineserver program; ntdll's Unix side
 * loads it from Wine's bin folder and finds the entry points below with dlsym. This header is
 * internal to Wine: the host never includes it.
 *
 * In this mode the server never forks, execs, changes the process's current directory or
 * signal dispositions, never sends a signal, and never calls exit or _exit: when its last
 * process ends, or when it is told to stop, it saves the registry and returns from its thread's
 * body.
 */

#ifndef __WINE_WINE_SERVER_PANEPORT_H
#define __WINE_WINE_SERVER_PANEPORT_H

/* What ntdll passes to the server's initialization. */
struct wine_paneport_server_args
{
    unsigned int size;          /* sizeof(struct wine_paneport_server_args) */
    int          config_dir_fd; /* the prefix's folder, opened with O_DIRECTORY; the server keeps it */
    int          server_dir_fd; /* a folder of the server's own for temporary files, opened with
                                   O_DIRECTORY; the server keeps it */
    const char  *nls_dir;       /* the folder that holds the NLS files */
    int          debug_level;   /* the server's debug level, as its -d option sets it */

    /* Interrupts a thread of a client process as the server's SIGUSR1 (suspension, context,
     * system APCs) or SIGQUIT (termination) would, without sending a signal. tid is the thread's
     * Unix thread id as the client sent it: its Mach port name on macOS. Runs on the server's
     * thread. Returns 1 when the thread will handle it, 0 when the thread is gone. */
    int  (*interrupt_thread)( unsigned int tid, int sig );

    /* Hands a line of the server's diagnostics, such as a fatal error, to the host's log;
     * level is 0 debug, 1 info, 2 warning or 3 error. Any thread. */
    void (*log)( int level, const char *text );

    /* Reports that a client process ended, with the exit code that GetExitCodeProcess gives,
     * once its last thread is gone. process_id is its Windows process id. Runs on the server's
     * thread, once for each process. */
    void (*process_ended)( unsigned int process_id, unsigned int exit_code );
};

/* The names under which wineserver.so exports its entry points. */
#define WINE_PANEPORT_SERVER_INIT    "__wine_paneport_server_init"
#define WINE_PANEPORT_SERVER_RUN     "__wine_paneport_server_run"
#define WINE_PANEPORT_SERVER_CONNECT "__wine_paneport_server_connect"
#define WINE_PANEPORT_SERVER_STOP    "__wine_paneport_server_stop"
#define WINE_PANEPORT_SERVER_TERMINATE "__wine_paneport_server_terminate"

/* Sets the server up on the calling thread, before the server's thread exists: what the
 * wineserver program's main() does, without its master socket, signal handlers and limits.
 * It copies what it keeps of args. Returns 0, or -1 when the setup failed. */
typedef int (*wine_paneport_server_init_t)( const struct wine_paneport_server_args *args );

/* The body of the server's thread: runs the main loop until the server ends, then saves the
 * registry and returns. */
typedef void (*wine_paneport_server_run_t)( void *unused );

/* Gives the server one end of a connected socket pair, for a new client process whose ntdll
 * takes the other end as its server socket. Any thread. The server takes the descriptor.
 * Returns 0, or -1 when the server has ended. */
typedef int (*wine_paneport_server_connect_t)( int fd );

/* Tells the server to terminate its processes, save the registry and end. Any thread; it
 * returns at once. */
typedef void (*wine_paneport_server_stop_t)( void );

/* Terminates a client process, or every client process for a process_id of 0, with exit_code,
 * as TerminateProcess does. Any thread, such as one of the host's; it returns at once, and
 * process_ended reports the end. Returns 0, or -1 when the server has ended. */
typedef int (*wine_paneport_server_terminate_t)( unsigned int process_id, unsigned int exit_code );

#endif /* __WINE_WINE_SERVER_PANEPORT_H */
