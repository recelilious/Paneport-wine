/*
 * Thread-safe static constructor support functions.
 *
 * Copyright 2026 Jacek Caban for CodeWeavers
 * Changed for Paneport on 2026-11-27: refers to __emutls_get_address by its ARM64EC name, so that the linker finds it for code built with emulated TLS.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public License,
 * the authors give you unlimited permission to link the compiled version
 * of this file with other programs, and to distribute those programs
 * without any restriction coming from the use of this file.  (The GNU
 * Lesser General Public License restrictions do apply in other respects;
 * for example, they cover modification of the file, and distribution when
 * not linked into another program.)
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep implib
#endif

#ifdef __WINE_PE_BUILD

#include <stdarg.h>
#include <limits.h>
#include "windef.h"
#include "winbase.h"

static SRWLOCK init_lock;
static CONDITION_VARIABLE init_cv;

int _Init_global_epoch = INT_MIN;
__thread int _Init_thread_epoch = INT_MIN;

#ifdef __arm64ec__
/* Paneport builds the thread-local variables of ARM64 PE code with emulated TLS, since the
 * kernel of Apple systems zeroes x18, through which native TLS finds its array. The compiler
 * then calls __emutls_get_address for each access to _Init_thread_epoch, here and in the
 * guard of every static initializer. In ARM64EC code it names that function without the
 * ARM64EC mangling, and the linker finds no archive member for such a name: compiler-rt
 * defines #__emutls_get_address and only a weak alias for the plain name. This call refers
 * to the mangled name, which brings the member in, and the alias then serves the others.
 * Every module that has such a guard links this file for the variable above. */
extern void *__emutls_get_address( void *control );
void *__wine_emutls_get_address( void *control );
void *__wine_emutls_get_address( void *control )
{
    return __emutls_get_address( control );
}
#endif

void _Init_thread_lock(void)
{
    AcquireSRWLockExclusive( &init_lock );
}

void _Init_thread_unlock(void)
{
    ReleaseSRWLockExclusive( &init_lock );
}

void _Init_thread_wait( DWORD timeout )
{
    SleepConditionVariableSRW( &init_cv, &init_lock, timeout, 0 );
}

void _Init_thread_notify(void)
{
    WakeAllConditionVariable( &init_cv );
}

void _Init_thread_header( int *once )
{
    _Init_thread_lock();
    while (*once == -1) _Init_thread_wait( INFINITE );
    if (*once) _Init_thread_epoch = _Init_global_epoch;
    else *once = -1;
    _Init_thread_unlock();
}

void _Init_thread_footer( int *once )
{
    _Init_thread_lock();
    _Init_thread_epoch = *once = ++_Init_global_epoch;
    _Init_thread_unlock();
    _Init_thread_notify();
}

void _Init_thread_abort( int *once )
{
    _Init_thread_lock();
    *once = 0;
    _Init_thread_unlock();
    _Init_thread_notify();
}

#endif
