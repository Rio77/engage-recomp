from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import tempfile
import unittest
import app

class DlcImport(unittest.TestCase):
    def fixture(self, root, ident='0100a6301214f001'):
        romfs = root / ident / 'romfs'
        romfs.mkdir(parents=True)
        (romfs / 'content.bin').write_bytes(b'isolated fixture')
        return romfs

    def test_copy_and_wrong_game_and_empty_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = root / 'input'; dest = root / 'staged'
            self.fixture(source)
            imports = app.validate_dlc(source)
            app.copy_dlc(imports, dest)
            self.assertEqual((dest / '0100A6301214F001/romfs/content.bin').read_bytes(), b'isolated fixture')
            self.fixture(source, '010055d009f79001')
            with self.assertRaises(ValueError): app.validate_dlc(source)
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError): app.validate_dlc(Path(temp))

    def test_decline_cancel_and_choose(self):
        with patch.object(app, 'dialog', return_value=SimpleNamespace(returncode=1)):
            self.assertIsNone(app.choose_dlc(Path('/tmp')))
        with patch.object(app, 'dialog', return_value=SimpleNamespace(returncode=2)):
            with self.assertRaises(KeyboardInterrupt): app.choose_dlc(Path('/tmp'))
        with patch.object(app, 'dialog', return_value=SimpleNamespace(returncode=0)), \
             patch.object(app, 'choose', return_value=Path('/dlc')):
            self.assertEqual(app.choose_dlc(Path('/tmp')), Path('/dlc'))

    def test_duplicate_title_id_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.fixture(root)
            self.fixture(root, '0100A6301214F001')
            with self.assertRaises(ValueError): app.validate_dlc(root)

    def test_cancel_precedes_transaction_and_build(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            args = SimpleNamespace(root=root/'destination', exefs=root/'exefs', romfs=root/'romfs',
                dlc=None, no_dlc=False, noninteractive=False)
            with patch.object(app, 'validate'), patch.object(app, 'choose_dlc', side_effect=KeyboardInterrupt), \
                 patch.object(app, 'Installation') as transaction:
                with self.assertRaises(KeyboardInterrupt): app.install(args)
                transaction.assert_not_called()
                self.assertFalse(args.root.exists())

    def test_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            romfs = self.fixture(root)
            (romfs/'link').symlink_to(romfs/'content.bin')
            with self.assertRaises(ValueError): app.validate_dlc(root)

    def test_direct_romfs_payload_is_normalized(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); title=root/'input/0100A6301214F001'; title.mkdir(parents=True)
            (title/'e0').write_bytes(b'')  # DLC entitlement RomFS may contain an empty marker.
            app.copy_dlc(app.validate_dlc(root/'input'),root/'output')
            self.assertEqual((root/'output/0100A6301214F001/romfs/e0').read_bytes(),b'')

if __name__ == '__main__': unittest.main()
