/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>


#include "elf.h"
#include "process.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"


#include "spike_interface/spike_utils.h"

extern elf_symbol symbols[ 64 ];
extern char sym_name_pool[ 64 ][ 32 ];
extern int sym_count;

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print( const char *buf, size_t n ) {
    sprint( buf );
    return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit( uint64 code ) {
    sprint( "User exit with code:%d.\n", code );
    // in lab1, PKE considers only one app (one process).
    // therefore, shutdown the system when the app calls exit()
    shutdown( code );
}

// added @lab1_challenge1
ssize_t sys_backtrace( uint64 depth ) {

    int i, off;

    uint64 fun_sp = current->trapframe->regs.sp + 32;
    uint64 fun_pa = fun_sp + 8;
    i = 0;
    while ( i < depth ) {
        if ( elf_get_funname( *(uint64 *) fun_pa ) == 0 ) return i;
        fun_pa += 16;
        i++;
    }
    return i;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall( long a0, long a1, long a2, long a3, long a4, long a5, long a6,
                 long a7 ) {
    switch ( a0 ) {
    case SYS_user_print:
        return sys_user_print( (const char *) a1, a2 );
    case SYS_user_exit:
        return sys_user_exit( a1 );
    case SYS_print_backtrace:
        return sys_backtrace( a1 );
    default:
        panic( "Unknown syscall %ld \n", a0 );
    }
}
