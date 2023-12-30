/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF)
 * file into the (emulated) memory.
 */

#include "elf.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"
#include "string.h"


typedef struct elf_info_t {
    spike_file_t *f;
    process *p;
} elf_info;

elf_symbol symbols[ 64 ];
char sym_name_pool[ 64 ][ 32 ];
int sym_count;

//
// the implementation of allocater. allocates memory space for later segment
// loading
//
static void *elf_alloc_mb( elf_ctx *ctx, uint64 elf_pa, uint64 elf_va,
                           uint64 size ) {
    // directly returns the virtual address as we are in the Bare mode in lab1_x
    return (void *) elf_va;
}

//
// actual file reading, using the spike file interface.
//
static uint64 elf_fpread( elf_ctx *ctx, void *dest, uint64 nb, uint64 offset ) {
    elf_info *msg = (elf_info *) ctx->info;
    // call spike file utility to load the content of elf file into memory.
    // spike_file_pread will read the elf file (msg->f) from offset to memory
    // (indicated by *dest) for nb bytes.
    return spike_file_pread( msg->f, dest, nb, offset );
}

//
// init elf_ctx, a data structure that loads the elf.
//
elf_status elf_init( elf_ctx *ctx, void *info ) {
    ctx->info = info;

    // load the elf header
    if ( elf_fpread( ctx, &ctx->ehdr, sizeof( ctx->ehdr ), 0 ) !=
         sizeof( ctx->ehdr ) )
        return EL_EIO;

    // check the signature (magic value) of the elf
    if ( ctx->ehdr.magic != ELF_MAGIC ) return EL_NOTELF;

    return EL_OK;
}

//
// load the elf segments to memory regions as we are in Bare mode in lab1
//
elf_status elf_load( elf_ctx *ctx ) {
    // elf_prog_header structure is defined in kernel/elf.h
    elf_prog_header ph_addr;
    int i, off;

    // traverse the elf program segment headers
    for ( i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum;
          i++, off += sizeof( ph_addr ) ) {
        // read segment headers
        if ( elf_fpread( ctx, (void *) &ph_addr, sizeof( ph_addr ), off ) !=
             sizeof( ph_addr ) )
            return EL_EIO;

        if ( ph_addr.type != ELF_PROG_LOAD ) continue;
        if ( ph_addr.memsz < ph_addr.filesz ) return EL_ERR;
        if ( ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr ) return EL_ERR;

        // allocate memory block before elf loading
        void *dest =
            elf_alloc_mb( ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz );

        // actual loading
        if ( elf_fpread( ctx, dest, ph_addr.memsz, ph_addr.off ) !=
             ph_addr.memsz )
            return EL_EIO;
    }

    return EL_OK;
}

typedef union {
    uint64 buf[ MAX_CMDLINE_ARGS ];
    char *argv[ MAX_CMDLINE_ARGS ];
} arg_buf;

//
// returns the number (should be 1) of string(s) after PKE kernel in command
// line. and store the string(s) in arg_bug_msg.
//
static size_t parse_args( arg_buf *arg_bug_msg ) {
    // HTIFSYS_getmainvars frontend call reads command arguments to (input)
    // *arg_bug_msg
    long r = frontend_syscall( HTIFSYS_getmainvars, (uint64) arg_bug_msg,
                               sizeof( *arg_bug_msg ), 0, 0, 0, 0, 0 );
    kassert( r == 0 );

    size_t pk_argc = arg_bug_msg->buf[ 0 ];
    uint64 *pk_argv = &arg_bug_msg->buf[ 1 ];

    int arg = 1; // skip the PKE OS kernel string, leave behind only the
                 // application name
    for ( size_t i = 0; arg + i < pk_argc; i++ )
        arg_bug_msg->argv[ i ] = (char *) (uintptr_t) pk_argv[ arg + i ];

    // returns the number of strings after PKE kernel in command line
    return pk_argc - arg;
}

void load_func_name( elf_ctx *ctx ) {
    int len_section_header = sizeof( elf_section_header );
    uint64 shstr_offset =
        ctx->ehdr.shoff + ctx->ehdr.shstrndx * len_section_header;
    elf_section_header shstr;
    elf_fpread( ctx, (void *) &shstr, sizeof( shstr ), shstr_offset );

    char temp_str[ shstr.size ];
    uint64 shstr_sect_off = shstr.offset;
    elf_fpread( ctx, &temp_str, shstr.size, shstr_sect_off );

    elf_section_header sym;
    elf_section_header str;
    elf_section_header temp;
    int sect_num = ctx->ehdr.shnum;
    for ( int i = 0; i < sect_num; i++ ) {
        elf_fpread( ctx, (void *) &temp, sizeof( temp ),
                    ctx->ehdr.shoff + i * ctx->ehdr.shentsize );
        uint32 type = temp.type;
        if ( type == SHT_SYMTAB ) {
            sym = temp;
        } else if ( type == SHT_STRTAB ) {
            if ( strcmp( temp_str + temp.name, ".strtab" ) == 0 ) {
                str = temp;
            }
        } else {
        }
    }

    int cot = 0;
    uint64 str_sec_off = str.offset;
    uint64 sym_num = sym.size / sizeof( elf_symbol );
    for ( int i = 0; i < sym_num; i++ ) {
        elf_symbol symbol;
        elf_fpread( ctx, (void *) &symbol, sizeof( symbol ),
                    sym.offset + i * sizeof( elf_symbol ) );
        if ( symbol.st_name == 0 ) continue;
        if ( symbol.st_info == 18 ) { // STT_FUNC
            char sym_name[ 32 ];
            elf_fpread( ctx, (void *) &sym_name, sizeof( sym_name ),
                        str_sec_off + symbol.st_name );
            symbols[ cot++ ] = symbol;
            strcpy( sym_name_pool[ cot - 1 ], sym_name );
        }
    }
    sym_count = cot;
    return ;
}

//
// load the elf of user application, by using the spike file interface.
//
void load_bincode_from_host_elf( process *p ) {
    arg_buf arg_bug_msg;

    // retrieve command line arguements
    size_t argc = parse_args( &arg_bug_msg );
    if ( !argc ) panic( "You need to specify the application program!\n" );

    sprint( "Application: %s\n", arg_bug_msg.argv[ 0 ] );

    // elf loading. elf_ctx is defined in kernel/elf.h, used to track the
    // loading process.
    elf_ctx elfloader;
    // elf_info is defined above, used to tie the elf file and its corresponding
    // process.
    elf_info info;

    info.f = spike_file_open( arg_bug_msg.argv[ 0 ], O_RDONLY, 0 );
    info.p = p;
    // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
    if ( IS_ERR_VALUE( info.f ) )
        panic( "Fail on openning the input application program.\n" );

    // init elfloader context. elf_init() is defined above.
    if ( elf_init( &elfloader, &info ) != EL_OK )
        panic( "fail to init elfloader.\n" );

    // load elf. elf_load() is defined above.
    if ( elf_load( &elfloader ) != EL_OK ) panic( "Fail on loading elf.\n" );

    load_func_name( &elfloader );

    // entry (virtual, also physical in lab1_x) address
    p->trapframe->epc = elfloader.ehdr.entry;

    // close the host spike file
    spike_file_close( info.f );

    sprint( "Application program entry point (virtual address): 0x%lx\n",
            p->trapframe->epc );
}

// added @lab1_challenge1
int elf_get_funname( uint64 ret_addr ) {
    int len_count = sym_count;
    for ( int i = 0; i < sym_count; i++ ) {

        if ( ret_addr >= symbols[ i ].st_value &&
             ret_addr < symbols[ i ].st_value + symbols[ i ].st_size ) {
            sprint( "%s\n", sym_name_pool[ i ] );
            if ( strcmp( sym_name_pool[ i ], "main" ) == 0 ) return 0;
            return 1;
        }
    }
    return 1;
}

long backtrace( int depth ) {
    int i, off;
    i = 1;
    // sprint( "backtrace:\n" );
    return i;
}