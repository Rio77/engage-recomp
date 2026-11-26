"""Regression checks for local computed-branch dispatch in generated functions."""
from collections import Counter
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from gen import Insn, Lifter, Module
import reemit_main_functions as reemit


class LocalDispatchTests(unittest.TestCase):
    def setUp(self):
        self.entry = 0x7000001000
        self.end = self.entry + 16
        self.module = Module.__new__(Module)
        self.module.base = 0x7000000000
        self.module.lo = self.entry
        self.module.hi = self.end
        instructions = {
            self.entry: Insn('br', target='c->x[8]'),
            self.entry + 4: Insn('ret'),
            self.entry + 8: Insn('ret'),
            self.entry + 12: Insn('ret'),
        }
        self.module.lift = instructions.__getitem__

    def test_inferred_jump_table_keeps_all_local_fallback_targets(self):
        body = self.module.emit_function(
            self.entry, self.end, {self.entry}, {self.entry: [self.entry + 4]}, {}
        )
        dispatch = body[body.index('L_dispatch:') + 1]
        self.assertIn(f'case {self.entry + 8:#x}ull: goto L_{self.entry + 8:x};', dispatch)

    def test_landing_pad_only_dispatch_remains_exact(self):
        self.module.lift = lambda address: Insn('ret')
        body = self.module.emit_function(
            self.entry, self.end, {self.entry}, {}, {}, dispatch_targets={self.entry + 4}
        )
        dispatch = body[body.index('L_dispatch:') + 1]
        self.assertIn(f'case {self.entry + 4:#x}ull: goto L_{self.entry + 4:x};', dispatch)
        self.assertNotIn(f'case {self.entry + 8:#x}ull:', dispatch)


class ComputedBranchTests(unittest.TestCase):
    def setUp(self):
        self.entry = 0x7000001000
        self.module = Module.__new__(Module)
        self.module.base = 0x7000000000
        self.module.lo = self.entry
        self.module.lifter = Lifter(self.module.base, 'c')
        self.module.cache = {}
        self.module.stats = Counter()

    def load_words(self, *words):
        self.module.words = words
        self.module.hi = self.entry + len(words) * 4
        return self.module.hi - 4

    def test_vector_store_does_not_hide_jump_table_address(self):
        br = self.load_words(
            0x8B0B094A,  # add x10, x10, x11, lsl #2
            0x2D0137EA,  # stp s10, s13, [sp, #8]
            0x2D002FEE,  # stp s14, s11, [sp]
            0xD61F0140,  # br x10
        )
        self.assertTrue(self.module.computed_target(br, self.entry))

    def test_integer_pair_load_second_destination_clobbers_address(self):
        br = self.load_words(
            0x8B0B094A,  # add x10, x10, x11, lsl #2
            0xA9402BE0,  # ldp x0, x10, [sp]
            0xD61F0140,  # br x10
        )
        self.assertFalse(self.module.computed_target(br, self.entry))

    def test_pair_store_writeback_clobbers_address(self):
        br = self.load_words(
            0x8B0B094A,  # add x10, x10, x11, lsl #2
            0xA9810540,  # stp x0, x1, [x10, #16]!
            0xD61F0140,  # br x10
        )
        self.assertFalse(self.module.computed_target(br, self.entry))

    def test_table_target_routes_to_a_later_split_part(self):
        self.load_words(0xD61F0140, *(0xD65F03C0,) * 7)  # br x10, then seven returns
        br = self.entry
        self.module.SPLIT_ALL_LABELS_AT = 16
        self.module.SPLIT_ALL_LABELS_PART = 16
        target = self.entry + 24
        self.module.emit_function(self.entry, self.module.hi, {self.entry}, {br: [target]}, {})
        first, second = ('\n'.join(lines) for lines, _ in self.module._parts)
        self.assertIn('case 0x7000001018ull: c->pc = 0x7000001018ull;', first)
        self.assertIn('return f_7000001000_p1(c);', first)
        self.assertIn('case 0x7000001018ull: goto L_7000001018;', second)
        self.assertIn('L_7000001018:;', second)

    def test_large_extent_recovers_table_targets_beyond_the_primary_slice(self):
        self.load_words(
            0x7100051F,  # cmp w8, #1
            0x540000E8,  # b.hi 0x1020
            0x10007FC9,  # adr x9, 0x2000
            0x1000012A,  # adr x10, 0x1030
            0x7868792B,  # ldrh w11, [x9, x8, lsl #1]
            0x8B0B094A,  # add x10, x10, x11, lsl #2
            0xD61F0140,  # br x10
            *(0xD65F03C0,) * 25,
        )
        self.module.SPLIT_AT = 0x40
        self.module.SPLIT_ALL_LABELS_AT = 0x30
        current_slice_end = self.entry + 0x20
        image = bytearray(0x3000)
        struct.pack_into('<32I', image, 0x1000, *self.module.words)
        struct.pack_into('<2H', image, 0x2000, 0, 16)
        self.module.nso = SimpleNamespace(image=image)
        self.module.mu = None

        with tempfile.TemporaryDirectory() as tmp:
            generated = Path(tmp)
            (generated / 'main_table.c').write_text(
                f'void f_{self.entry:x}(Cpu *c);\nvoid f_{current_slice_end:x}(Cpu *c);\n')
            with patch.multiple(reemit, KNOWN_EXTENTS={}, EXPLICIT_EXTENTS={}), \
                    patch.object(reemit, 'unwind_extents', return_value={0x1000: 0x1080}):
                switches, _ = reemit.split_switches(self.module, generated)

        self.assertEqual(switches, {self.entry: self.module.hi})
        _, _, tables = self.module.scan(self.entry, switches[self.entry])
        self.assertEqual(tables[self.entry + 0x18], [self.entry + 0x30, self.entry + 0x70])

    def test_emitter_rejects_table_target_outside_function(self):
        br = self.load_words(0xD61F0140)
        with self.assertRaisesRegex(ValueError, 'main.*0x7000001000.*0x7000001004'):
            self.module.name = 'main'
            self.module.emit_function(self.entry, self.module.hi, {self.entry},
                                      {br: [self.module.hi]}, {})


if __name__ == '__main__':
    unittest.main()
