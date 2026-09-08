import contextlib
import io
import json
import unittest
from unittest.mock import patch

import wifi_setup


class FakeSerial:
    def __init__(self, replies):
        self.replies = bytearray(replies)
        self.sent = []

    def read(self, count):
        result = self.replies[:count]
        del self.replies[:count]
        return bytes(result)

    def write(self, payload):
        self.sent.append(json.loads(payload))


class ProtocolTests(unittest.TestCase):
    def board(self, replies):
        board = wifi_setup.Board.__new__(wifi_setup.Board)
        board.serial = FakeSerial(replies)
        board.sequence = 0
        return board

    def test_ignores_boot_noise_and_unrelated_reply(self):
        board = self.board(b'boot message\n{"id":99,"ok":true}\n{"id":1,"ok":true,"protocol":1}\n')
        self.assertEqual(board.request('status')['protocol'], 1)
        self.assertEqual(board.serial.sent, [{'id': 1, 'cmd': 'status'}])

    def test_board_error_is_not_success(self):
        board = self.board(b'{"id":1,"ok":false,"error":"Previous network retained."}\n')
        with self.assertRaisesRegex(RuntimeError, 'Previous network retained'):
            board.request('connect')

    def test_unsecured_board_never_gets_credentials(self):
        class Unsecured:
            calls = []
            def __init__(self, port): pass
            def request(self, command, **kwargs):
                self.calls.append(command)
                return {'protocol': 1, 'mac': '00:00:00:00:00:00', 'secure_storage': False}
            def close(self): pass
        with patch.object(wifi_setup, 'Board', Unsecured), \
             patch('sys.argv', ['wifi_setup.py', '--port', 'COM6']), \
             patch('builtins.input', side_effect=AssertionError('Must not prompt for credentials')), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'No password'):
                wifi_setup.main()
        self.assertEqual(Unsecured.calls, ['status', 'lock'])

    def test_hostile_ssid_cannot_control_terminal(self):
        self.assertNotIn('\x1b', wifi_setup.safe_text('evil\x1b[2J\n'))
        self.assertNotIn('\n', wifi_setup.safe_text('evil\n'))

    def test_password_is_not_echoed_on_getpass_fallback(self):
        with patch.object(wifi_setup.getpass, 'getpass', side_effect=wifi_setup.getpass.GetPassWarning):
            with self.assertRaisesRegex(RuntimeError, 'real terminal'):
                wifi_setup.read_password()

    def test_hidden_network_validates_byte_length(self):
        with patch('builtins.input', side_effect=['h', 'é' * 17, 'h', 'my wifi']), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(wifi_setup.choose_network([]), ('connect', 'my wifi'))

    def test_failed_attempt_can_retry_without_logging_password(self):
        class RetryBoard:
            attempts = 0
            calls = []
            def __init__(self, port): pass
            def request(self, command, **kwargs):
                self.calls.append(command)
                if command == 'status':
                    return {'protocol': 1, 'mac': 'test', 'secure_storage': True}
                if command == 'scan':
                    return {'networks': [{'ssid': 'Home', 'rssi': -50, 'supported': True}]}
                if command == 'connect':
                    self.attempts += 1
                    if self.attempts == 1:
                        raise RuntimeError('Previous network retained.')
                    return {'ip': '192.0.2.1'}
                return {}
            def close(self): pass
        output = io.StringIO()
        with patch.object(wifi_setup, 'Board', RetryBoard), \
             patch('sys.argv', ['wifi_setup.py', '--port', 'COM6']), \
             patch('builtins.input', side_effect=['', '1', '1']), \
             patch.object(wifi_setup.getpass, 'getpass', return_value='secret-test-pass'), \
             contextlib.redirect_stdout(output):
            self.assertEqual(wifi_setup.main(), 0)
        self.assertEqual(RetryBoard.calls.count('connect'), 2)
        self.assertEqual(RetryBoard.calls[-1], 'lock')
        self.assertNotIn('secret-test-pass', output.getvalue())
        self.assertIn('192.0.2.1', output.getvalue())

    def test_retries_read_only_handshake_while_board_boots(self):
        class BootingBoard:
            status_calls = 0
            def __init__(self, port): pass
            def request(self, command, **kwargs):
                if command == 'status':
                    BootingBoard.status_calls += 1
                    if BootingBoard.status_calls < 3:
                        raise wifi_setup.BoardTimeout('booting')
                    return {'protocol': 1, 'mac': 'test', 'secure_storage': False}
                return {}
            def close(self): pass
        with patch.object(wifi_setup, 'Board', BootingBoard), \
             patch('sys.argv', ['wifi_setup.py', '--port', 'COM6']), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'No password'):
                wifi_setup.main()
        self.assertEqual(BootingBoard.status_calls, 3)


if __name__ == '__main__':
    unittest.main()
