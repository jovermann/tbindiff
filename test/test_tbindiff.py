import subprocess
import tempfile
import unittest
import re
import random
from pathlib import Path


class TbindiffTest(unittest.TestCase):
    def run_diff(self, left, right, *args):
        with tempfile.TemporaryDirectory() as directory:
            a = Path(directory) / "a.bin"
            b = Path(directory) / "b.bin"
            a.write_bytes(left)
            b.write_bytes(right)
            return subprocess.run(["./tbindiff", *args, str(a), str(b)], text=True, capture_output=True)

    def run_view(self, data, *args):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.bin"
            path.write_bytes(data)
            return subprocess.run(["./tbindiff", *args, str(path)], text=True, capture_output=True)

    def test_identical_hidden_and_shown(self):
        result = self.run_diff(b"abc", b"abc")
        self.assertEqual(result.returncode, 0)
        self.assertIn("3 matching bytes", result.stdout)
        shown = self.run_diff(b"abc", b"abc", "--show-matches", "--ascii")
        self.assertEqual(shown.returncode, 0)
        self.assertIn("abc", shown.stdout)

    def test_substitution_hex_and_ascii(self):
        result = self.run_diff(b"abc", b"axc", "--hex")
        self.assertEqual(result.returncode, 1)
        self.assertIn("62 ", result.stdout)
        self.assertIn("78 ", result.stdout)
        result = self.run_diff(b"abc", b"axc", "--ascii")
        self.assertIn("b", result.stdout)
        self.assertIn("x", result.stdout)

    def test_insertion_and_deletion_are_aligned(self):
        inserted = self.run_diff(b"abcd", b"abXcd", "--ascii")
        self.assertEqual(inserted.returncode, 1)
        self.assertIn("X", inserted.stdout)
        deleted = self.run_diff(b"abXcd", b"abcd", "--ascii")
        self.assertEqual(deleted.returncode, 1)
        self.assertIn("X", deleted.stdout)

    def test_fine_diff_is_opt_in_for_unanchored_gaps(self):
        coarse = self.run_diff(b"abXcd", b"abcd", "--ascii", "--show-matches", "--width", "100")
        fine = self.run_diff(b"abXcd", b"abcd", "--ascii", "--show-matches", "--fine-diff", "--width", "100")
        self.assertIn("abXc", coarse.stdout)
        self.assertNotIn("abXc", fine.stdout)

    def test_unequal_coarse_replacement_is_one_run(self):
        result = self.run_diff(b"ABCDE", b"xy", "--ascii", "--show-matches", "--width", "100")
        data_lines = [line for line in result.stdout.splitlines() if re.match(r"^[0-9a-f]{8}:", line)]
        self.assertEqual(len(data_lines), 1)
        self.assertIn("ABCDE", data_lines[0])
        self.assertIn("xy", data_lines[0])

    def test_anchor_size_must_be_positive(self):
        result = self.run_diff(b"a", b"b", "--anchor-size", "0")
        self.assertEqual(result.returncode, 1)
        self.assertIn("positive integer", result.stdout)

    def test_help_has_examples_and_copyright_trailer(self):
        result = subprocess.run(["./tbindiff", "--help"], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertLess(result.stdout.index("Examples:"), result.stdout.index("Options:"))
        self.assertIn("Copyright (c) 2026 Johannes Overmann", result.stdout)
        self.assertIn("https://github.com/jovermann/tbindiff", result.stdout)
        self.assertTrue(result.stdout.endswith("\n"))
        self.assertFalse(result.stdout.endswith("\n\n"))

    def test_ui_requires_a_terminal(self):
        one_file = self.run_view(b"abc", "--ui")
        self.assertEqual(one_file.returncode, 2)
        self.assertIn("requires terminal output", one_file.stderr)
        two_files = self.run_diff(b"abc", b"abd", "--ui")
        self.assertEqual(two_files.returncode, 2)
        self.assertIn("requires terminal output", two_files.stderr)

    def test_argument_errors(self):
        result = self.run_diff(b"a", b"b", "--hex", "--ascii")
        self.assertEqual(result.returncode, 1)
        self.assertIn("mutually exclusive", result.stdout)

    def test_memory_limit_uses_bounded_fallback(self):
        result = self.run_diff(b"a" * 140000, b"b" * 140000, "--max-mem", "1")
        self.assertEqual(result.returncode, 1)

    def test_forced_color_width_and_progress(self):
        result = self.run_diff(b"abc", b"axc", "--color", "--width", "60", "--progress")
        self.assertEqual(result.returncode, 1)
        self.assertIn("\x1b[", result.stdout)
        self.assertIn("Diffing: done", result.stderr)
        plain_lines = [re.sub(r"\x1b\[[0-9;]*m", "", line) for line in result.stdout.splitlines()]
        self.assertTrue(all(len(line) <= 60 for line in plain_lines))

    def test_hex_ascii_column_has_no_bars(self):
        result = self.run_diff(b"A", b"B", "--hex", "--width", "80")
        self.assertNotIn("|A|", result.stdout)
        self.assertNotIn("|B|", result.stdout)

    def test_hex_rows_group_each_eight_bytes(self):
        result = self.run_view(bytes(range(16)), "--hex", "--width", "100")
        self.assertIn("07  08", result.stdout)

    def test_ascii_control_non_ascii_and_space_annotations(self):
        result = self.run_diff(b"\n \t\x00\x07\x08\x7f\x01\x80", b"\r \t\x02\x03\x04\x05\x06\xff", "--ascii", "--show-matches",
                               "--annotate-spaces", "--width", "100")
        self.assertIn("<LF>", result.stdout)
        self.assertIn("<CR>", result.stdout)
        self.assertIn("<SP>", result.stdout)
        self.assertIn("<TAB>", result.stdout)
        self.assertIn("<x00>", result.stdout)
        self.assertIn("<x07>", result.stdout)
        self.assertIn("<x08>", result.stdout)
        self.assertIn("<x7F>", result.stdout)
        self.assertNotIn("<NUL>", result.stdout)
        self.assertNotIn("<BEL>", result.stdout)
        self.assertNotIn("<BS>", result.stdout)
        self.assertNotIn("<DEL>", result.stdout)
        self.assertIn("<x01>", result.stdout)
        self.assertIn("<x02>", result.stdout)
        self.assertIn("<x80>", result.stdout)
        self.assertIn("<xFF>", result.stdout)

    def test_default_mode_is_autodetected(self):
        text = self.run_diff(b"plain text", b"plain Text")
        self.assertIn("t", text.stdout)
        self.assertIn("T", text.stdout)
        self.assertNotIn("74 ", text.stdout)
        binary = self.run_diff(b"\x80abc", b"\x81abc")
        self.assertIn("80 ", binary.stdout)
        self.assertIn("81 ", binary.stdout)
        controls = self.run_diff(b"\x00abcdefghi", b"\x7fabcdefghi")
        self.assertIn("00 ", controls.stdout)
        self.assertIn("7f ", controls.stdout)
        named_controls = self.run_diff(b"\tabcdefghi", b"\nabcdefghi")
        self.assertIn("<TAB>", named_controls.stdout)
        self.assertIn("<LF>", named_controls.stdout)

    def test_ascii_rows_fill_rendered_width(self):
        result = self.run_diff(b"A" * 20, b"B" * 20, "--ascii", "--width", "80")
        data_lines = [line for line in result.stdout.splitlines() if re.match(r"^[0-9a-f]{8}:", line)]
        self.assertEqual(len(data_lines), 1)

    def test_single_file_ascii_viewer(self):
        result = self.run_view(b"hello\n world", "--ascii", "--annotate-spaces", "--width", "40")
        self.assertEqual(result.returncode, 0)
        self.assertRegex(result.stdout, r"00000000: hello<LF>\n00000006: <SP>world")
        self.assertNotIn(" | ", result.stdout)

    def test_single_file_hex_viewer_and_detection(self):
        result = self.run_view(bytes(range(128, 145)), "--width", "100")
        self.assertEqual(result.returncode, 0)
        self.assertIn("80 81 82 83", result.stdout)
        self.assertNotIn(" | ", result.stdout)

    def test_ascii_diff_linefeed_ends_rendered_row(self):
        result = self.run_diff(b"left\nafter", b"LEFT\nAFTER", "--ascii", "--show-matches", "--width", "100")
        data_lines = [line for line in result.stdout.splitlines() if re.match(r"^[0-9a-f]{8}:", line)]
        linefeed_lines = [line for line in data_lines if "<LF>" in line]
        self.assertEqual(len(linefeed_lines), 1)
        self.assertNotIn("after", linefeed_lines[0])
        self.assertNotIn("AFTER", linefeed_lines[0])

    def test_matching_anchor_is_expanded_to_full_sequence(self):
        rng = random.Random(7)
        common = bytes(rng.randrange(ord("a"), ord("z") + 1) for _ in range(512))
        result = self.run_diff(b"static removed section\n\nstatic " + common, b"static " + common,
                               "--ascii", "--show-matches", "--width", "160")
        matching_static = [line for line in result.stdout.splitlines() if line.count("static") >= 2]
        self.assertTrue(matching_static)


if __name__ == "__main__":
    unittest.main(verbosity=2)
