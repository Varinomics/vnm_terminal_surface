#!/usr/bin/env python3

import argparse
import hashlib
import os
from pathlib import Path
import tempfile

import fontTools
from fontTools.ttLib import TTFont


EXPECTED_FONTTOOLS_VERSION = "4.56.0"
EXPECTED_INPUT_SHA256 = "09ea86e719381a9b1aecf7590338b625edd00e9ad74ec2484b94bd35a029010d"
EXPECTED_OUTPUT_SHA256 = "2448ba77c08b20151cdfd11da8bc72abc8b84fc46e85785c9a40f1a2a8c0655f"

EXPECTED_NAMES = {
    1: "Ubuntu Mono derivative Bront",
    3: "Ubuntu Mono derivative Bront Regular 0.1",
    4: "Ubuntu Mono derivative Bront",
    6: "UbuntuMonoDerivativeBront-Regular",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def raw_tables(path: Path) -> dict[str, bytes]:
    with path.open("rb") as stream:
        font = TTFont(stream, lazy=True)
        tables = {}
        for tag, entry in font.reader.tables.items():
            stream.seek(entry.offset)
            tables[tag] = stream.read(entry.length)
        font.close()
    return tables


def normalized_head(data: bytes) -> bytes:
    if len(data) < 12:
        raise RuntimeError("The head table is too short to contain checkSumAdjustment.")
    return data[:8] + bytes(4) + data[12:]


def name_records(path: Path) -> dict[tuple[int, int, int, int], str]:
    font = TTFont(path, lazy=True)
    records = {
        (record.platformID, record.platEncID, record.langID, record.nameID): record.toUnicode()
        for record in font["name"].names
    }
    font.close()
    return records


def verify_only_name_metadata_changed(input_path: Path, output_path: Path) -> None:
    input_tables = raw_tables(input_path)
    output_tables = raw_tables(output_path)
    if input_tables.keys() != output_tables.keys():
        raise RuntimeError("The transformation changed the font table set.")

    for tag, input_data in input_tables.items():
        output_data = output_tables[tag]
        if tag == "name":
            continue
        if tag == "head":
            if normalized_head(input_data) != normalized_head(output_data):
                raise RuntimeError("The transformation changed head data beyond checkSumAdjustment.")
            continue
        if input_data != output_data:
            raise RuntimeError(f"The transformation changed the {tag} table.")

    input_names = name_records(input_path)
    output_names = name_records(output_path)
    if input_names.keys() != output_names.keys():
        raise RuntimeError("The transformation changed the name-record key set.")
    for key, input_value in input_names.items():
        name_id = key[3]
        output_value = output_names[key]
        if name_id in EXPECTED_NAMES:
            if output_value != EXPECTED_NAMES[name_id]:
                raise RuntimeError(f"The transformed name ID {name_id} value is incorrect.")
        elif output_value != input_value:
            raise RuntimeError(f"The transformation changed undeclared name ID {name_id} metadata.")


def replace_names(input_path: Path, output_path: Path) -> None:
    if input_path == output_path:
        raise RuntimeError("Input and output paths must be different.")
    if fontTools.__version__ != EXPECTED_FONTTOOLS_VERSION:
        raise RuntimeError(
            f"fontTools {EXPECTED_FONTTOOLS_VERSION} is required; found {fontTools.__version__}."
        )
    if sha256(input_path) != EXPECTED_INPUT_SHA256:
        raise RuntimeError("The input font does not match the pinned upstream Bront artifact.")

    font = TTFont(input_path, recalcBBoxes=False, recalcTimestamp=False)
    name_table = font["name"]
    observed_name_ids = {record.nameID for record in name_table.names}
    for name_id in EXPECTED_NAMES:
        if name_id not in observed_name_ids:
            raise RuntimeError(f"The input font has no name ID {name_id} record.")

    for record in name_table.names:
        replacement = EXPECTED_NAMES.get(record.nameID)
        if replacement is not None:
            record.string = replacement.encode(record.getEncoding())

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=output_path.parent,
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
        font.save(temporary_path, reorderTables=False)
        font.close()

        verify_only_name_metadata_changed(input_path, temporary_path)
        output_hash = sha256(temporary_path)
        if EXPECTED_OUTPUT_SHA256 is not None and output_hash != EXPECTED_OUTPUT_SHA256:
            raise RuntimeError(
                f"Output SHA-256 mismatch: expected {EXPECTED_OUTPUT_SHA256}, found {output_hash}."
            )
        os.replace(temporary_path, output_path)
        temporary_path = None
        print(output_hash)
    finally:
        font.close()
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply the UFL-compliant Bront name-table transformation."
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def main() -> None:
    arguments = parse_arguments()
    replace_names(arguments.input.resolve(), arguments.output.resolve())


if __name__ == "__main__":
    main()
