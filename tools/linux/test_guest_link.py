"""Synthetic ELF linkage validation; no game input."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from engage_build import verify_library

@unittest.skipUnless(shutil.which('cc'), 'native C compiler required')
class GuestLink(unittest.TestCase):
    def test_missing_guest_function_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'fixture.c'
            source.write_text('extern void f_700bad(void); void synthetic_entry(void) { f_700bad(); }\n')
            library = root / 'fixture.so'
            subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
            with self.assertRaisesRegex(ValueError, 'f_700bad'): verify_library(library)
    def test_host_reference_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'fixture.c'
            source.write_text('extern void engage_host_service(void); void synthetic_entry(void) { engage_host_service(); }\n')
            library = root / 'fixture.so'
            subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
            verify_library(library)

if __name__ == '__main__': unittest.main()
