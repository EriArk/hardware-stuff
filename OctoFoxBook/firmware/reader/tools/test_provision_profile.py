"""Provisioning checks without a physical board or real credentials."""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import provision_reader as tool


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / ".env"
        self.values = {
            "ABYSS_WIFI_SSID": "test-network",
            "ABYSS_WIFI_PASSWORD": "test-wifi-secret",
            "ABYSS_OPDS_URL": "https://example.test/api/v1/opds",
            "ABYSS_OPDS_USERNAME": "test-reader",
            "ABYSS_OPDS_PASSWORD": "test-account-secret",
        }
        self.path.write_text("\n".join(f"{k}={v}" for k, v in self.values.items()))

    def test_check_only_never_opens_device_or_logs_secrets(self):
        output = io.StringIO()
        with patch.dict(tool.os.environ, {}, clear=True), \
                patch.object(tool.sys, "argv", ["provision", "--env-file", str(self.path),
                                                "--check-only"]), \
                patch.object(tool, "open_with_retry") as connection, \
                contextlib.redirect_stdout(output):
            self.assertEqual(tool.main(), 0)
            connection.assert_not_called()
        self.assertIn("PROFILE_VALID", output.getvalue())
        for value in self.values.values():
            self.assertNotIn(value, output.getvalue())

    def test_selected_account_sent_before_atomic_commit(self):
        with patch.object(tool, "transact") as send:
            tool.provision(object(), self.values)
        commands = [call.args[1] for call in send.call_args_list]
        self.assertEqual(commands[0], "PROVISION BEGIN")
        self.assertEqual(commands[-1], "PROVISION COMMIT")
        self.assertEqual(len(commands), 7)
        for key, field in tool.ENV_TO_FIELD:
            self.assertIn(f"PROVISION FIELD {field} {tool.encoded(self.values[key])}", commands)
        self.assertFalse(any("CLEAR" in command for command in commands))

    def test_missing_password_rejected(self):
        self.path.write_text("ABYSS_OPDS_USERNAME=test-reader\n")
        with patch.dict(tool.os.environ, {}, clear=True), self.assertRaises(ValueError):
            tool.load_configuration(self.path)

    def test_resume_only_releases_pause_without_requesting_network(self):
        with patch.object(tool, "transact") as send:
            tool.resume_sync(object())
        self.assertEqual(send.call_count, 1)
        self.assertEqual(send.call_args.args[1], "SYNC RESUME")

    def test_restart_verification_releases_diagnostic_pause(self):
        with patch.object(tool, "open_with_retry"), patch.object(tool.time, "sleep"), \
                patch.object(tool, "transact", return_value=["PROVISION STATUS configured=true fields=complete"]) as send:
            tool.verify_after_restart("NO_DEVICE", False)
        self.assertEqual([call.args[1] for call in send.call_args_list], ["PROVISION STATUS", "SYNC RESUME"])


if __name__ == "__main__":
    unittest.main()
