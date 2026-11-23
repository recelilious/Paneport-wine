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
 * wineserver.so is built from the same objects as the wineserver program and exports the entry
 * points that include/wine/server_paneport.h declares. Only __wine_paneport_server_init sets
 * paneport_mode; without it the server behaves as the wineserver program does.
 *
 * Other threads reach the server's thread through a control pipe, whose read end is an fd in the
 * main loop: each call writes one small fixed-size message with a single write().
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "process.h"
#include "thread.h"
#include "request.h"
#include "unicode.h"
#include "paneport.h"
#include "wine/server_paneport.h"

int paneport_mode = 0;

enum control_op
{
    CONTROL_CONNECT,    /* a new client process on a connected socket */
    CONTROL_TERMINATE,  /* terminate a client process, or all of them */
    CONTROL_STOP        /* terminate every process and end the server */
};

/* a message on the control pipe, far smaller than PIPE_BUF so that a write() is atomic */
struct control_msg
{
    enum control_op  op;
    int              fd;    /* CONTROL_CONNECT: the server's end of the socket pair */
    unsigned int     id;    /* CONTROL_TERMINATE: the process id, or 0 for every process */
    unsigned int     code;  /* CONTROL_TERMINATE: the exit code */
};

/* what other threads may still post; it only goes up */
enum control_state
{
    CONTROL_OPEN,      /* every message */
    CONTROL_CLOSING,   /* no new clients, the server is ending */
    CONTROL_STOPPING,  /* no new clients, and the stop message is posted */
    CONTROL_ENDED      /* nothing, the server's thread has left its main loop */
};

enum server_stage { STAGE_NONE, STAGE_FAILED, STAGE_READY, STAGE_RUNNING, STAGE_DONE };

struct control
{
    struct object  obj;  /* object header */
    struct fd     *fd;   /* file descriptor of the pipe's read end */
};

static void control_dump( struct object *obj, int verbose );
static void control_destroy( struct object *obj );
static void control_poll_event( struct fd *fd, int event );

static const struct object_ops control_ops =
{
    .size    = sizeof(struct control),
    .type    = &no_type,
    .dump    = control_dump,
    .destroy = control_destroy,
};

static const struct fd_ops control_fd_ops =
{
    .poll_event = control_poll_event,
};

static struct wine_paneport_server_args server_args;  /* what init keeps of its arguments */
static enum server_stage server_stage;
static struct control *server_control;  /* the server's side of the control pipe */
static int listening = 1;               /* new clients are accepted, as on the master socket */
static int registry_saved;              /* the registry was saved when the server ended */

/* shared with the threads that post messages */
static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;
static enum control_state control_state = CONTROL_ENDED;
static int control_write_fd = -1;


static void control_dump( struct object *obj, int verbose )
{
    struct control *control = (struct control *)obj;
    assert( obj->ops == &control_ops );
    fprintf( stderr, "Control pipe fd=%p\n", control->fd );
}

static void control_destroy( struct object *obj )
{
    struct control *control = (struct control *)obj;
    assert( obj->ops == &control_ops );
    if (control->fd) release_object( control->fd );
}

/* accept a new client on a connected socket, as master_socket_poll_event does */
static void accept_client( int client )
{
    struct process *process;

    if (!listening)  /* the counterpart of the master socket is closed */
    {
        close( client );
        return;
    }
    fcntl( client, F_SETFL, O_NONBLOCK );
    if ((process = create_process( client, NULL, 0, NULL, NULL, NULL, 0, NULL )))
    {
        struct thread *thread = create_thread( -1, process, NULL );
        if (thread) add_process_thread( process, thread );
        release_object( process );
    }
}

/* handle the messages that other threads posted */
static void control_poll_event( struct fd *fd, int event )
{
    struct control_msg msg;

    if (event & (POLLERR | POLLHUP))
    {
        /* this is not supposed to happen */
        fprintf( stderr, "wineserver: Error on control pipe\n" );
        set_fd_events( fd, -1 );
        return;
    }
    if (!(event & POLLIN)) return;

    while (read( get_unix_fd( fd ), &msg, sizeof(msg) ) == sizeof(msg))
    {
        switch (msg.op)
        {
        case CONTROL_CONNECT:
            accept_client( msg.fd );
            break;
        case CONTROL_TERMINATE:
            terminate_process_from_id( msg.id, msg.code );
            break;
        case CONTROL_STOP:
            shutdown_master_socket();  /* what SIGINT does to the wineserver program */
            break;
        }
    }
}

/* write a message on the control pipe, with control_mutex held */
static int write_control_msg( const struct control_msg *msg )
{
    ssize_t ret;

    do ret = write( control_write_fd, msg, sizeof(*msg) );
    while (ret == -1 && errno == EINTR);
    return ret == (ssize_t)sizeof(*msg);
}

/* the folder of the NLS files, for get_nls_dir */
char *paneport_get_nls_dir(void)
{
    return strdup( server_args.nls_dir );
}

/* interrupt a thread of a client process instead of sending it a signal */
int paneport_interrupt_thread( unsigned int tid, int sig )
{
    return server_args.interrupt_thread( tid, sig );
}

/* report that a process ended, once its last thread is gone */
void paneport_process_ended( struct process *process )
{
    if (server_args.process_ended) server_args.process_ended( process->id, process->exit_code );
}

/* the counterpart of closing the master socket: refuse new clients from now on */
void paneport_close_listener(void)
{
    listening = 0;
    pthread_mutex_lock( &control_mutex );
    if (control_state == CONTROL_OPEN) control_state = CONTROL_CLOSING;
    pthread_mutex_unlock( &control_mutex );
}

/* end the server once the registry is saved: the main loop ends and the server's thread returns */
void paneport_end_server(void)
{
    registry_saved = 1;
    quit_main_loop();
}

/* hand a fatal error to the host's log and abort, instead of exiting */
void paneport_fatal_error( const char *err, va_list args )
{
    char text[1024];
    size_t len;

    strcpy( text, "wineserver: " );
    len = strlen( text );
    vsnprintf( text + len, sizeof(text) - len, err, args );
    len = strlen( text );
    if (len && text[len - 1] == '\n') text[len - 1] = 0;
    if (server_args.log) server_args.log( 3, text );
    else fprintf( stderr, "%s\n", text );
    abort();
}

/* set the server up on the calling thread: main() without the master socket, signals and limits */
DECLSPEC_EXPORT int __wine_paneport_server_init( const struct wine_paneport_server_args *args )
{
    int fds[2];

    if (server_stage != STAGE_NONE) return -1;
    server_stage = STAGE_FAILED;  /* the server cannot be set up twice */

    if (!args || args->size < sizeof(*args)) return -1;
    if (args->config_dir_fd < 0 || args->server_dir_fd < 0 || !args->nls_dir || !args->interrupt_thread)
        return -1;
    server_args = *args;
    server_args.size = sizeof(server_args);
    if (!(server_args.nls_dir = strdup( args->nls_dir ))) return -1;

    paneport_mode = 1;
    debug_level = args->debug_level;
    master_socket_timeout = 0;  /* end as soon as the last process is gone */
    config_dir_fd = args->config_dir_fd;
    server_dir_fd = args->server_dir_fd;

    sock_init();

    /* the control pipe takes the place of the master socket */
    if (pipe( fds ) == -1) return -1;
    fcntl( fds[0], F_SETFD, FD_CLOEXEC );
    fcntl( fds[1], F_SETFD, FD_CLOEXEC );
    fcntl( fds[0], F_SETFL, O_NONBLOCK );
    fcntl( fds[1], F_SETFL, O_NONBLOCK );  /* a full pipe fails the write instead of blocking */
    if (!(server_control = alloc_object( &control_ops )))
    {
        close( fds[0] );
        close( fds[1] );
        return -1;
    }
    if (!(server_control->fd = create_anonymous_fd( &control_fd_ops, fds[0], &server_control->obj, 0 )))
    {
        close( fds[1] );
        release_object( server_control );
        server_control = NULL;
        return -1;
    }
    set_fd_events( server_control->fd, POLLIN );
    control_write_fd = fds[1];

    if (debug_level) fprintf( stderr, "wineserver: starting (pid=%ld)\n", (long) getpid() );
    set_current_time();
    init_memory();
    init_directories( load_intl_file() );
    init_threading();
    init_registry();

    pthread_mutex_lock( &control_mutex );
    control_state = CONTROL_OPEN;
    pthread_mutex_unlock( &control_mutex );
    server_stage = STAGE_READY;
    return 0;
}

/* the body of the server's thread */
DECLSPEC_EXPORT void __wine_paneport_server_run( void *unused )
{
    struct control_msg msg;
    sigset_t sigset;

    if (server_stage != STAGE_READY) return;
    server_stage = STAGE_RUNNING;

    /* the asynchronous signals are the host's, never this thread's */
    sigfillset( &sigset );
    sigdelset( &sigset, SIGSEGV );
    sigdelset( &sigset, SIGBUS );
    sigdelset( &sigset, SIGILL );
    sigdelset( &sigset, SIGFPE );
    sigdelset( &sigset, SIGTRAP );
    sigdelset( &sigset, SIGABRT );
    pthread_sigmask( SIG_BLOCK, &sigset, NULL );

    main_loop();

    pthread_mutex_lock( &control_mutex );
    control_state = CONTROL_ENDED;
    close( control_write_fd );
    control_write_fd = -1;
    pthread_mutex_unlock( &control_mutex );

    /* nobody else writes now: refuse the clients that are still on their way */
    while (read( get_unix_fd( server_control->fd ), &msg, sizeof(msg) ) == sizeof(msg))
        if (msg.op == CONTROL_CONNECT) close( msg.fd );
    release_object( server_control );
    server_control = NULL;

    if (!registry_saved) flush_registry();
    server_stage = STAGE_DONE;
}

/* hand the server its end of a new client's socket pair; any thread */
DECLSPEC_EXPORT int __wine_paneport_server_connect( int fd )
{
    struct control_msg msg = { .op = CONTROL_CONNECT, .fd = fd };
    int ret;

    pthread_mutex_lock( &control_mutex );
    ret = (control_state == CONTROL_OPEN && write_control_msg( &msg )) ? 0 : -1;
    pthread_mutex_unlock( &control_mutex );
    return ret;
}

/* terminate a client process, or every one for a process_id of 0; any thread */
DECLSPEC_EXPORT int __wine_paneport_server_terminate( unsigned int process_id, unsigned int exit_code )
{
    struct control_msg msg = { .op = CONTROL_TERMINATE, .id = process_id, .code = exit_code };
    int ret;

    pthread_mutex_lock( &control_mutex );
    ret = (control_state < CONTROL_ENDED && write_control_msg( &msg )) ? 0 : -1;
    pthread_mutex_unlock( &control_mutex );
    return ret;
}

/* terminate every process, save the registry and end the server; any thread */
DECLSPEC_EXPORT void __wine_paneport_server_stop(void)
{
    struct control_msg msg = { .op = CONTROL_STOP };

    pthread_mutex_lock( &control_mutex );
    if (control_state < CONTROL_STOPPING && write_control_msg( &msg )) control_state = CONTROL_STOPPING;
    pthread_mutex_unlock( &control_mutex );
}
