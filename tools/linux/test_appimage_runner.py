from pathlib import Path
import subprocess
import sys
import unittest

class Runner(unittest.TestCase):
    def test_private_extraction_directories_and_cleanup(self):
        command = [sys.executable, str(Path(__file__).with_name('run_appimage.py')),
                   '/bin/sh', '-c', 'printf "%s" "$TMPDIR"; sleep 0.1']
        children = [subprocess.Popen(command, stdout=subprocess.PIPE, text=True) for _ in range(2)]
        paths = [process.communicate(timeout=10)[0] for process in children]
        self.assertNotEqual(*paths)
        for path in paths:
            self.assertTrue(Path(path).name.startswith('engage-recomp-'))
            self.assertFalse(Path(path).exists())
        self.assertTrue(all(process.returncode == 0 for process in children))
    def test_exit_status_preserved(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('run_appimage.py')),
                                 '/bin/sh', '-c', 'exit 7'])
        self.assertEqual(result.returncode, 7)
if __name__ == '__main__': unittest.main()
