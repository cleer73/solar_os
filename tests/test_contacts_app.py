from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ContactsAppTest(unittest.TestCase):
    def test_actions_scope_confirmation_refresh_and_compact_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "contacts"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-include", "tests/host/compat.h",
                "-Itests/host", "-Isrc", "-Isrc/apps", "-Isrc/services", "-Isrc/shell",
                "-idirafter", "include", "tests/host/contacts_app_test.c",
                "src/services/solar_os_tui_widgets.c", "-Wl,--gc-sections", "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)
