#include "cpu.h"

// Keep the high half of the monitor in the existing reserved CPU-state space.
// The low address bits distinguish this monitor from scalar LDXR/STXR.
__attribute__((target("cx16")))
void engage_ldxp(Cpu *c, uint64_t addr, unsigned wide, uint64_t out[2]) {
    if (wide) {
        __uint128_t value = __sync_val_compare_and_swap((__uint128_t *)(uintptr_t)addr, 0, 0);
        out[0] = (uint64_t)value;
        out[1] = (uint64_t)(value >> 64);
    } else {
        uint64_t value = __atomic_load_n((uint64_t *)(uintptr_t)addr, __ATOMIC_ACQUIRE);
        out[0] = (uint32_t)value;
        out[1] = value >> 32;
    }
    c->excl_addr = addr | (wide ? 2 : 1);
    c->excl_val = out[0];
    memcpy(c->_pad, &out[1], sizeof out[1]);
}

__attribute__((target("cx16")))
uint32_t engage_stxp(Cpu *c, uint64_t addr, unsigned wide, uint64_t lo, uint64_t hi) {
    uint64_t monitor = c->excl_addr;
    c->excl_addr = ~0ull;
    if (monitor != (addr | (wide ? 2 : 1))) return 1;
    uint64_t old_hi;
    memcpy(&old_hi, c->_pad, sizeof old_hi);
    if (wide) {
        __uint128_t expected = ((__uint128_t)old_hi << 64) | c->excl_val;
        __uint128_t value = ((__uint128_t)hi << 64) | lo;
        return __sync_val_compare_and_swap((__uint128_t *)(uintptr_t)addr, expected, value) != expected;
    }
    uint64_t expected = (old_hi << 32) | c->excl_val;
    uint64_t value = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
    return !__atomic_compare_exchange_n((uint64_t *)(uintptr_t)addr, &expected, value, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
