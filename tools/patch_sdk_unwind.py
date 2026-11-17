#!/usr/bin/env python3
"""Resume an Engage SDK exception at the native checkpoint for its landing pad."""

import sys
from pathlib import Path


def main():
    sdk = Path(sys.argv[1])
    before = (
        "    c->sp = (uint64_t)(c->x[16] + 0x0ull);\n"
        "    return;\n"
        "}\n"
        "void f_7008519e30(Cpu *c) {"
    )
    after = before.replace(
        "    return;",
        "    extern void engage_resume_exception(Cpu *);\n"
        "    return engage_resume_exception(c);",
    )
    matches = []
    for chunk in sdk.glob("sdk_*.c"):
        text = chunk.read_text(encoding="utf-8")
        count = text.count(before)
        if count:
            matches.append((chunk, text, count))
    if len(matches) != 1 or matches[0][2] != 1:
        raise ValueError("No se encontró una única rutina de reanudación del SDK de Engage 2.0.0.")
    chunk, text, _ = matches[0]
    chunk.write_text(text.replace(before, after), encoding="utf-8")
    print(f"Reanudación de excepciones del SDK corregida en {chunk.name}")


if __name__ == "__main__":
    main()
