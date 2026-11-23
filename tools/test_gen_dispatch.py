"""Regression checks for local computed-branch dispatch in generated functions."""
import unittest

from gen import Insn, Module


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


if __name__ == '__main__':
    unittest.main()
