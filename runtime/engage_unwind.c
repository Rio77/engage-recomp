#include <stdlib.h>
#include "rt.h"
#include "engage_unwind.h"

static _Thread_local EngageEhFrame *eh_top;

void engage_eh_enter(EngageEhFrame *frame, Cpu *c, uint64_t begin, uint64_t end) {
    frame->previous = eh_top;
    frame->begin = begin;
    frame->end = end;
    frame->sp = c->sp;
    frame->fp = c->x[29];
    eh_top = frame;
}

void engage_eh_leave(EngageEhFrame *frame) {
    if (eh_top != frame) FATAL("Engage exception frame mismatch\n");
    eh_top = frame->previous;
}

void engage_resume_exception(Cpu *c) {
    uint64_t pc = c->x[30];
    for (EngageEhFrame *frame = eh_top; frame; frame = frame->previous) {
        if (pc >= frame->begin && pc < frame->end && c->sp == frame->sp && c->x[29] == frame->fp) {
            c->pc = pc;
            eh_top = frame;
            if (getenv("ENGAGE_TRACE_EXCEPTIONS"))
                LOG("[engage-eh] resume %llx sp=%llx\n", pc, c->sp);
            longjmp(frame->landing, 1);
        }
    }
    FATAL("Engage exception landing pad %llx sp=%llx fp=%llx has no native frame\n",
          pc, c->sp, c->x[29]);
}
