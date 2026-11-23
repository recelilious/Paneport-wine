/*
 * Wine server single-process mode for Paneport
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

#ifndef __WINE_SERVER_PANEPORT_H
#define __WINE_SERVER_PANEPORT_H

#include <stdarg.h>

struct process;

/* paneport.c: in single-process mode the server runs as a thread of the host's process,
 * built as wineserver.so, and only __wine_paneport_server_init sets paneport_mode */

extern int paneport_mode;

extern char *paneport_get_nls_dir(void);
extern int paneport_interrupt_thread( unsigned int tid, int sig );
extern void paneport_process_ended( struct process *process );
extern void paneport_close_listener(void);
extern void paneport_end_server(void);
#ifdef __GNUC__
extern void paneport_fatal_error( const char *err, va_list args )
    __attribute__((noreturn,format(printf,1,0)));
#else
extern void paneport_fatal_error( const char *err, va_list args );
#endif

/* fd.c */

extern void quit_main_loop(void);

/* process.c */

extern void terminate_process_from_id( unsigned int id, int exit_code );

#endif  /* __WINE_SERVER_PANEPORT_H */
