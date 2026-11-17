#pragma once
#include <setjmp.h>
#include "cpu.h"

typedef struct EngageEhFrame {
    struct EngageEhFrame *previous;
    jmp_buf landing;
    uint64_t begin, end, sp, fp;
} EngageEhFrame;

void engage_eh_enter(EngageEhFrame *frame, Cpu *c, uint64_t begin, uint64_t end);
void engage_eh_leave(EngageEhFrame *frame);

#define ENGAGE_EH_CALL(c, begin, end, call) do {                     \
    EngageEhFrame frame;                                           \
    engage_eh_enter(&frame, (c), (begin), (end));                    \
    if (setjmp(frame.landing) == 0) {                               \
        call;                                                      \
        engage_eh_leave(&frame);                                   \
    } else {                                                       \
        engage_eh_leave(&frame);                                   \
        goto L_dispatch;                                           \
    }                                                              \
} while (0)
