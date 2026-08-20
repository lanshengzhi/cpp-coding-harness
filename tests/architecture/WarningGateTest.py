#!/usr/bin/env python3
"""Unit tests for compilation database command handling in the warning gate."""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import warning_gate as wg


class CompileCommandTest(unittest.TestCase):
    def test_check_entry_parses_shell_escaped_command(self):
        entry = {
            "directory": "/tmp/build",
            "command": "/usr/bin/c++ -DNAME='value with spaces' -I'include dir' "
            "-c 'source file.cpp'",
            "file": "/tmp/source file.cpp",
        }
        completed = subprocess.CompletedProcess([], 0, stdout="", stderr="")

        with patch.object(wg.subprocess, "run", return_value=completed) as run:
            source, diagnostic = wg.check_entry(entry, ccache=False)

        self.assertEqual(source, "/tmp/source file.cpp")
        self.assertEqual(diagnostic, "")
        run.assert_called_once_with(
            [
                "/usr/bin/c++",
                "-DNAME=value with spaces",
                "-Iinclude dir",
                "-c",
                "source file.cpp",
                "-Werror",
                "-fsyntax-only",
            ],
            cwd="/tmp/build",
            capture_output=True,
            text=True,
        )

    def test_check_entry_uses_arguments_list_without_shell_reparsing(self):
        entry = {
            "directory": "/tmp/build",
            "arguments": [
                "/usr/bin/c++",
                "-DNAME=value with spaces",
                "-Iinclude dir",
                "-c",
                "source file.cpp",
            ],
            "file": "source file.cpp",
        }
        completed = subprocess.CompletedProcess([], 0, stdout="", stderr="")

        with patch.object(wg.subprocess, "run", return_value=completed) as run:
            source, diagnostic = wg.check_entry(entry, ccache=True)

        self.assertEqual(source, "/tmp/build/source file.cpp")
        self.assertEqual(diagnostic, "")
        run.assert_called_once_with(
            [
                "ccache",
                "/usr/bin/c++",
                "-DNAME=value with spaces",
                "-Iinclude dir",
                "-c",
                "source file.cpp",
                "-Werror",
                "-fsyntax-only",
            ],
            cwd="/tmp/build",
            capture_output=True,
            text=True,
        )


class SourceSelectionTest(unittest.TestCase):
    def test_main_accepts_arguments_compilation_database(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            project_root = Path(temporary_directory)
            build_directory = project_root / "build"
            source_file = project_root / "src" / "source.cpp"
            build_directory.mkdir(parents=True)
            source_file.parent.mkdir(parents=True)
            source_file.write_text("// fixture source\\n", encoding="utf-8")
            compile_commands = project_root / "compile_commands.json"
            compile_commands.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_directory),
                            "arguments": ["/bin/true", "-c", "../src/source.cpp"],
                            "file": "../src/source.cpp",
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with contextlib.redirect_stdout(io.StringIO()):
                result = wg.main(
                    [
                        "--compile-commands",
                        str(compile_commands),
                        "--project-root",
                        str(project_root),
                        "--no-ccache",
                        "--jobs",
                        "1",
                    ]
                )

        self.assertEqual(result, 0)

    def test_is_project_source_resolves_relative_file_against_directory(self):
        entry = {
            "directory": "/tmp/project/build",
            "arguments": ["/usr/bin/c++", "-c", "../src/source.cpp"],
            "file": "../src/source.cpp",
        }

        self.assertTrue(wg.is_project_source(entry, "/tmp/project", []))
        self.assertFalse(
            wg.is_project_source(entry, "/tmp/project", ["/tmp/project/src"])
        )


if __name__ == "__main__":
    unittest.main()
