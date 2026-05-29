#!/usr/bin/env python3
"""
Static review guard for primary backing Phase 0B.

The tool is wired as a CTest static check and is not part of production
runtime. It gives reviewers a repeatable predicate for the no
recovery-as-storage-evidence foundation rule. Diff mode can also check added
lines for common current storage member mutation shapes during local review.
Tree scans intentionally check vocabulary only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCAN_ROOTS = (
    "include",
    "src",
    "tests",
    "tools",
)

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".cxx",
    ".h",
    ".hpp",
    ".py",
}

PHASE_0B_GUARD_PATH = Path("tools/primary_backing_phase_0b_guard.py")
TRANSCRIPT_TESTS_PATH = Path("tests/transcript/transcript_tests.cpp")

RECOVERY_ALLOWED_PATHS = {
    PHASE_0B_GUARD_PATH,
}

RECOVERY_ALLOWED_LINES_BY_PATH = {
    Path("include/vnm_terminal/internal/session_contract.h"): {
        "bool                            recover_scrollback_from_primary_repaints = false;",
    },
    Path("include/vnm_terminal/internal/terminal_screen_model.h"): {
        "bool                   recover_scrollback_from_primary_repaints = false;",
        "struct primary_repaint_recovery_candidate_t",
        "void arm_resize_repaint_clear_guard();",
        "void cancel_resize_repaint_clear_guard();",
        "void cancel_resize_repaint_clear_guard_before_visible_clear();",
        "void advance_resize_repaint_clear_guard();",
        "void begin_primary_repaint_recovery_candidate();",
        "void finish_primary_repaint_recovery_candidate(bool discard_if_no_match);",
        "void cancel_primary_repaint_recovery_candidate();",
        "const primary_repaint_recovery_candidate_t&    candidate) const;",
        "int                             m_resize_repaint_clear_guard_remaining = 0;",
        "bool                            m_resize_repaint_clear_guard_saw_visible_clear = false;",
        "primary_repaint_recovery_candidate_t",
        "m_primary_repaint_recovery_candidate;",
    },
    Path("src/terminal_screen_model.cpp"): {
        "constexpr int k_resize_repaint_clear_guard_action_budget = 64;",
        "advance_resize_repaint_clear_guard();",
        "if (m_config.recover_scrollback_from_primary_repaints == enabled) {",
        "m_config.recover_scrollback_from_primary_repaints = enabled;",
        "if (m_primary_repaint_recovery_candidate.active && m_modes.cursor_visible) {",
        "finish_primary_repaint_recovery_candidate(false);",
        "begin_primary_repaint_recovery_candidate();",
        "cancel_primary_repaint_recovery_candidate();",
        "arm_resize_repaint_clear_guard();",
        "cancel_resize_repaint_clear_guard();",
        "cancel_resize_repaint_clear_guard_before_visible_clear();",
        "m_primary_repaint_recovery_candidate.active;",
        "if (m_primary_repaint_recovery_candidate.active &&",
        "m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous = false;",
        "m_primary_repaint_recovery_candidate.rows[static_cast<std::size_t>(m_cursor.row)];",
        "m_primary_repaint_recovery_candidate.line_start_clear_before_text =",
        "m_primary_repaint_recovery_candidate.line_start_clear_before_text ||",
        "m_primary_repaint_recovery_candidate.explicit_non_home_repaint_address =",
        "m_primary_repaint_recovery_candidate.explicit_non_home_repaint_address ||",
        "(m_primary_repaint_recovery_candidate.pending_non_home_addressed_row ==",
        "m_primary_repaint_recovery_candidate.pending_non_home_addressed_row = -1;",
        "if (m_primary_repaint_recovery_candidate.active) {",
        "m_primary_repaint_recovery_candidate.pending_non_home_addressed_row =",
        "void Terminal_screen_model::arm_resize_repaint_clear_guard()",
        "if (!m_config.recover_scrollback_from_primary_repaints ||",
        "m_resize_repaint_clear_guard_remaining         =",
        "k_resize_repaint_clear_guard_action_budget;",
        "m_resize_repaint_clear_guard_saw_visible_clear = false;",
        "void Terminal_screen_model::cancel_resize_repaint_clear_guard()",
        "m_resize_repaint_clear_guard_remaining         = 0;",
        "void Terminal_screen_model::cancel_resize_repaint_clear_guard_before_visible_clear()",
        "if (m_resize_repaint_clear_guard_saw_visible_clear) {",
        "void Terminal_screen_model::advance_resize_repaint_clear_guard()",
        "if (m_resize_repaint_clear_guard_remaining <= 0) {",
        "--m_resize_repaint_clear_guard_remaining;",
        "if (m_resize_repaint_clear_guard_remaining <= 0 ||",
        "m_resize_repaint_clear_guard_saw_visible_clear = true;",
        "!m_resize_repaint_clear_guard_saw_visible_clear)",
        "void Terminal_screen_model::begin_primary_repaint_recovery_candidate()",
        "finish_primary_repaint_recovery_candidate(true);",
        "m_primary_repaint_recovery_candidate.rows                         = active_grid_rows();",
        "m_primary_repaint_recovery_candidate.scrollback_rows              = scrollback_size();",
        "m_primary_repaint_recovery_candidate.unmatched_finish_budget      = 1;",
        "m_primary_repaint_recovery_candidate.pending_non_home_addressed_row = -1;",
        "m_primary_repaint_recovery_candidate.line_start_clear_before_text = false;",
        "m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous =",
        "m_primary_repaint_recovery_candidate.active                       = true;",
        "void Terminal_screen_model::finish_primary_repaint_recovery_candidate(",
        "if (!m_primary_repaint_recovery_candidate.active) {",
        "primary_repaint_recovery_candidate_t candidate =",
        "std::move(m_primary_repaint_recovery_candidate);",
        "m_primary_repaint_recovery_candidate = {};",
        "m_primary_repaint_recovery_candidate = std::move(candidate);",
        "void Terminal_screen_model::cancel_primary_repaint_recovery_candidate()",
        "m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous)",
        "const primary_repaint_recovery_candidate_t& candidate) const",
        "m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous = true;",
    },
    Path("src/terminal_session.cpp"): {
        "if (m_config.recover_scrollback_from_primary_repaints == enabled) {",
        "m_config.recover_scrollback_from_primary_repaints = enabled;",
        "screen_config.recover_scrollback_from_primary_repaints =",
        "m_config.recover_scrollback_from_primary_repaints;",
    },
    Path("src/terminal_transcript.cpp"): {
        "{QStringLiteral(\"recover_scrollback_from_primary_repaints\"),",
        "config.recover_scrollback_from_primary_repaints},",
        "(!config.contains(QStringLiteral(\"recover_scrollback_from_primary_repaints\")) ||",
        "QStringLiteral(\"recover_scrollback_from_primary_repaints\"),",
    },
    Path("src/vnm_terminal_surface.cpp"): {
        "session_config.recover_scrollback_from_primary_repaints =",
    },
    Path("tools/transcript_replay/terminal_transcript_replay.cpp"): {
        "config.recover_scrollback_from_primary_repaints =",
        "session_config.value(QStringLiteral(\"recover_scrollback_from_primary_repaints\")).toBool();",
        "config.recover_scrollback_from_primary_repaints = false;",
    },
    Path("tests/helpers/primary_backing_test_config.h"): {
        "config.recover_scrollback_from_primary_repaints = false;",
    },
    Path("tests/conformance/capture_replay_conformance_tests.cpp"): {
        "config.recover_scrollback_from_primary_repaints = false;",
    },
    Path("tests/conformance/parser_libfuzzer_harness.cpp"): {
        "config.recover_scrollback_from_primary_repaints = false;",
    },
    Path("tests/windows_conpty_backend/windows_conpty_backend_tests.cpp"): {
        "recovery_model_config.recover_scrollback_from_primary_repaints = true;",
    },
    Path("tests/screen_operations/model_ops_tests.cpp"): {
        "config.recover_scrollback_from_primary_repaints = true;",
    },
    TRANSCRIPT_TESTS_PATH: {
        "config.recover_scrollback_from_primary_repaints =",
        "session_config.value(QStringLiteral(\"recover_scrollback_from_primary_repaints\")).toBool();",
        "session_config.recover_scrollback_from_primary_repaints = recovery_enabled;",
        "QStringLiteral(\"recover_scrollback_from_primary_repaints\")) &&",
        "QStringLiteral(\"recover_scrollback_from_primary_repaints\")).toBool() ==",
        "!config.recover_scrollback_from_primary_repaints,",
        "QStringLiteral(\"recover_scrollback_from_primary_repaints\"),",
        "}).recover_scrollback_from_primary_repaints;",
        "config.recover_scrollback_from_primary_repaints = false;",
    },
}

RECOVERY_ALLOWED_LINE_COUNT_OVERRIDES_BY_PATH = {
    Path("src/terminal_screen_model.cpp"): {
        "advance_resize_repaint_clear_guard();": 2,
        "cancel_primary_repaint_recovery_candidate();": 8,
        "cancel_resize_repaint_clear_guard();": 6,
        "cancel_resize_repaint_clear_guard_before_visible_clear();": 3,
        "finish_primary_repaint_recovery_candidate(false);": 2,
        "if (!m_config.recover_scrollback_from_primary_repaints ||": 3,
        "if (m_primary_repaint_recovery_candidate.active &&": 4,
        "if (m_primary_repaint_recovery_candidate.active) {": 2,
        "if (m_resize_repaint_clear_guard_remaining <= 0 ||": 2,
        "if (m_resize_repaint_clear_guard_remaining <= 0) {": 2,
        "k_resize_repaint_clear_guard_action_budget;": 2,
        "const primary_repaint_recovery_candidate_t& candidate) const": 2,
        "m_primary_repaint_recovery_candidate = {};": 2,
        "m_primary_repaint_recovery_candidate.explicit_non_home_repaint_address =": 2,
        "m_primary_repaint_recovery_candidate.pending_non_home_addressed_row = -1;": 2,
        "m_resize_repaint_clear_guard_saw_visible_clear = false;": 2,
    },
    Path("include/vnm_terminal/internal/terminal_screen_model.h"): {
        "const primary_repaint_recovery_candidate_t&    candidate) const;": 2,
    },
    TRANSCRIPT_TESTS_PATH: {
        "!config.recover_scrollback_from_primary_repaints,": 2,
        "config.recover_scrollback_from_primary_repaints = false;": 2,
    },
    Path("tests/helpers/primary_backing_test_config.h"): {
        "config.recover_scrollback_from_primary_repaints = false;": 2,
    },
}

SOURCE_SWITCH_ALLOWED_PATHS = {
    PHASE_0B_GUARD_PATH,
}

SOURCE_SWITCH_ALLOWED_LINE_RE = {
    TRANSCRIPT_TESTS_PATH: (
        re.compile(r"legacy[_ -]reason", re.IGNORECASE),
    ),
}

RECOVERY_TERMS = (
    "recover_scrollback_from_primary_repaints",
    "primary_repaint_recovery_candidate",
    "inferred_primary_repaint_scroll_rows",
    "resize_repaint_clear_guard",
    "repaint inference",
    "repaint-inference",
    "repaint_inference",
    "repaint recovery",
    "recovery candidate",
    "text matching",
    "text-matching",
    "text_matching",
)

SOURCE_SWITCH_TERMS = (
    "_legacy",
    "_v2",
    "dual-written",
    "dual_written",
    "fallback row producer",
    "fallback-row-producer",
    "fallback_row_producer",
    "fallback source",
    "fallback-source",
    "fallback_source",
    "mirror storage",
    "mirror-storage",
    "mirror_storage",
    "source switch",
    "source-switch",
    "source_switch",
)

CURRENT_STORAGE_MEMBER_RE = r"\b(?:m_scrollback|m_rows|m_primary_buffer|m_alternate_buffer)\b"
ASSIGNMENT_OPERATOR_RE = r"(?:<<=|>>=|[+\-*/%&|^]?=(?!=))"
CURRENT_STORAGE_MUTATION_RE = re.compile(
    rf"{CURRENT_STORAGE_MEMBER_RE}(?:"
    rf"\s*{ASSIGNMENT_OPERATOR_RE}"
    rf"|(?:\s*\[[^\]\n]*\])+\s*{ASSIGNMENT_OPERATOR_RE}"
    r"|\s*\.(?:assign|clear|emplace|emplace_back|emplace_front|erase|insert|"
    r"pop_back|pop_front|push_back|push_front|resize|swap)\s*\("
    r")")

HUNK_RE = re.compile(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")


@dataclass
class Guard_violation:
    path: Path
    line_number: int
    rule: str
    detail: str

    def format(self) -> str:
        return f"{self.path.as_posix()}:{self.line_number}: {self.rule}: {self.detail}"


def normalized_relative_path(root: Path, path: Path) -> Path:
    try:
        return path.resolve().relative_to(root.resolve())
    except ValueError:
        return path


def is_scannable_file(path: Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix in SOURCE_SUFFIXES


def contains_any(text: str, terms: Iterable[str]) -> str | None:
    lower_text = text.lower()
    for term in terms:
        if term in lower_text:
            return term
    return None


def recovery_violation_for_line(rel_path: Path, line: str) -> str | None:
    term = contains_any(line, RECOVERY_TERMS)
    if term is None:
        return None
    if rel_path in RECOVERY_ALLOWED_PATHS:
        return None
    if line.strip() in RECOVERY_ALLOWED_LINES_BY_PATH.get(rel_path, set()):
        return None
    return f"unowned recovery-as-storage-evidence vocabulary '{term}' needs an owning phase"


def expected_allowed_recovery_line_count(rel_path: Path, line: str) -> int | None:
    if line not in RECOVERY_ALLOWED_LINES_BY_PATH.get(rel_path, set()):
        return None
    return RECOVERY_ALLOWED_LINE_COUNT_OVERRIDES_BY_PATH.get(rel_path, {}).get(line, 1)


def source_switch_violation_for_line(rel_path: Path, line: str) -> str | None:
    term = contains_any(line, SOURCE_SWITCH_TERMS)
    if term is None:
        return None
    if rel_path in SOURCE_SWITCH_ALLOWED_PATHS:
        return None
    for allowed_line_re in SOURCE_SWITCH_ALLOWED_LINE_RE.get(rel_path, ()):
        if allowed_line_re.search(line):
            return None
    return f"source-switch, mirror-storage, or fallback-row-producer vocabulary '{term}' needs a deletion gate"


def storage_mutation_violation_for_line(rel_path: Path, line: str) -> str | None:
    if rel_path == PHASE_0B_GUARD_PATH:
        return None
    if CURRENT_STORAGE_MUTATION_RE.search(line) is None:
        return None
    return "current storage member mutation needs an extraction-phase owner"


def scan_tree(root: Path) -> list[Guard_violation]:
    violations: list[Guard_violation] = []
    allowed_recovery_line_counts: dict[tuple[Path, str], int] = {}
    for scan_root in SCAN_ROOTS:
        base = root / scan_root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or not is_scannable_file(path):
                continue
            rel_path = normalized_relative_path(root, path)
            text = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(text.splitlines(), start=1):
                stripped_line = line.strip()
                if (
                    contains_any(line, RECOVERY_TERMS) is not None and
                    expected_allowed_recovery_line_count(rel_path, stripped_line) is not None
                ):
                    key = (rel_path, stripped_line)
                    allowed_recovery_line_counts[key] = (
                        allowed_recovery_line_counts.get(key, 0) + 1
                    )
                recovery_detail = recovery_violation_for_line(rel_path, line)
                if recovery_detail is not None:
                    violations.append(Guard_violation(
                        rel_path, line_number, "primary-backing-recovery-evidence", recovery_detail))

                source_switch_detail = source_switch_violation_for_line(rel_path, line)
                if source_switch_detail is not None:
                    violations.append(Guard_violation(
                        rel_path, line_number, "primary-backing-source-switch", source_switch_detail))
    for rel_path, allowed_lines in RECOVERY_ALLOWED_LINES_BY_PATH.items():
        if rel_path in RECOVERY_ALLOWED_PATHS:
            continue
        if not (root / rel_path).exists():
            continue
        for allowed_line in allowed_lines:
            expected_count = expected_allowed_recovery_line_count(rel_path, allowed_line)
            if expected_count is None:
                continue
            actual_count = allowed_recovery_line_counts.get((rel_path, allowed_line), 0)
            if actual_count != expected_count:
                violations.append(Guard_violation(
                    rel_path,
                    1,
                    "recovery-allowlist-count",
                    "allowed recovery baseline line count changed "
                    f"(expected {expected_count}, found {actual_count}): {allowed_line}",
                ))
    return violations


def path_from_diff_header(line: str) -> Path | None:
    if line == "+++ /dev/null":
        return None
    if not line.startswith("+++ "):
        return None

    value = line[4:].strip()
    if value.startswith("b/") or value.startswith("a/"):
        value = value[2:]
    return Path(value)


def scan_diff_text(diff_text: str) -> list[Guard_violation]:
    violations: list[Guard_violation] = []
    rel_path: Path | None = None
    target_line = 0

    for raw_line in diff_text.splitlines():
        if raw_line.startswith("+++ "):
            rel_path = path_from_diff_header(raw_line)
            target_line = 0
            continue

        hunk_match = HUNK_RE.match(raw_line)
        if hunk_match is not None:
            target_line = int(hunk_match.group(1))
            continue

        if rel_path is None:
            continue

        if raw_line.startswith("+") and not raw_line.startswith("+++"):
            added_line = raw_line[1:]

            recovery_detail = recovery_violation_for_line(rel_path, added_line)
            if recovery_detail is not None:
                violations.append(Guard_violation(
                    rel_path, target_line, "primary-backing-recovery-evidence", recovery_detail))

            source_switch_detail = source_switch_violation_for_line(rel_path, added_line)
            if source_switch_detail is not None:
                violations.append(Guard_violation(
                    rel_path, target_line, "primary-backing-source-switch", source_switch_detail))

            storage_detail = storage_mutation_violation_for_line(rel_path, added_line)
            if storage_detail is not None:
                violations.append(Guard_violation(
                    rel_path, target_line, "primary-backing-storage-mutation", storage_detail))

            target_line += 1
        elif raw_line.startswith("-") and not raw_line.startswith("---"):
            continue
        else:
            target_line += 1

    return violations


def read_git_diff(repo_root: Path) -> str:
    result = subprocess.run(
        [
            "git",
            "diff",
            "--no-ext-diff",
            "--",
            *SCAN_ROOTS,
        ],
        cwd=repo_root,
        check=False,
        capture_output=True,
        text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git diff failed")
    return result.stdout


def read_diff(repo_root: Path, path: str) -> str:
    if path == "":
        return read_git_diff(repo_root)
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8", errors="replace")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Check Phase 0B primary backing review guards. Without --diff, "
            "tree scan checks recovery-as-storage-evidence vocabulary only; "
            "--diff also checks added lines for storage mutations."))
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Repository root. Defaults to the parent of the tools directory.")
    parser.add_argument(
        "--diff",
        nargs="?",
        const="",
        help=(
            "Unified diff path, or '-' to read stdin. Diff mode checks added "
            "lines from the current git diff when no path is supplied. It also checks "
            "added lines for vocabulary and storage mutations; without this, tree "
            "scan checks vocabulary only."))
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.diff is not None:
        violations = scan_diff_text(read_diff(args.repo_root, args.diff))
    else:
        violations = scan_tree(args.repo_root)

    if not violations:
        return 0

    print("Primary backing Phase 0B guard violations:", file=sys.stderr)
    for violation in violations:
        print(f"  {violation.format()}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
