#!/usr/bin/env python3
"""Unit tests for the Runtime-only install tooling (issue #472).

Covers the dependency-closure audit (ldd parsing, allowlist schema, closure
rules) and the install freshness check (Gate evidence, depfile/Makefile
parsing, and rebuild-required staleness detection) with synthetic fixtures.

Run directly: `python3 tests/install/install_tools_test.py`, or through the
CTest case `cch_install_tools_unit` registered in CMakeLists.txt.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "cmake" / "install"))

import audit_runtime_deps as audit  # noqa: E402
import check_install_freshness as freshness  # noqa: E402

ALLOWLIST_PATH = REPO_ROOT / "cmake" / "install" / "runtime-deps.json"

LDD_CLEAN = """\
\tlinux-vdso.so.1 (0x00007ffe3b1fe000)
\tlibstdc++.so.6 => /usr/lib/libstdc++.so.6 (0x00007f6beba00000)
\tlibm.so.6 => /usr/lib/libm.so.6 (0x00007f6beb8cd000)
\tlibgcc_s.so.1 => /usr/lib/libgcc_s.so.1 (0x00007f6bebdd3000)
\tlibc.so.6 => /usr/lib/libc.so.6 (0x00007f6beb600000)
\t/lib64/ld-linux-x86-64.so.2 (0x00007f6bedc33000)
"""

LDD_UBUNTU_LAYOUT = """\
\tlinux-vdso.so.1 (0x00007ffd5a7a5000)
\tlibstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6 (0x00007f9c6a200000)
\tlibm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x00007f9c69f19000)
\tlibgcc_s.so.1 => /lib/x86_64-linux-gnu/libgcc_s.so.1 (0x00007f9c69c00000)
\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f9c69a00000)
\t/lib64/ld-linux-x86-64.so.2 (0x00007f9c6a600000)
"""

# When the loader path is a symlink, ldd resolves it and prints the
# `<link> => <target>` form with a path as the name.
LDD_SYMLINKED_LOADER = """\
\tlinux-vdso.so.1 (0x00007ffe3b1fe000)
\tlibstdc++.so.6 => /usr/lib/libstdc++.so.6 (0x00007f6beba00000)
\tlibm.so.6 => /usr/lib/libm.so.6 (0x00007f6beb8cd000)
\tlibgcc_s.so.1 => /usr/lib/libgcc_s.so.1 (0x00007f6bebdd3000)
\tlibc.so.6 => /usr/lib/libc.so.6 (0x00007f6beb600000)
\t/lib64/ld-linux-x86-64.so.2 => /usr/lib64/ld-linux-x86-64.so.2 (0x00007f6bedc33000)
"""


def load_allowlist():
    return audit.load_allowlist(ALLOWLIST_PATH)


class ParseLddOutputTest(unittest.TestCase):
    def test_parses_the_measured_closure(self):
        deps = audit.parse_ldd_output(LDD_CLEAN)
        by_name = {dep.name: dep for dep in deps}
        self.assertEqual(
            sorted(by_name),
            [
                "ld-linux-x86-64.so.2",
                "libc.so.6",
                "libgcc_s.so.1",
                "libm.so.6",
                "libstdc++.so.6",
                "linux-vdso.so.1",
            ],
        )
        self.assertIsNone(by_name["linux-vdso.so.1"].path)
        self.assertEqual(
            by_name["libstdc++.so.6"].path, "/usr/lib/libstdc++.so.6"
        )
        self.assertEqual(
            by_name["ld-linux-x86-64.so.2"].path,
            "/lib64/ld-linux-x86-64.so.2",
        )

    def test_marks_not_found_entries_unresolved(self):
        deps = audit.parse_ldd_output("\tlibmissing.so.7 => not found\n")
        self.assertEqual(len(deps), 1)
        self.assertEqual(deps[0].name, "libmissing.so.7")
        self.assertIsNone(deps[0].path)

    def test_rejects_a_line_without_a_resolvable_form(self):
        with self.assertRaises(ValueError):
            audit.parse_ldd_output("\tgarbage without an address\n")


class LoadAllowlistTest(unittest.TestCase):
    def test_accepts_the_checked_in_allowlist(self):
        allowlist = load_allowlist()
        self.assertIn("libc.so.6", allowlist.system)
        self.assertIn("linux-vdso.so.1", allowlist.virtual)
        self.assertIn("ld-linux-x86-64.so.2", allowlist.loader)

    def _write_allowlist(self, document):
        handle = tempfile.NamedTemporaryFile(
            "w", suffix=".json", delete=False, encoding="utf-8"
        )
        with handle:
            json.dump(document, handle)
        self.addCleanup(os.unlink, handle.name)
        return handle.name

    def test_rejects_an_unknown_schema_version(self):
        path = self._write_allowlist({"schema_version": 99})
        with self.assertRaises(ValueError):
            audit.load_allowlist(path)

    def test_rejects_unknown_fields(self):
        path = self._write_allowlist(
            {
                "schema_version": 1,
                "allowed_system": ["libc.so.6"],
                "surprise": [],
            }
        )
        with self.assertRaises(ValueError):
            audit.load_allowlist(path)


class AuditDependenciesTest(unittest.TestCase):
    def test_clean_closure_passes(self):
        diagnostics = audit.audit_dependencies(
            audit.parse_ldd_output(LDD_CLEAN), load_allowlist(), []
        )
        self.assertEqual(diagnostics, [])

    def test_ubuntu_merged_usr_layout_passes(self):
        diagnostics = audit.audit_dependencies(
            audit.parse_ldd_output(LDD_UBUNTU_LAYOUT), load_allowlist(), []
        )
        self.assertEqual(diagnostics, [])

    def test_symlinked_loader_resolution_passes(self):
        deps = audit.parse_ldd_output(LDD_SYMLINKED_LOADER)
        by_name = {dep.name: dep for dep in deps}
        self.assertEqual(
            by_name["ld-linux-x86-64.so.2"].path,
            "/usr/lib64/ld-linux-x86-64.so.2",
        )
        diagnostics = audit.audit_dependencies(deps, load_allowlist(), [])
        self.assertEqual(diagnostics, [])

    def test_undeclared_library_fails(self):
        text = LDD_CLEAN + "\tlibcurl.so.4 => /usr/lib/libcurl.so.4 (0x00007f1a00000000)\n"
        diagnostics = audit.audit_dependencies(
            audit.parse_ldd_output(text), load_allowlist(), []
        )
        self.assertEqual(
            diagnostics, ["undeclared runtime dependency: libcurl.so.4"]
        )

    def test_unresolved_library_fails(self):
        text = "\tlibc.so.6 => not found\n"
        diagnostics = audit.audit_dependencies(
            audit.parse_ldd_output(text), load_allowlist(), []
        )
        self.assertEqual(
            diagnostics, ["unresolved runtime dependency: libc.so.6 (not found)"]
        )

    def test_build_tree_resolution_fails(self):
        with tempfile.TemporaryDirectory() as build_root:
            text = (
                "\tlibc.so.6 => "
                + os.path.join(build_root, "vcpkg_installed", "x64-linux", "lib", "libc.so.6")
                + " (0x00007f1a00000000)\n"
            )
            diagnostics = audit.audit_dependencies(
                audit.parse_ldd_output(text), load_allowlist(), [build_root]
            )
        self.assertEqual(len(diagnostics), 1)
        self.assertIn("forbidden runtime dependency path", diagnostics[0])
        self.assertIn("libc.so.6", diagnostics[0])

    def test_path_outside_system_roots_fails(self):
        text = "\tlibc.so.6 => /opt/toolchain/lib/libc.so.6 (0x00007f1a00000000)\n"
        diagnostics = audit.audit_dependencies(
            audit.parse_ldd_output(text), load_allowlist(), []
        )
        self.assertEqual(len(diagnostics), 1)
        self.assertIn("outside the allowed system roots", diagnostics[0])

    def test_diagnostics_are_sorted(self):
        text = (
            "\tlibz.so.1 => /usr/lib/libz.so.1 (0x00007f1a00000000)\n"
            "\tlibaaa.so.1 => /usr/lib/libaaa.so.1 (0x00007f1a00000000)\n"
        )
        diagnostics = audit.audit_dependencies(
            audit.parse_ldd_output(text), load_allowlist(), []
        )
        self.assertEqual(diagnostics, sorted(diagnostics))
        self.assertEqual(len(diagnostics), 2)


class AuditCliTest(unittest.TestCase):
    def run_audit(self, *args):
        return subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "cmake" / "install" / "audit_runtime_deps.py"),
                *args,
            ],
            capture_output=True,
            text=True,
        )

    def test_missing_binary_fails_closed(self):
        result = self.run_audit(
            "--binary", "/nonexistent/cpp_harness", "--allowlist", str(ALLOWLIST_PATH)
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nonexistent", result.stderr + result.stdout)

    def test_non_elf_binary_fails_closed(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write("not an executable\n")
            path = handle.name
        self.addCleanup(os.unlink, path)
        result = self.run_audit(
            "--binary", path, "--allowlist", str(ALLOWLIST_PATH)
        )
        self.assertNotEqual(result.returncode, 0)


class ParseMakefileDepfileTest(unittest.TestCase):
    def test_reads_first_rule_prerequisites(self):
        text = (
            "CMakeFiles/cch.dir/src/agent/Agent.cpp.o.ddi: \\\n"
            " /repo/src/agent/Agent.cpp \\\n"
            " /usr/include/stdc-predef.h \\\n"
            " /repo/src/agent/include/cch/agent/Agent.hpp\n"
            "/usr/include/stdc-predef.h:\n"
        )
        self.assertEqual(
            freshness.parse_makefile_depfile(text),
            [
                "/repo/src/agent/Agent.cpp",
                "/usr/include/stdc-predef.h",
                "/repo/src/agent/include/cch/agent/Agent.hpp",
            ],
        )

    def test_unescapes_spaces_in_paths(self):
        text = "obj.o: /repo/my\\ dir/source.cpp /repo/other.cpp\n"
        self.assertEqual(
            freshness.parse_makefile_depfile(text),
            ["/repo/my dir/source.cpp", "/repo/other.cpp"],
        )


class CheckFreshnessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.binary = self.root / "cpp_harness"
        self.binary.write_bytes(b"elf")
        self.report = self.root / "parity-build-gate.json"
        self.report.write_text(json.dumps({"ok": True, "diagnostics": []}))
        self.depfiles = self.root / "parity-build-gate-depfiles.json"

    def write_depfile(self, name, prerequisites, *, depfile_is_older=False):
        """Write one depfile and its inputs; prerequisites sit at a fixed mtime
        and the depfile is newer by default (a depfile_is_older depfile is
        stale, as after a post-build source edit)."""
        source = self.root / f"{name}.cpp"
        source.write_text("// source\n")
        all_inputs = [source, *prerequisites]
        depfile = self.root / f"{name}.d"
        body = f"{name}.o: \\\n"
        body += " \\\n".join(f" {path}" for path in all_inputs)
        depfile.write_text(body + "\n")
        base = 1_700_000_000
        for path in all_inputs:
            os.utime(path, (base, base))
        depfile_time = base - 100 if depfile_is_older else base + 100
        os.utime(depfile, (depfile_time, depfile_time))
        return source, depfile

    def write_evidence(self, entries):
        self.depfiles.write_text(
            json.dumps(
                {
                    "producer": "cch-compiler-depfile",
                    "schema_version": 1,
                    "config_digest": "0" * 64,
                    "entries": [
                        {"source": str(source), "depfile": str(depfile), "digest": "0" * 64}
                        for source, depfile in entries
                    ],
                }
            )
        )

    def check(self):
        return freshness.check_freshness(
            report_path=self.report,
            depfiles_path=self.depfiles,
            binary_path=self.binary,
        )

    def test_fresh_tree_passes(self):
        header = self.root / "Header.hpp"
        header.write_text("#pragma once\n")
        source, depfile = self.write_depfile("Agent", [header])
        self.write_evidence([(source, depfile)])
        # Binary linked after the last compile.
        binary_mtime = depfile.stat().st_mtime + 100
        os.utime(self.binary, (binary_mtime, binary_mtime))
        self.assertEqual(self.check(), [])

    def test_missing_report_fails(self):
        self.report.unlink()
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        diagnostics = self.check()
        self.assertEqual(len(diagnostics), 1)
        self.assertIn("successful Gate evidence", diagnostics[0])

    def test_failed_gate_report_fails(self):
        self.report.write_text(json.dumps({"ok": False, "diagnostics": ["PARITY-6001"]}))
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        diagnostics = self.check()
        self.assertEqual(len(diagnostics), 1)
        self.assertIn("does not record success", diagnostics[0])

    def test_source_newer_than_depfile_fails(self):
        header = self.root / "Header.hpp"
        header.write_text("#pragma once\n")
        source, depfile = self.write_depfile("Agent", [header], depfile_is_older=True)
        self.write_evidence([(source, depfile)])
        diagnostics = self.check()
        self.assertTrue(any("stale build products" in d for d in diagnostics))
        self.assertTrue(any("Header.hpp" in d or "Agent.cpp" in d for d in diagnostics))

    def test_missing_prerequisite_fails(self):
        source, depfile = self.write_depfile("Agent", [])
        header = self.root / "Deleted.hpp"
        text = depfile.read_text()[:-1] + " \\\n " + str(header) + "\n"
        depfile.write_text(text)
        self.write_evidence([(source, depfile)])
        diagnostics = self.check()
        self.assertTrue(any("Deleted.hpp" in d for d in diagnostics))

    def test_missing_depfile_fails(self):
        source, depfile = self.write_depfile("Agent", [])
        depfile.unlink()
        self.write_evidence([(source, depfile)])
        diagnostics = self.check()
        self.assertTrue(any("missing depfile evidence" in d for d in diagnostics))

    def test_binary_older_than_a_depfile_fails(self):
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        os.utime(self.binary, (1_600_000_000, 1_600_000_000))
        diagnostics = self.check()
        self.assertTrue(any("stale Runtime binary" in d for d in diagnostics))

    def test_missing_binary_fails(self):
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        self.binary.unlink()
        diagnostics = self.check()
        self.assertTrue(any("missing Runtime binary" in d for d in diagnostics))


class FreshnessCliTest(unittest.TestCase):
    def test_missing_arguments_fail_closed(self):
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "cmake" / "install" / "check_install_freshness.py"),
                "--report",
                "/nonexistent/report.json",
                "--depfiles",
                "/nonexistent/depfiles.json",
                "--binary",
                "/nonexistent/cpp_harness",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
