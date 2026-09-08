import contextlib
import io
import unittest
from unittest.mock import MagicMock, patch

import mailjet_setup


class MailjetSetupTests(unittest.TestCase):
    def run_setup(self, confirmation):
        board = MagicMock()
        board.request.return_value = {"mailjet": {
            "sender": "old@example.com", "recipient": "to@example.com",
            "cooldown_seconds": 1800, "armed": False,
        }}
        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch("sys.argv", ["mailjet_setup.py", "--development"]))
            stack.enter_context(patch.object(mailjet_setup, "select_port", return_value="COM6"))
            stack.enter_context(patch.object(mailjet_setup, "Board", return_value=board))
            stack.enter_context(patch.object(mailjet_setup, "read_status", return_value={"mailjet_setup": True}))
            stack.enter_context(patch.object(mailjet_setup, "check_mode"))
            stack.enter_context(patch.object(mailjet_setup, "authorize"))
            stack.enter_context(patch.object(mailjet_setup, "read_key", side_effect=["test-api", "test-secret"]))
            stack.enter_context(patch("builtins.input", side_effect=["", "", "15", confirmation]))
            stack.enter_context(contextlib.redirect_stdout(output))
            self.assertEqual(mailjet_setup.main(), 0)
        self.assertNotIn("test-secret", output.getvalue())
        self.assertNotIn("test-api", output.getvalue())
        board.close.assert_called_once()
        return board

    def test_save_defaults_and_new_interval_without_arming_or_sending(self):
        board = self.run_setup("SAVE")
        board.request.assert_any_call("mailjet_save", sender="old@example.com",
                                      recipient="to@example.com", api_key="test-api",
                                      secret_key="test-secret", cooldown_seconds=900)
        self.assertEqual([call.args[0] for call in board.request.call_args_list],
                         ["mailjet_status", "mailjet_save", "lock"])

    def test_cancel_does_not_save(self):
        board = self.run_setup("")
        self.assertEqual([call.args[0] for call in board.request.call_args_list],
                         ["mailjet_status", "lock"])

    def test_interval_rejects_out_of_bounds(self):
        with patch("builtins.input", side_effect=["0", "1441", "bad", "1"]), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mailjet_setup.alert_interval(), 60)
