#!/usr/bin/env python3
"""Generate the QSG atlas warm-set header from the declarative table."""

from __future__ import annotations

import argparse
import pathlib
import sys
import tomllib


def codepoint(text: str) -> int:
    return int(text, 16)


def escaped_scalar(value: int) -> str:
    if value <= 0xFFFF:
        return f"\\u{value:04x}"
    return f"\\U{value:08x}"


def escaped_token(value: int) -> str:
    if value == 0x22:
        return '\\"'
    if value == 0x5C:
        return "\\\\"
    if 0x20 <= value <= 0x7E:
        return chr(value)
    return escaped_scalar(value)


def escaped_tokens(values: list[int]) -> list[str]:
    return [escaped_token(value) for value in values]


def codepoint_tokens(values: list[str]) -> list[str]:
    return escaped_tokens([codepoint(value) for value in values])


def family_values(family: dict) -> list[int]:
    values: list[int] = []
    for first, last in family.get("ranges", []):
        values.extend(range(codepoint(first), codepoint(last) + 1))
    for scalar in family.get("scalars", []):
        values.append(codepoint(scalar))
    return values


def append_space_separated_tokens(target: list[str], tokens: list[str]) -> None:
    if not tokens:
        return
    if target:
        target.append(" ")
    target.extend(tokens)


def family_token_stream(family: dict) -> list[str]:
    tokens: list[str] = []
    append_space_separated_tokens(tokens, escaped_tokens(family_values(family)))
    for cluster in family.get("clusters_utf16", []):
        append_space_separated_tokens(tokens, codepoint_tokens(cluster))
    return tokens


def wrapped_u16_literal(
    tokens: list[str],
    indent: str = "            ",
    max_payload: int = 64,
) -> list[str]:
    chunks: list[str] = []
    current = ""
    for token in tokens:
        if current and len(current) + len(token) > max_payload:
            chunks.append(current)
            current = token
            continue
        current += token
    if current:
        chunks.append(current)
    return [f'{indent}u"{chunk}"' for chunk in chunks]


def self_test_token_wrapping() -> None:
    tokens = ["\\u1234", "\\U0001f600", "A", "\\\\", '\\"']
    wrapped = wrapped_u16_literal(tokens, "", 9)
    text = "\n".join(wrapped)
    for token in tokens:
        if token not in text:
            raise AssertionError(f"wrapped output lost token {token!r}")
    for line in wrapped:
        payload = line.split('"', 1)[1].rsplit('"', 1)[0]
        if payload.endswith("\\u") or payload.endswith("\\U"):
            raise AssertionError(f"wrapped output split an escape in {line!r}")


def check_generated_header(source: pathlib.Path, expected: pathlib.Path) -> int:
    generated = generate(source)
    expected_text = expected.read_text(encoding="utf-8")
    if generated == expected_text:
        return 0
    print(
        f"{expected} is not the exact output of {source}",
        file=sys.stderr,
    )
    return 1


def generate(source: pathlib.Path) -> str:
    data = tomllib.loads(source.read_text(encoding="utf-8"))
    families = data["family"]
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <string_view>",
        "",
        "namespace vnm_terminal::internal {",
        "",
        "struct qsg_atlas_warm_seed_string_t",
        "{",
        "    std::u16string_view text;",
        "    std::string_view    family;",
        "};",
        "",
        f"inline constexpr std::size_t k_qsg_atlas_warm_seed_string_budget = {data['string_budget']}U;",
        f"inline constexpr std::size_t k_qsg_atlas_warm_seed_code_unit_budget = {data['code_unit_budget']}U;",
        f"inline constexpr std::size_t k_qsg_atlas_warm_seed_shaped_record_budget = {data['shaped_record_budget']}U;",
        "",
        f"inline constexpr std::array<qsg_atlas_warm_seed_string_t, {len(families)}>",
        "    k_qsg_atlas_warm_seed_strings = {{",
    ]
    for family in families:
        lines.append("        {")
        literal_lines = wrapped_u16_literal(family_token_stream(family))
        for index, literal in enumerate(literal_lines):
            suffix = "" if index + 1 < len(literal_lines) else ","
            lines.append(f"{literal}{suffix}")
        lines.append(f'            "{family["name"]}",')
        lines.append("        },")
    lines.extend([
        "    }};",
        "",
        "} // namespace vnm_terminal::internal",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("source", type=pathlib.Path, nargs="?")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    args = parser.parse_args()
    if args.self_test:
        self_test_token_wrapping()
        return 0
    if args.source is None or args.output is None:
        parser.error("source and output are required")
    if args.check:
        return check_generated_header(args.source, args.output)
    args.output.write_text(generate(args.source), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
