"""Extract the pinned UE 5.6.1 GPU readback class definitions for MiniMap."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


EXPECTED_HEADER_SHA256 = "27d0c4bcde6374a2d77707610fa7a25384783ac376c17ba320a63b00a57b43ef"
EXPECTED_VERSION = (5, 6, 1, 44394996, 43139311, "++UE5+Release-5.6")
REQUESTED_CLASSES = ("FRHIGPUMemoryReadback", "FRHIGPUTextureReadback")


def mask_comments_and_literals(source: str) -> str:
    """Replace comments and literals with spaces so braces remain structurally visible."""
    result = list(source)
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index)
            end = len(source) if end == -1 else end
            for position in range(index, end):
                result[position] = " "
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end == -1:
                raise ValueError("Unterminated block comment in RHIGPUReadback.h")
            end += 2
            for position in range(index, end):
                if source[position] != "\n":
                    result[position] = " "
            index = end
            continue

        raw_prefix = re.match(r"(?:u8|u|U|L)?R\"([^ ()\\\t\r\n]{0,16})\(", source[index:])
        if raw_prefix:
            terminator = ")" + raw_prefix.group(1) + '"'
            end = source.find(terminator, index + raw_prefix.end())
            if end == -1:
                raise ValueError("Unterminated raw string in RHIGPUReadback.h")
            end += len(terminator)
            for position in range(index, end):
                if source[position] != "\n":
                    result[position] = " "
            index = end
            continue

        if source[index] in ('"', "'"):
            quote = source[index]
            end = index + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            else:
                raise ValueError("Unterminated string or character literal in RHIGPUReadback.h")
            for position in range(index, min(end, len(source))):
                if source[position] != "\n":
                    result[position] = " "
            index = end
            continue
        index += 1
    return "".join(result)


def extract_class(source: str, masked: str, class_name: str) -> str:
    """Return one complete class definition with its original source formatting."""
    matches = list(re.finditer(r"\bclass\s+" + re.escape(class_name) + r"\b", masked))
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one {class_name} declaration, found {len(matches)}")

    opening = masked.find("{", matches[0].end())
    semicolon = masked.find(";", matches[0].end())
    if opening == -1 or (semicolon != -1 and semicolon < opening):
        raise ValueError(f"{class_name} is not a complete class definition")

    depth = 0
    closing = -1
    for position in range(opening, len(masked)):
        if masked[position] == "{":
            depth += 1
        elif masked[position] == "}":
            depth -= 1
            if depth == 0:
                closing = position
                break
    if closing == -1:
        raise ValueError(f"Unbalanced class body for {class_name}")

    declaration_end = closing + 1
    while declaration_end < len(masked) and masked[declaration_end].isspace():
        declaration_end += 1
    if declaration_end >= len(masked) or masked[declaration_end] != ";":
        raise ValueError(f"Could not locate the terminating semicolon for {class_name}")
    return source[matches[0].start():declaration_end + 1]


def validate_engine_version(unreal_root: Path) -> None:
    """Reject any source root that is not the pinned UE 5.6.1 release."""
    version_path = unreal_root / "Engine" / "Build" / "Build.version"
    version = json.loads(version_path.read_text(encoding="utf-8-sig"))
    actual = (
        version.get("MajorVersion"),
        version.get("MinorVersion"),
        version.get("PatchVersion"),
        version.get("Changelist"),
        version.get("CompatibleChangelist"),
        version.get("BranchName"),
    )
    if actual != EXPECTED_VERSION:
        raise ValueError(f"Unreal Build.version does not match pinned UE 5.6.1: {actual!r}")


def generate(unreal_root: Path, output_path: Path) -> None:
    """Validate and extract the two exact UE definitions to the ephemeral output."""
    validate_engine_version(unreal_root)
    source_path = unreal_root / "Engine" / "Source" / "Runtime" / "RHI" / "Public" / "RHIGPUReadback.h"
    source_bytes = source_path.read_bytes()
    digest = hashlib.sha256(source_bytes).hexdigest()
    if digest != EXPECTED_HEADER_SHA256:
        raise ValueError("RHIGPUReadback.h does not match the verified UE 5.6.1 source definition")

    source = source_bytes.decode("utf-8-sig")
    masked = mask_comments_and_literals(source)
    definitions = [extract_class(source, masked, name) for name in REQUESTED_CLASSES]
    generated = "\n".join(
        (
            "// Generated from licensed Unreal Engine 5.6.1 source. Do not commit or distribute.",
            "#pragma once",
            "#include <RHIResources.h>",
            "",
            "// Genuine declaration used by Epic's inline base constructor. NativeApi owns construction.",
            "RHI_API FGPUFenceRHIRef RHICreateGPUFence(const FName& Name);",
            "",
            *definitions,
            "",
            "static_assert(sizeof(FRHIGPUMemoryReadback) == 0x18);",
            "static_assert(alignof(FRHIGPUMemoryReadback) == 8);",
            "static_assert(sizeof(FRHIGPUTextureReadback) == 0x58);",
            "static_assert(alignof(FRHIGPUTextureReadback) == 8);",
            "",
        )
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(generated, encoding="utf-8", newline="\n")
    print("Generated pinned UE 5.6.1 readback compatibility header (2 classes).")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unreal-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        generate(arguments.unreal_root.resolve(), arguments.output.resolve())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Readback compatibility generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
