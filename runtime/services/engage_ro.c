#include "hle.h"
#include "ipc.h"

static void bind_slot(const char *name, const void *slot) {
    uint64_t address = loader_find_symbol(name);
    if (!address) FATAL("Engage loader symbol %s is missing\n", name);
    ST64(address, (uintptr_t)slot);
}

static void *empty_module_list(void) {
    uint64_t *list = hle_alloc(2 * sizeof *list);
    list[0] = list[1] = (uintptr_t)list;
    return list;
}

void engage_ro_boot(void) {
    bind_slot("_ZN2nn2ro6detail15g_pAutoLoadListE", empty_module_list());
    bind_slot("_ZN2nn2ro6detail17g_pManualLoadListE", empty_module_list());
    bind_slot("_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE", hle_alloc(8));
    bind_slot("_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE", hle_alloc(8));
    bind_slot("_ZN2nn2ro6detail14g_pRoDebugFlagE", hle_alloc(8));
}

HLE_FUNC("_ZN2nn2ro12LookupSymbolEPmPKc", engage_lookup_symbol) {
    uint64_t address = loader_find_symbol((const char *)(uintptr_t)c->x[1]);
    if (address) ST64(c->x[0], address);
    c->x[0] = address ? RES_OK : MAKE_RESULT(22, 5);
}

SERVICE("ldr:ro", engage_ldr_ro) {
    (void)s;
    if (req->cmd == 4) return;  // Attach this process to its loader session.
    LOG("[engage-ro] unsupported command %u\n", req->cmd);
    resp->result = MAKE_RESULT(1, 120);
}
