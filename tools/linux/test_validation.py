"""Synthetic dump metadata and installation input checks."""
from pathlib import Path
import tempfile
import unittest
from app import validate

class Inputs(unittest.TestCase):
    def fixture(self, root):
        exefs, romfs = root / 'exefs', root / 'romfs'
        exefs.mkdir()
        for name, ident in [('main', '8c08b9719e085f91847b5e0f935d9488'),
            ('sdk', '56ec24747ec9e7f19b3efb062483a19d1da091a8'),
            ('subsdk0', 'bffed55ea46c1b48ddeabcacdfbf81bf418125ac')]:
            header = bytearray(256)
            header[:4] = b'NSO0'
            raw = bytes.fromhex(ident)
            header[64:64+len(raw)] = raw
            (exefs / name).write_bytes(header)
        (romfs / 'Data/Managed/Metadata').mkdir(parents=True)
        (romfs / 'Data/StreamingAssets').mkdir()
        (romfs / 'Data/data.unity3d').write_bytes(b'synthetic')
        (romfs / 'Data/Managed/Metadata/global-metadata.dat').write_bytes(b'synthetic')
        return exefs, romfs
    def test_expected_metadata_and_missing_base_data(self):
        with tempfile.TemporaryDirectory() as temp:
            exefs, romfs = self.fixture(Path(temp))
            validate(exefs, romfs)
            (romfs / 'Data/data.unity3d').unlink()
            with self.assertRaisesRegex(ValueError, 'Missing base'): validate(exefs, romfs)
    def test_wrong_game_version(self):
        with tempfile.TemporaryDirectory() as temp:
            exefs, romfs = self.fixture(Path(temp))
            (exefs / 'main').write_bytes(b'NSO0' + bytes(252))
            with self.assertRaisesRegex(ValueError, '2.0.0'): validate(exefs, romfs)

if __name__ == '__main__': unittest.main()
