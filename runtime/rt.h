// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), rt_fatal_trace())

// ---- address space layout (guest addresses == host addresses)
// macOS reserves 0x2_0000_0000-0x70_0000_0000, and guest addresses must fit in 39 bits, so the
// guest lives in a 23.5 GB window at 0x70_0000_0000 claimed at startup (see main.c).
// ALIAS_SIZE is 16 GB (15 GB usable after nn::os's 0x3FE00000 reservation) so sparse virtual
// address reservations (Unity VammManager + NVN memory pools) never fragment out of 16 MB holes.
#define GUEST_LO     0x7000000000ull
#define GUEST_HI     0x75E0000000ull
#define MAIN_BASE    0x7000000000ull
#define SDK_BASE     0x7008000000ull
#define SUBSDK0_BASE 0x7010000000ull   // FE3H: glslc.nss (NVIDIA shader compiler)
#define HLE_BASE     0x7080000000ull   // fake addresses for host-implemented functions
#define HLE_SIZE     0x0000100000ull
#define TLS_BASE     0x7090000000ull
#define TLS_SIZE     0x0000100000ull
#define HLE_HEAP     0x7092000000ull   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x000E000000ull
#define ALIAS_BASE   0x70C0000000ull   // alias region (svcMapMemory / nn::os::MemoryBlock)
#define ALIAS_SIZE   0x0400000000ull   // 16 GB (0x70C0000000..0x74C0000000)
#define HEAP_BASE    0x7500000000ull
#define HEAP_MAX     0x00D0000000ull   // 3.25 GB (0x7500000000..0x75D0000000)

// ---- loader.c
typedef struct Module {
    const char *name;
    uint64_t base, size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint64_t symtab, strtab, nsyms;
} Module;

extern Module g_modules[4];
extern int g_nmodules;
void loader_load(const char *exefs_dir);  // (again after a reboot: modules are reloaded into wiped memory)
uint64_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint64_t addr);
uint64_t hle_register(const char *name, HostFn fn);  // returns fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint64_t guest_addr, HostFn fn, const char *name);
void rt_fatal_trace(void) __attribute__((noreturn));
void crash_handler_install(void);  // per thread (alternate signal stack)

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
void kernel_stop_threads(void (*idle)(void));  // reboot: unwind every guest thread and join it (idle: called while waiting)
void kernel_reset(void);                       // reboot: empty handle table, memory map, thread list
KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio);
void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- in-place reboot (main.c): switch seeds or go back to the start screen without leaving the process
enum { RT_REBOOT_NONE, RT_REBOOT_GAME, RT_REBOOT_TITLE, RT_REBOOT_REBUILD };  // GAME/TITLE: in-place; REBUILD: exit for rebuild
void rt_request_reboot(int kind);  // any thread; the window loop (ui_run) returns it and main() carries it out
int rt_reboot_requested(void);
extern volatile int g_rt_unwind;   // set while guest threads are being stopped (kernel.c)
void rt_unwind_point(void);        // guest threads: unwind to thread_main if a reboot is stopping them

// ---- ui (runtime/nvn/metal.m)
int ui_run(void);  // window loop; returns the reboot kind requested (the window stays open)

// ---- helpers
void guest_call(Cpu *c, uint64_t addr);   // call guest function at addr (saves/restores x30)
uint64_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
void *hle_alloc(uint64_t size);           // zeroed guest-visible memory (never freed)
extern const char *g_romfs_dir;
