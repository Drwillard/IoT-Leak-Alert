import contextlib
import io
import unittest
from unittest.mock import patch

import email_test
import wifi_setup


class EmailTests(unittest.TestCase):
    def test_development_requires_both_opt_in_and_ready_test_firmware(self):
        with self.assertRaises(RuntimeError):
            wifi_setup.check_mode({'development': True, 'storage_ready': True})
        with self.assertRaises(RuntimeError):
            wifi_setup.check_mode({'development': False, 'storage_ready': True}, True)
        with contextlib.redirect_stdout(io.StringIO()):
            wifi_setup.check_mode({'development': True, 'storage_ready': True}, True)

    def test_rejects_header_injection(self):
        with patch('builtins.input', side_effect=['a@example.com\r\nBcc: b@example.com', 'a@example.com']), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(email_test.email_address('email'), 'a@example.com')

    def run_flow(self, confirmation, connected=True, reject=False, sandbox=False, provider='mailjet'):
        calls = []
        class FakeBoard:
            def __init__(self, port): pass
            def request(self, command, **fields):
                calls.append((command, fields))
                if command == 'status':
                    return {'protocol': 1, 'mac': 'test', 'development': True,
                            'storage_ready': True, 'email_test': True, 'connected': connected,
                            'email_provider': provider}
                if command == 'send_email':
                    if reject:
                        raise RuntimeError('Mailjet authentication failed')
                    return {'result': 'validated_by_mailjet' if sandbox else 'accepted_by_mailjet'}
                return {}
            def close(self): pass
        output = io.StringIO()
        with patch.object(email_test, 'Board', FakeBoard), \
             patch('sys.argv', ['email_test.py', '--port', 'COM6', '--development'] + (['--sandbox'] if sandbox else [])), \
             patch('builtins.input', side_effect=['sender@example.com', '', confirmation]), \
             patch.object(email_test.getpass, 'getpass', side_effect=['test-api-key', 'test-secret-key']) as keys, \
             contextlib.redirect_stdout(output):
            if reject or not connected or provider != 'mailjet':
                with self.assertRaises(RuntimeError):
                    email_test.main()
            else:
                self.assertEqual(email_test.main(), 0)
        self.assertNotIn('test-api-key', output.getvalue())
        self.assertNotIn('test-secret-key', output.getvalue())
        if provider != 'mailjet' or not connected:
            keys.assert_not_called()
        return calls, output.getvalue()

    def test_cancel_never_sends(self):
        calls, output = self.run_flow('')
        self.assertNotIn('send_email', [c[0] for c in calls])
        self.assertIn('No email sent', output)

    def test_confirmed_test_is_sent_once_by_board(self):
        calls, output = self.run_flow('SEND')
        sent = [c for c in calls if c[0] == 'send_email']
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0][1]['recipient'], 'sender@example.com')
        self.assertEqual(sent[0][1]['api_key'], 'test-api-key')
        self.assertEqual(sent[0][1]['secret_key'], 'test-secret-key')
        self.assertFalse(sent[0][1]['sandbox'])
        self.assertIn('Mailjet accepted', output)

    def test_authentication_failure_is_not_reported_as_success(self):
        calls, output = self.run_flow('SEND', reject=True)
        self.assertNotIn('Mailjet accepted', output)
        self.assertEqual(len([c for c in calls if c[0] == 'send_email']), 1)

    def test_no_wifi_never_sends(self):
        calls, _ = self.run_flow('SEND', connected=False)
        self.assertNotIn('send_email', [c[0] for c in calls])

    def test_old_firmware_never_receives_keys(self):
        calls, _ = self.run_flow('SEND', provider='gmail')
        self.assertNotIn('send_email', [c[0] for c in calls])

    def test_sandbox_is_explicit_and_not_reported_as_delivery(self):
        calls, output = self.run_flow('TEST', sandbox=True)
        sent = [c for c in calls if c[0] == 'send_email']
        self.assertEqual(len(sent), 1)
        self.assertTrue(sent[0][1]['sandbox'])
        self.assertIn('No email was delivered', output)
        self.assertNotIn('Mailjet accepted', output)

    def test_keys_reject_header_controls_and_username_colon(self):
        for value in ['key\r\nX-Header: injected', 'user:other', '', 'x' * 129]:
            with patch.object(email_test.getpass, 'getpass', return_value=value):
                with self.assertRaises(RuntimeError):
                    email_test.read_key('API key', username=True)


if __name__ == '__main__':
    unittest.main()
