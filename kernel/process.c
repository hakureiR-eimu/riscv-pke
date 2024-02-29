/*
 * Utility functions for process management.
 *
 * Note: in Lab1, only one process (i.e., our user application) exists.
 * Therefore, PKE OS at this stage will set "current" to the loaded user
 * application, and also switch to the old "current" process after trap
 * handling.
 */

#include "process.h"
#include "config.h"
#include "elf.h"
#include "memlayout.h"
#include "pmm.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"
#include "strap.h"
#include "string.h"
#include "util/functions.h"
#include "vmm.h"

// Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user( trapframe *, uint64 satp );

// current points to the currently running user-mode application.
process *current = NULL;

// points to the first free page in our simple heap. added @lab2_2
uint64 g_ufree_page = USER_FREE_ADDRESS_START;

//
// switch to a user-mode process
//
void switch_to( process *proc ) {
    assert( proc );
    current = proc;

    // write the smode_trap_vector (64-bit func. address) defined in
    // kernel/strap_vector.S to the stvec privilege register, such that trap
    // handler pointed by smode_trap_vector will be triggered when an interrupt
    // occurs in S mode.
    write_csr( stvec, (uint64) smode_trap_vector );

    // set up trapframe values (in process structure) that smode_trap_vector
    // will need when the process next re-enters the kernel.
    proc->trapframe->kernel_sp = proc->kstack;       // process's kernel stack
    proc->trapframe->kernel_satp = read_csr( satp ); // kernel page table
    proc->trapframe->kernel_trap = (uint64) smode_trap_handler;

    // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
    // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register)
    // to User mode.
    unsigned long x = read_csr( sstatus );
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode

    // write x back to 'sstatus' register to enable interrupts, and sret
    // destination mode.
    write_csr( sstatus, x );

    // set S Exception Program Counter (sepc register) to the elf entry pc.
    write_csr( sepc, proc->trapframe->epc );

    // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added
    // @lab2_1
    uint64 user_satp = MAKE_SATP( proc->pagetable );

    // return_to_user() is defined in kernel/strap_vector.S. switch to user mode
    // with sret. note, return_to_user takes two parameters @ and after lab2_1.
    return_to_user( proc->trapframe, user_satp );
}

// added @lab2_challenge2
uint64 user_better_allocate( long n ) {

    // 以8为倍数向上取整
    n = ROUNDUP( n, 8 );
    if ( n > PGSIZE ) {
        panic( "fail to allocate PGSIZE due to too big size.\n" );
    }

    // 遍历块链表取取到一个合适的块
    BLOCK *cur = current->free_start, *pre = current->free_start;
    while ( cur ) {
        if ( cur->size >= n ) break;
        pre = cur;
        cur = cur->next;
    }

    // 页表耗尽则触发分页
    if ( cur == NULL ) {

        void *pa = alloc_page();
        uint64 va = g_ufree_page;
        g_ufree_page += PGSIZE;
        user_vm_map( (pagetable_t) current->pagetable, va, PGSIZE, (uint64) pa,
                     prot_to_type( PROT_WRITE | PROT_READ, 1 ) );
        // turn this new page to a block
        BLOCK *temp = (BLOCK *) pa;
        temp->start = (uint64) pa + sizeof( BLOCK );
        temp->size = PGSIZE - sizeof( BLOCK );
        temp->va = va + sizeof( BLOCK );
        temp->next = NULL;
        // 插入新块
        if ( pre == NULL ) {
            current->free_start = temp;
            cur = temp;
        } else {
            if ( pre->va + pre->size >= ( ( pre->va ) / PGSIZE + PGSIZE ) ) {
                pre->size += PGSIZE;
                cur = pre;
            } else {
                pre->next = temp;
                cur = temp;
            }
        }
    }

    //分配块
    if ( n + sizeof( BLOCK ) < cur->size ) {
        BLOCK *to_use = (BLOCK *) ( cur->start + n );
        to_use->start = cur->start + n + sizeof( BLOCK );
        to_use->size = cur->size - n - sizeof( BLOCK );
        to_use->va = cur->va + n + sizeof( BLOCK );
        to_use->next = cur->next;
        if ( cur == current->free_start )
            current->free_start = to_use;
        else
            pre->next = to_use;
    } else {
        if ( cur == current->free_start )
            current->free_start = cur->next;
        else
            pre->next = cur->next;
    }

    // 块置入已用链表
    cur->size = n;
    cur->next = current->used_start;
    current->used_start = cur;
    return cur->va;
}

// added @lab2_challenge2
void user_better_free( uint64 va ) {
    // 寻找需要释放的块
    BLOCK *cur = current->used_start, *pre = current->free_start;
    while ( cur ) {
        if ( cur->va <= va && cur->va + cur->size > va ) break;
        pre = cur;
        cur = cur->next;
    }
    if ( cur == NULL ) {
        panic( "fail to find specific block.\n" );
    }
    // free,update used line
    if ( cur == current->used_start ) {
        current->used_start = cur->next;
    } else {
        pre->next = cur->next;
    }
    // update free line
    // find the corrent position to insert
    BLOCK *free_cur = current->free_start, *free_pre = current->used_start;
    while ( free_cur ) {
        if ( free_cur->va > cur->va )
{
    break;
}
        free_pre = free_cur;
        free_cur = free_cur->next;
    }
    // insert this block into the last
    if ( free_cur == NULL ) {
        if ( free_pre == NULL ) {
            cur->next = current->free_start;
            current->free_start = cur;
        } else {
            // merge too blocks
            if ( free_pre->va + free_pre->size >= cur->va ) {
                free_pre->size += cur->size;
            } else
                free_pre->next = cur, cur->next = NULL; // don't merge
        }
    } else {
        // insert the block into the first
        if ( free_cur == current->free_start ) {
            if ( cur->va + cur->size >=
                 free_cur->va ) // merge this and the next
            {
                cur->size += free_cur->size;
                cur->next = free_cur->next;
                current->free_start = cur;
            } else {
                cur->next = current->free_start;
                current->free_start = cur;
            }
        } else { // insert the block into the middle
            if ( free_pre->va + free_pre->size >= cur->va &&
                 cur->va + cur->size >= free_cur->va ) // merge three blocks
            {
                free_pre->size += cur->size + free_cur->size;
                free_pre->next = free_cur->next;
            } else if ( cur->va + cur->size >=
                        free_cur->va ) // merge this and the next
            {
                cur->size += free_cur->size;
                cur->next = free_cur->next;
                free_pre->next = cur;
            } else if ( free_pre->va + free_pre->size >=
                        cur->va ) // merge this and pre
            {
                free_pre->size += cur->size;
            } else // don't merge
            {
                free_pre->next = cur;
                cur->next = free_cur;
            }
        }
    }
    // cur->next=current->free_start;
    // current->free_start=cur;
}