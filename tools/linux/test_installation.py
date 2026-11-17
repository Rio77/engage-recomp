"""Synthetic transaction tests: no game data required."""
import tempfile
import unittest
from pathlib import Path
from install_core import Installation, digest, InstallError

class Transactions(unittest.TestCase):
    def test_dlc_retained_and_selected_title_replaced_whole(self):
        with tempfile.TemporaryDirectory() as temp:
            inst = Installation(Path(temp)/'root')
            app = Path(temp)/'app'; app.write_bytes(b'fixture')
            with inst.lock():
                stored = inst.add_app(app, digest(app))
                for title in ('0100A6301214F001', '0100A6301214F002'):
                    folder=inst.root/'extracted/dlc'/title/'romfs'
                    folder.mkdir(parents=True); (folder/'old').write_text('old')
                token,work=inst.workspace(); stage=work/'data/extracted'
                new=stage/'dlc/0100A6301214F001/romfs'
                new.mkdir(parents=True); (new/'new').write_text('new')
                inst.commit(token,digest(stored),'test')
                self.assertFalse((inst.root/'extracted/dlc/0100A6301214F001/romfs/old').exists())
                self.assertTrue((inst.root/'extracted/dlc/0100A6301214F001/romfs/new').exists())
                self.assertTrue((inst.root/'extracted/dlc/0100A6301214F002/romfs/old').exists())

    def test_activation_and_save_preservation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'installation with spaces'
            inst = Installation(root)
            (root / 'save').mkdir()
            (root / 'save/slot').write_bytes(b'precious save')
            app = Path(temp) / 'input.AppImage'
            app.write_bytes(b'synthetic application')
            with inst.lock():
                stored = inst.add_app(app, digest(app))
                for text in ('first', 'second'):
                    token, work = inst.workspace()
                    stage = work / 'data/extracted'
                    stage.mkdir()
                    (stage / 'content').write_text(text)
                    inst.commit(token, digest(stored), 'test')
                    inst.cleanup_workspace(token)
                self.assertEqual((root / 'extracted/content').read_text(), 'second')
                self.assertEqual((root / 'save/slot').read_bytes(), b'precious save')
                self.assertEqual(len(list((root / '.recovery').glob('extracted-*'))), 1)
    def test_interrupted_switch(self):
        for point in ('prepared', 'old-moved', 'new-moved', 'manifest-written'):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as temp:
                inst = Installation(Path(temp) / 'root')
                app = Path(temp) / 'app'
                app.write_bytes(b'test')
                with inst.lock():
                    stored = inst.add_app(app, digest(app))
                    (inst.root / 'extracted').mkdir()
                    (inst.root / 'extracted/value').write_text('old')
                    token, work = inst.workspace()
                    (work / 'data/extracted').mkdir()
                    (work / 'data/extracted/value').write_text('new')
                    def fail(where):
                        if where == point: raise RuntimeError('simulated interruption')
                    with self.assertRaises(RuntimeError): inst.commit(token, digest(stored), 'test', fail)
                    inst.recover()
                    self.assertIn((inst.root / 'extracted/value').read_text(), ('old', 'new'))
                    self.assertFalse(inst.journal.exists())
    def test_invalid_app_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            inst = Installation(Path(temp) / 'root')
            app = Path(temp) / 'app'
            app.write_bytes(b'wrong')
            with self.assertRaises(InstallError): inst.add_app(app, '0' * 64)

if __name__ == '__main__': unittest.main()
