#!/usr/bin/env python3
"""Unit tests for the Runtime-only install tooling (issue #472).

Covers the dependency-closure audit (ldd parsing, allowlist schema, closure
rules), the install freshness check (Gate evidence, depfile/Makefile parsing,
and rebuild-required staleness detection), and the Runtime link closure the
freshness check dates the binary against (issue #1044), with synthetic
fixtures.

Run directly: `python3 tests/install/install_tools_test.py`, or through the
CTest case `cch_install_tools_unit` registered in `cmake/tests/InstallTests.cmake`.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
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
            "--binary", "/nonexistent/pike", "--allowlist", str(ALLOWLIST_PATH)
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


class RuntimeLinkClosureTest(unittest.TestCase):
    """The closure comes from the build graph, so an object only some other
    target links can never date the Runtime binary (issue #1044)."""

    NINJA_GRAPH = """\
rule touch
  command = touch $out

build linked.o: touch
build unrelated.o: touch
build liblinked.a: touch linked.o
build libunrelated.a: touch unrelated.o
build pike: touch liblinked.a
build pike_without_objects: touch
"""

    def setUp(self):
        self.ninja = shutil.which("ninja")
        self.assertIsNotNone(
            self.ninja, "ninja is required: the supported build uses the Ninja generator"
        )
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.build_dir = Path(self.tmp.name)
        (self.build_dir / "build.ninja").write_text(self.NINJA_GRAPH)

    def closure(self, target):
        return freshness.runtime_link_closure(
            ninja=self.ninja, build_dir=str(self.build_dir), target=target
        )

    def test_closure_holds_only_the_targets_own_objects(self):
        self.assertEqual(self.closure("pike"), ["linked.o"])

    def test_unknown_target_fails_closed(self):
        with self.assertRaises(freshness.LinkClosureError):
            self.closure("no_such_target")

    def test_target_without_compiled_input_fails_closed(self):
        with self.assertRaises(freshness.LinkClosureError):
            self.closure("pike_without_objects")

    def test_missing_ninja_fails_closed(self):
        with self.assertRaises(freshness.LinkClosureError):
            freshness.runtime_link_closure(
                ninja=str(self.build_dir / "no-such-ninja"),
                build_dir=str(self.build_dir),
                target="pike",
            )


class CheckFreshnessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.binary = self.root / "pike"
        self.binary.write_bytes(b"elf")
        self.report = self.root / "parity-build-gate.json"
        self.report.write_text(json.dumps({"ok": True, "diagnostics": []}))
        self.depfiles = self.root / "parity-build-gate-depfiles.json"
        # Objects the Runtime is linked from; each test that needs staleness
        # adds the objects it wants dated against the binary.
        self.link_closure: list[str] = []

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
    def write_evidence_v2(self, entries, deps_log=None):
        doc = {
            "producer": "cch-compiler-depfile",
            "schema_version": 2,
            "config_digest": "0" * 64,
            "deps_log": str(deps_log) if deps_log else None,
            "deps_log_digest": "0" * 64 if deps_log else None,
            "entries": [
                {
                    "source": str(source),
                    "output": str(output),
                    "dependencies": [str(p) for p in prereqs],
                    "digest": "0" * 64,
                }
                for source, output, prereqs in entries
            ],
        }
        self.depfiles.write_text(json.dumps(doc))

    def check(self):
        return freshness.check_freshness(
            report_path=self.report,
            depfiles_path=self.depfiles,
            binary_path=self.binary,
            link_closure=self.link_closure,
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

    def test_binary_older_than_a_link_closure_object_fails(self):
        """Negative control: an object the Runtime is linked from dates it."""
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        linked_object = self.root / "Agent.cpp.o"
        linked_object.write_bytes(b"obj")
        base = 1_700_000_000
        os.utime(linked_object, (base, base))
        os.utime(self.binary, (base - 100, base - 100))
        self.link_closure = ["Agent.cpp.o"]
        diagnostics = self.check()
        self.assertTrue(any("stale Runtime binary" in d for d in diagnostics))
        self.assertTrue(any("Agent.cpp.o" in d for d in diagnostics))

    def test_object_outside_the_link_closure_does_not_date_the_binary(self):
        """Issue #1044: an Owner library the Runtime does not link cannot make
        a fresh Runtime look stale, however recently it was rebuilt."""
        linked_source, linked_depfile = self.write_depfile("Linked", [])
        unrelated_source, unrelated_depfile = self.write_depfile("Unrelated", [])
        self.write_evidence(
            [(linked_source, linked_depfile), (unrelated_source, unrelated_depfile)]
        )
        linked_object = self.root / "Linked.cpp.o"
        linked_object.write_bytes(b"obj")
        unrelated_object = self.root / "Unrelated.cpp.o"
        unrelated_object.write_bytes(b"obj")
        base = 1_700_000_000
        os.utime(linked_object, (base, base))
        # The unrelated object is rebuilt after the Runtime was linked.
        os.utime(unrelated_object, (base + 200, base + 200))
        os.utime(self.binary, (base + 100, base + 100))
        self.link_closure = ["Linked.cpp.o"]
        self.assertEqual(self.check(), [])

    def test_missing_link_closure_object_fails(self):
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        self.link_closure = ["NeverBuilt.cpp.o"]
        diagnostics = self.check()
        self.assertTrue(any("missing link-closure object" in d for d in diagnostics))

    def test_missing_binary_fails(self):
        source, depfile = self.write_depfile("Agent", [])
        self.write_evidence([(source, depfile)])
        self.binary.unlink()
        diagnostics = self.check()
        self.assertTrue(any("missing Runtime binary" in d for d in diagnostics))

    def test_schema_2_fresh_tree_passes(self):
        header = self.root / "Header.hpp"
        header.write_text("#pragma once\n")
        source = self.root / "Agent.cpp"
        source.write_text("// source\n")
        output = self.root / "Agent.o"
        output.write_bytes(b"obj")
        base = 1_700_000_000
        os.utime(source, (base, base))
        os.utime(header, (base, base))
        os.utime(output, (base + 100, base + 100))
        deps_log = self.root / ".ninja_deps"
        deps_log.write_bytes(b"# ninjadeps\n\x04\x00\x00\x00")
        os.utime(deps_log, (base + 100, base + 100))
        self.write_evidence_v2([(source, "Agent.o", [source, header])], deps_log=deps_log)
        binary_mtime = base + 200
        os.utime(self.binary, (binary_mtime, binary_mtime))
        self.link_closure = ["Agent.o"]
        self.assertEqual(self.check(), [])

    def test_schema_2_object_outside_the_link_closure_does_not_date_the_binary(self):
        """Issue #1044 for the active (.ninja_deps) evidence schema: the recorded
        entry set is whole-tree, so the closure -- not the entry set -- decides
        which objects date the Runtime binary."""
        base = 1_700_000_000
        deps_log = self.root / ".ninja_deps"
        deps_log.write_bytes(b"# ninjadeps\n\x04\x00\x00\x00")
        entries = []
        for name in ("Linked", "Unrelated"):
            source = self.root / f"{name}.cpp"
            source.write_text("// source\n")
            output = self.root / f"{name}.o"
            output.write_bytes(b"obj")
            os.utime(source, (base, base))
            object_mtime = base + 200 if name == "Unrelated" else base
            os.utime(output, (object_mtime, object_mtime))
            entries.append((source, f"{name}.o", [source]))
        self.write_evidence_v2(entries, deps_log=deps_log)
        os.utime(self.binary, (base + 100, base + 100))
        self.link_closure = ["Linked.o"]
        self.assertEqual(self.check(), [])

        # Control: the same stale object dates the binary once it is linked in.
        self.link_closure = ["Linked.o", "Unrelated.o"]
        diagnostics = self.check()
        self.assertTrue(any("stale Runtime binary" in d for d in diagnostics))
        self.assertTrue(any("Unrelated.o" in d for d in diagnostics))

    def test_schema_2_missing_object_fails(self):
        source = self.root / "Agent.cpp"
        source.write_text("// source\n")
        deps_log = self.root / ".ninja_deps"
        deps_log.write_bytes(b"# ninjadeps\n\x04\x00\x00\x00")
        self.write_evidence_v2([(source, "Agent.o", [source])], deps_log=deps_log)
        diagnostics = self.check()
        self.assertTrue(any("missing compiled object" in d for d in diagnostics))

    def test_schema_2_source_newer_than_object_fails(self):
        header = self.root / "Header.hpp"
        header.write_text("#pragma once\n")
        source = self.root / "Agent.cpp"
        source.write_text("// source\n")
        output = self.root / "Agent.o"
        output.write_bytes(b"obj")
        base = 1_700_000_000
        os.utime(source, (base + 200, base + 200))
        os.utime(output, (base + 100, base + 100))
        deps_log = self.root / ".ninja_deps"
        deps_log.write_bytes(b"# ninjadeps\n\x04\x00\x00\x00")
        self.write_evidence_v2([(source, "Agent.o", [source, header])], deps_log=deps_log)
        binary_mtime = base + 300
        os.utime(self.binary, (binary_mtime, binary_mtime))
        diagnostics = self.check()
        self.assertTrue(any("stale build products" in d for d in diagnostics))


class FreshnessCliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "cmake" / "install" / "check_install_freshness.py"),
                *args,
            ],
            capture_output=True,
            text=True,
        )

    def test_missing_arguments_fail_closed(self):
        result = self.run_cli(
            "--report",
            "/nonexistent/report.json",
            "--depfiles",
            "/nonexistent/depfiles.json",
            "--binary",
            "/nonexistent/pike",
        )
        self.assertNotEqual(result.returncode, 0)

    def test_unreadable_link_closure_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            result = self.run_cli(
                "--report",
                os.path.join(root, "report.json"),
                "--depfiles",
                os.path.join(root, "depfiles.json"),
                "--binary",
                os.path.join(root, "pike"),
                "--ninja",
                os.path.join(root, "no-such-ninja"),
                "--target",
                "pike",
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("link closure", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
