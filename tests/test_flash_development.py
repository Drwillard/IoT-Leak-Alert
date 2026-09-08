import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import flash_development


class BoardMetadataTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.private = self.root / 'private'
        self.private.mkdir()
        self.patch = patch.object(flash_development, 'ROOT', self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def metadata(self, mac='02:00:00:00:00:01', backup='original.bin'):
        (self.private / 'board.json').write_text(json.dumps({
            'expected_mac': mac, 'backup_file': backup,
        }))

    def test_loads_local_board_and_requires_complete_backup(self):
        self.metadata()
        with self.assertRaises(RuntimeError):
            flash_development.backed_up_board()
        with (self.private / 'original.bin').open('wb') as f:
            f.truncate(0x400000)
        self.assertEqual(flash_development.backed_up_board(), '020000000001')

    def test_rejects_missing_metadata_bad_mac_and_backup_outside_private(self):
        with self.assertRaises(RuntimeError):
            flash_development.backed_up_board()
        for mac, backup in [('invalid', 'original.bin'), ('020000000001', '../outside.bin')]:
            self.metadata(mac, backup)
            with self.assertRaises(RuntimeError):
                flash_development.backed_up_board()
