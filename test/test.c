//
//  test.c
//  test
//
//  Created by eyakovlev on 06.02.16.
//  Copyright © 2016 acme. All rights reserved.
//


///////////////////////////////////////////////////////////////////////////////////////////////////

#include <mach-o/loader.h>
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/mach_port.h>
#include <mach/task.h>

#include <kern/task.h>

#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/proc.h>

#include <libkern/OSMalloc.h>
#include <libkern/version.h>

#include "test.h"
#include "sysent.h"
#include "resolver.h"

#define MSR_EFER        0xc0000080 /* extended feature register */
#define MSR_STAR        0xc0000081 /* legacy mode SYSCALL target */
#define MSR_LSTAR       0xc0000082 /* long mode SYSCALL target */
#define MSR_CSTAR       0xc0000083 /* compat mode SYSCALL target */

#define XNU_FIXED_BASE  (0xffffff8000200000ull)
#define INVALID_VADDR   ((uintptr_t)-1)

#if !defined(assert)
#   define assert(cond)    \
         ((void) ((cond) ? 0 : panic("assertion failed: %s", # cond)))
#endif

OSMallocTag g_tag = NULL;


///////////////////////////////////////////////////////////////////////////////////////////////////


static uint64_t rdmsr(uint32_t index)
{
    uint32_t lo=0, hi=0;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(index));
    return (((uint64_t)hi) << 32) | ((uint64_t)lo);
}

static void disable_vm_protection(void)
{
    __asm__ __volatile__(
                         "cli    \n\t" \
                         "mov    %%cr0, %%rax \n\t" \
                         "and    $0xfffffffffffeffff, %%rax \n\t" \
                         "mov    %%rax, %%cr0 \n\t" \
                         "sti    \n\t"
                         :::"rax"
    );
}

static void enable_vm_protection(void)
{
    __asm__ __volatile__(
                         "cli    \n\t" \
                         "mov    %%cr0, %%rax \n\t" \
                         "or     $0x10000, %%rax \n\t" \
                         "mov    %%rax, %%cr0 \n\t" \
                         "sti    \n\t"
                         :::"rax"
    );
}

// Returns 64bit kernel base address or -1 if failed
static uintptr_t find_kernel_base(void)
{
    // In case of ASLR kernel find real kernel base.
    // For that dump MSR_LSTAR which contains a pointer to kernel syscall handler
    uint64_t ptr = rdmsr(MSR_LSTAR);
    
    // Round up to next page boundary - kernel should start at a page boundary ASLR or no ALSR
    ptr = ptr & ~PAGE_MASK_64;
    
    while (ptr >= XNU_FIXED_BASE) {
        if (*(uint32_t*)ptr == MH_MAGIC_64) {
            return ptr;
        }
        
        ptr -= PAGE_SIZE;
    }
    
    return INVALID_VADDR;
}


// Returns base address and size (in bytes) of a data segment inside kernel mach binary
static uintptr_t get_data_segment(const struct mach_header_64* mh, uint64_t* out_size)
{
    if (!mh || !out_size) {
        return INVALID_VADDR;
    }
    
    if (mh->magic != MH_MAGIC_64) {
        return INVALID_VADDR;
    }
    
    uintptr_t base = (uintptr_t)mh;
    uintptr_t addr = base + sizeof(*mh);
    
    // find the last command offset
    struct load_command* lc = NULL;
    
    for (uint32_t i = 0; i < mh->ncmds; i++)
    {
        lc = (struct load_command*)addr;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sc = (struct segment_command_64 *)lc;
            if (strncmp(sc->segname, "__DATA", 16) == 0) {
                *out_size = sc->vmsize;
                return sc->vmaddr;
            }
        }
        
        // advance to next command
        addr += lc->cmdsize;
    }
    
    return INVALID_VADDR;
}


static void* find_sysent_table(uintptr_t start, size_t size)
{
    uintptr_t addr = start;
    while(size != 0) {

        #define sysent_verify(_sysent)           \
            (_sysent[SYS_exit].sy_narg == 1 &&   \
            _sysent[SYS_fork].sy_narg == 0 &&   \
            _sysent[SYS_read].sy_narg == 3 &&   \
            _sysent[SYS_wait4].sy_narg == 4 &&  \
            _sysent[SYS_ptrace].sy_narg == 4)
        
        if (version_major == 14) {
            struct sysent_yosemite* sysent = (struct sysent_yosemite*)addr;
            if (sysent_verify(sysent)) {
                return sysent;
            }
        } else if (version_major == 13) {
            struct sysent_mavericks* sysent = (struct sysent_mavericks*)addr;
            if (sysent_verify(sysent)) {
                return sysent;
            }
        } else {
            struct sysent* sysent = (struct sysent*)addr;
            if (sysent_verify(sysent)) {
                return sysent;
            }
        }
        
        #undef sysent_verify
        
        addr++;
        size--;
    }
    
    return NULL;
}


static void* find_mach_trap_table(uintptr_t start, size_t size)
{
    uintptr_t addr = start;
    while(size != 0) {

        #define traps_verify(traps)            \
            (traps[0].mach_trap_arg_count == 0 &&   \
             traps[1].mach_trap_arg_count == 0 &&   \
             traps[MACH_MSG_TRAP].mach_trap_arg_count == 7 &&   \
             traps[MACH_MSG_OVERWRITE_TRAP].mach_trap_arg_count == 8)

        if (version_major >= 13) {
            mach_trap_mavericks_t* res = (mach_trap_mavericks_t*)addr;
            if (traps_verify(res)) {
                return res;
            }
        } else {
            mach_trap_t* res = (mach_trap_t*)addr;
            if (traps_verify(res)) {
                return res;
            }
        }
        
        #undef traps_verify
        
        addr++;
        size--;
    }
    
    return NULL;
}


///////////////////////////////////////////////////////////////////////////////////////////////////


// Private kernel symbols manually resolved on kext start
static task_t(*proc_task)(proc_t) = NULL;
static ipc_space_t(*get_task_ipcspace)(task_t) = NULL;
static kern_return_t (*mach_port_names_fptr)(ipc_space_t task, mach_port_name_array_t *names, mach_msg_type_number_t *namesCnt, mach_port_type_array_t *types, mach_msg_type_number_t *typesCnt) = NULL;

// Lock protects pid changes from concurrent syscalls
static lck_grp_t* g_lock_group = NULL;
static lck_mtx_t* g_task_lock = NULL;

// Traced task context
static task_t g_task = NULL;
static int32_t g_pid = 0;       // PID we will protect, set through sysctl node
static int g_unhook = 0;        // Dummy sysctl node var to unhook syscalls

static int(*g_orig_kill)(proc_t cp, void *uap, __unused int32_t *retval) = NULL;
static mach_msg_return_t (*g_mach_msg_trap)(void* args) = NULL;

static void* g_sysent_table = NULL;
static void* g_mach_trap_table = NULL;

static void* sysent_get_call(int callnum) {
    switch(version_major) {
        case 14: return ((struct sysent_yosemite*)g_sysent_table)[callnum].sy_call;
        case 13: return ((struct sysent_mavericks*)g_sysent_table)[callnum].sy_call;
        default: return ((struct sysent*)g_sysent_table)[callnum].sy_call;
    }
}

static void sysent_set_call(int callnum, void* sy_call) {
    switch(version_major) {
        case 14: ((struct sysent_yosemite*)g_sysent_table)[callnum].sy_call = sy_call; break;
        case 13: ((struct sysent_mavericks*)g_sysent_table)[callnum].sy_call = sy_call; break;
        default: ((struct sysent*)g_sysent_table)[callnum].sy_call = sy_call; break;
    }
}

static void* mach_table_get_trap(int trapnum) {
    if (version_major >= 13) {
        return ((mach_trap_mavericks_t*)g_mach_trap_table)[trapnum].mach_trap_function;
    } else {
        return ((mach_trap_t*)g_mach_trap_table)[trapnum].mach_trap_function;
    }
}

static void mach_table_set_trap(int trapnum, void* trap_function) {
    if (version_major >= 13) {
        ((mach_trap_mavericks_t*)g_mach_trap_table)[trapnum].mach_trap_function = trap_function;
    } else {
        ((mach_trap_t*)g_mach_trap_table)[trapnum].mach_trap_function = trap_function;
    }
}

static int sysctl_killhook_pid SYSCTL_HANDLER_ARGS;
static int sysctl_killhook_unhook SYSCTL_HANDLER_ARGS;

SYSCTL_NODE(_debug, OID_AUTO, killhook, CTLFLAG_RW, 0, "kill hook API");
SYSCTL_PROC(_debug_killhook, OID_AUTO, pid, (CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_SECURE), &g_pid, 0, sysctl_killhook_pid, "I", "Protected PID");
SYSCTL_PROC(_debug_killhook, OID_AUTO, unhook, (CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_SECURE), &g_unhook, 0, sysctl_killhook_unhook, "I", "");
                  
static int sysctl_killhook_pid(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
    
    int32_t curPid = g_pid;
    int res = sysctl_handle_int(oidp, oidp->oid_arg1, oidp->oid_arg2, req);
    
    if (g_pid != curPid) {
        
        //lck_mtx_lock(g_task_lock);
        
        proc_t proc = proc_find(g_pid);
        if (proc) {
            // Use private symbols to prepare task info
            g_task = proc_task(proc);
            proc_rele(proc);
            
            //mach_port_name_array_t names;
            //mach_msg_type_number_t namesCnt;
            //mach_port_type_array_t types;
            //mach_msg_type_number_t typesCnt;
            //kern_return_t err = mach_port_names_fptr(get_task_ipcspace(g_task), &names, &namesCnt, &types, &typesCnt);
            //if (err) {
            //    printf("mach_port_names failed: %d\n", err);
            //}
            
            printf("PID changed to %d, task %p\n", g_pid, g_task);
            //printf("Got %d port names\n", namesCnt);
        }
        
        //lck_mtx_unlock(g_task_lock);

    }
    
    return res;
}

static int sysctl_killhook_unhook(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
    int res = sysctl_handle_int(oidp, oidp->oid_arg1, oidp->oid_arg2, req);
    if (g_unhook && g_sysent_table && g_mach_trap_table)
    {
        // TODO: it is dangerous to just overwrite syscalls again, we need to make sure that there are no pending syscalls
        disable_vm_protection();
        {
            mach_table_set_trap(MACH_MSG_TRAP, g_mach_msg_trap);
            sysent_set_call(SYS_kill, (sy_call_t*)g_orig_kill);
        }
        enable_vm_protection();
    }
    
    return res;
}


//
// Mach msg hooks
// Intercept task_terminate IPC on our task's kernel port
//

#define PAD_ARG_(arg_type, arg_name) \
   char arg_name##_l_[PADL_(arg_type)]; arg_type arg_name; char arg_name##_r_[PADR_(arg_type)];

#define PAD_ARG_8

struct mach_msg_overwrite_trap_args {
    PAD_ARG_(user_addr_t, msg);
    PAD_ARG_(mach_msg_option_t, option);
    PAD_ARG_(mach_msg_size_t, send_size);
    PAD_ARG_(mach_msg_size_t, rcv_size);
    PAD_ARG_(mach_port_name_t, rcv_name);
    PAD_ARG_(mach_msg_timeout_t, timeout);
    PAD_ARG_(mach_port_name_t, notify);
    PAD_ARG_8
    PAD_ARG_(user_addr_t, rcv_msg);  /* Unused on mach_msg_trap */
};

#define MIG_TASK_TERMINATE_ID 3401 /* Taken from osfmk/mach/task.defs */

typedef	struct
{
    mach_msg_bits_t     msgh_bits;
    mach_msg_size_t     msgh_size;
    __darwin_natural_t	msgh_remote_port;
    __darwin_natural_t	msgh_local_port;
    __darwin_natural_t	msgh_voucher_port;
    mach_msg_id_t		msgh_id;
} mach_user_msg_header_t;

mach_msg_return_t my_mach_msg_trap(struct mach_msg_overwrite_trap_args *args)
{
    task_t task = current_task();
    proc_t proc = current_proc();
    pid_t pid = proc_pid(proc);
    
    if (!g_pid || pid != g_pid) {
        return g_mach_msg_trap(args);
    }
    
    //lck_mtx_lock(g_task_lock);
    
    printf("my_mach_msg_trap: %p (%zu)\n", args, sizeof(*args));
    printf(" msg = %llx\n", args->msg);
    printf(" option = %x\n", args->option);
    printf(" send_size = %d\n", args->send_size);
    printf(" rcv_size = %d\n", args->rcv_size);
    printf(" timeout = %d\n", args->timeout);
    
    //mach_port_names
    //mach_port_name_t
    mach_port_t port;
    kern_return_t err = task_get_special_port(task, TASK_KERNEL_PORT, &port);
    if (err) {
        printf("Failed to get task special port: %d\n", err);
    } else {
        printf("task kernel port: %p, %d\n", port, CAST_MACH_PORT_TO_NAME(port));
    }
    
    
    /* Let's see what is inside the message */
    if ((args->option & MACH_SEND_MSG) && (args->send_size)) {
        mach_user_msg_header_t* hdr = OSMalloc(args->send_size, g_tag);
        if (hdr) {
            copyin(args->msg, hdr, args->send_size);
            for (int i = 0; i < args->send_size; ++i) {
                if ((i % 4) == 0) {
                    printf("\n");
                }
                
                printf("0x%02x ", ((uint8_t*)hdr)[i]);
            }
            printf("\n");
            
            printf(" msg bits 0x%x, size %d, id %d, %d -> %d\n", hdr->msgh_bits, hdr->msgh_size, hdr->msgh_id, hdr->msgh_local_port, hdr->msgh_remote_port);
            
            
            //if (hdr->msgh_id == MIG_TASK_TERMINATE_ID) {
            //    printf("Blocking task_terminate");
            //    return MACH_SEND_INVALID_DATA;
            //}
            
            OSFree(hdr, args->send_size, g_tag);
        }
        
        //mach_port_t remote_port = hdr->msgh_remote_port;
        //mach_port_type_t type;
        //mach_port_type(get_task_ipcspace(task), remote_port, &type);
    }
    
    //lck_mtx_unlock(g_task_lock);
    
    printf(" calling %p\n", g_mach_msg_trap);
    mach_msg_return_t res = g_mach_msg_trap(args);
    printf(" result = 0x%x\n", res);
    
    
#if 0
    if ((res == MACH_MSG_SUCCESS) && (args->option & MACH_RCV_MSG) && (args->rcv_size)) {
        printf(" LOOK AT ME MA! \n");
        
        mach_msg_header_t* hdr = OSMalloc(args->rcv_size, g_tag);
        if (hdr) {
            copyin(args->msg, hdr, args->rcv_size);
            printf(" msg bits 0x%x, size %d, id %d, %p -> %p\n", hdr->msgh_bits, hdr->msgh_size, hdr->msgh_id, hdr->msgh_local_port, hdr->msgh_remote_port);
            OSFree(hdr, args->rcv_size, g_tag);
        }

        //mach_port_t remote_port = hdr->msgh_remote_port;
        //mach_port_type_t type;
        //mach_port_type(get_task_ipcspace(task), remote_port, &type);
    }
#endif
    return res;

    
    //if (args->option & MACH_RCV_MSG) {
    //    return MACH_RCV_TIMED_OUT;
    //}
    
    //return g_mach_msg_trap(args);
    
    /*
       
    //mach_msg_header_t* hdr = OSMalloc(args->send_size, g_tag);
    //if (hdr) {
    //    copyin(args->msg, hdr, args->send_size);
    //    printf(" msg bits 0x%x, size %d, id %d, %p -> %p\n", hdr->msgh_bits, hdr->msgh_size, hdr->msgh_id, hdr->msgh_local_port, hdr->msgh_remote_port);
    //    OSFree(hdr, args->send_size, g_tag);
    //}
    
        
    if ((args->option & MACH_SEND_MSG) && (args->send_size)) {
        mach_msg_header_t* hdr = OSMalloc(args->send_size, g_tag);
        if (hdr) {
            copyin(args->msg, hdr, args->send_size);
            printf(" msg bits 0x%x, size %d, id %d, %p -> %p\n", hdr->msgh_bits, hdr->msgh_size, hdr->msgh_id, hdr->msgh_local_port, hdr->msgh_remote_port);
            
            for (int i = 0; i < args->send_size; ++i) {
                if ((i % 32) == 0) {
                    printf("\n");
                }
                
                printf(" 0x%02x", ((uint8_t*)hdr)[i]);
            }
            
            OSFree(hdr, args->send_size, g_tag);
        }
    }
    
    int res = g_mach_msg_trap(args);
        
    printf("returning %d\n", res);
    return res;
    
    
    //for (int i = 0; i < 2; ++i) {
    //    printf(" 0x%x\n", ((uint32_t*)args)[i]);
    //}

    //return g_mach_msg_trap(args);
     */
    

}


//
// BSD kill(2) hook
// Intercept BSD signals
//

struct kill_args {
    char pid_l_[PADL_(int)]; int pid; char pid_r_[PADR_(int)];
    char signum_l_[PADL_(int)]; int signum; char signum_r_[PADR_(int)];
    char posix_l_[PADL_(int)]; int posix; char posix_r_[PADR_(int)];
};

int my_kill(proc_t cp, struct kill_args *uap, __unused int32_t *retval)
{
    // Negative pid is a killpg case
    pid_t pid = (uap->pid > 0 ? uap->pid : -uap->pid);
    
    if (!g_pid || (pid != g_pid)) {
        return g_orig_kill(cp, uap, retval);
    }
    
    printf("signal %d from pid %d to pid %d, posix %d\n", uap->signum, proc_pid(cp), uap->pid, uap->posix);
    
    // TODO: process cannot ignore or handle SIGKILL so we intercept it here.
    // However there are other signals that will terminate a process if it doesn't handle or ignore these signals (i.e. SIGTERM)
    // We don't handle those here for now.
    if (uap->signum == SIGKILL || uap->signum == SIGTERM) {
        printf("blocking SIGKILL\n");
        return EPERM;
    }
    
    return g_orig_kill(cp, uap, retval);
}


//
// Entry and init
//

kern_return_t test_start(kmod_info_t * ki, void *d)
{
    g_tag = OSMalloc_Tagalloc("test.kext", OSMT_DEFAULT);
    if (!g_tag) {
        printf("Failed to allocate OSMalloc tag\n");
        return KERN_FAILURE;
    }
    
    g_lock_group = lck_grp_alloc_init("test.kext", LCK_GRP_ATTR_NULL);
    if (!g_lock_group) {
        printf("Failed to create lock group\n");
        return KERN_FAILURE;
    }
    
    g_task_lock = lck_mtx_alloc_init(g_lock_group, LCK_ATTR_NULL);
    if (!g_task_lock) {
        printf("Failed to create lock group\n");
        return KERN_FAILURE;
    }
    
    //
    // We will attempt to hook sysent table to intercept syscalls we are interested in
    // For that we will find kernel base address, find data segment in kernel mach-o headers
    // and finally search for sysent pattern in data segment
    //
    
    uintptr_t kernel_base = find_kernel_base();
    if (kernel_base == INVALID_VADDR) {
        printf("Can't find kernel base address\n");
        return KERN_FAILURE;
    }
    
    struct mach_header_64* kernel_hdr = (struct mach_header_64*)kernel_base;
    if (kernel_hdr->magic != MH_MAGIC_64) {
        printf("Wrong kernel header\n");
        return KERN_FAILURE;
    }

    printf("kernel base @ %p\n", kernel_hdr);

    // Resolve some private symbols we're going to need
    proc_task = resolve_kernel_symbol("_proc_task", kernel_base);
    get_task_ipcspace = resolve_kernel_symbol("_get_task_ipcspace", kernel_base);
    mach_port_names_fptr = resolve_kernel_symbol("_mach_port_names", kernel_base);
    if (!proc_task || !get_task_ipcspace || !mach_port_names_fptr) {
        printf("Could not resolve private symbols\n");
        return KERN_FAILURE;
    }
    
    uint64_t data_seg_size = 0;
    uint64_t data_seg_addr = get_data_segment(kernel_hdr, &data_seg_size);
    if (data_seg_addr == INVALID_VADDR) {
        printf("Can't find kernel base address\n");
        return KERN_FAILURE;
    }
    
    printf("kernel data segment @ 0x%llx, %llu bytes\n", data_seg_addr, data_seg_size);

    // TODO: non-yosemite structures
    g_sysent_table = find_sysent_table(data_seg_addr, data_seg_size);
    if (!g_sysent_table) {
        printf("Can't find syscall table\n");
        return KERN_FAILURE;
    }
    
    printf("sysent @ %p\n", g_sysent_table);

    g_mach_trap_table = find_mach_trap_table(data_seg_addr, data_seg_size);
    if (!g_mach_trap_table) {
        printf("Can't find mach trap table\n");
        return KERN_FAILURE;
    }
    
    printf("mach trap table @ %p\n", g_mach_trap_table);
    
    
    // sysent is in read-only memory since 10.8.
    // good thing that intel architecture allows us to disable vm write protection completely from ring0 with a CR0 bit
    g_orig_kill = sysent_get_call(SYS_kill);
    g_mach_msg_trap = mach_table_get_trap(MACH_MSG_TRAP);
    
    disable_vm_protection();
    {
        sysent_set_call(SYS_kill, (sy_call_t*)my_kill);
        mach_table_set_trap(MACH_MSG_TRAP, my_mach_msg_trap);
    }
    enable_vm_protection();

    sysctl_register_oid(&sysctl__debug_killhook);
    sysctl_register_oid(&sysctl__debug_killhook_pid);
    sysctl_register_oid(&sysctl__debug_killhook_unhook);

    return KERN_SUCCESS;
}

kern_return_t test_stop(kmod_info_t *ki, void *d)
{
    sysctl_unregister_oid(&sysctl__debug_killhook);
    sysctl_unregister_oid(&sysctl__debug_killhook_pid);
    sysctl_unregister_oid(&sysctl__debug_killhook_unhook);

    lck_mtx_free(g_task_lock, g_lock_group);
    lck_grp_free(g_lock_group);
    
    OSMalloc_Tagfree(g_tag);
    
    return KERN_SUCCESS;
}
