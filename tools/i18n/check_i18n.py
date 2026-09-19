#!/usr/bin/env python3
"""Check that the M1 X-macro catalog is complete and UTF-8 readable."""

from pathlib import Path
import re
import sys


CATALOG = Path(__file__).with_name("lyra_i18n_catalog.inc")
SOURCE_ROOT = CATALOG.parents[2] / "main"
GUI_ROOT = SOURCE_ROOT / "gui"

# These values are intentionally not translation strings.  They are either
# live-data placeholders, a symbol, or a fixed product/hardware identifier.
# Keeping this list here makes a new direct make_label() literal fail the
# check instead of silently bypassing the catalog.
RAW_LABEL_ALLOWLIST = {
    "00:00",
    "--:--",
    "1",
    "12:30",
    "100%  ",
    "+6dB",
    "-6dB",
    "",
    "Emotivate Lyra",
    "JC3248W535EN",
    "1.0.1",
    "i",
}


def raw_label_literals() -> list[tuple[Path, str]]:
    """Return direct make_label string literals that are not approved data."""
    findings: list[tuple[Path, str]] = []

    def call_literals(text: str, opening: int) -> list[str]:
        literals: list[str] = []
        depth = 1
        index = opening + 1
        while index < len(text) and depth:
            char = text[index]
            if char == '"':
                index += 1
                value: list[str] = []
                while index < len(text):
                    char = text[index]
                    if char == '\\' and index + 1 < len(text):
                        value.append(text[index:index + 2])
                        index += 2
                    elif char == '"':
                        literals.append(''.join(value))
                        index += 1
                        break
                    else:
                        value.append(char)
                        index += 1
                continue
            if char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
            index += 1
        return literals

    for source in GUI_ROOT.rglob("*"):
        if source.suffix not in {".cpp", ".h"}:
            continue
        text = source.read_text(encoding="utf-8")
        for match in re.finditer(r"make_label\s*\(", text):
            for literal in call_literals(text, match.end() - 1):
                if literal not in RAW_LABEL_ALLOWLIST:
                    findings.append((source, literal))
    return findings


def main() -> int:
    text = CATALOG.read_text(encoding="utf-8")
    entries = re.findall(r"^LYRA_I18N_(?:FALLBACK|COUNT|COUNT_RU|ENTRY)\((\w+),", text, re.MULTILINE)
    if not entries:
        print("i18n: catalog contains no entries", file=sys.stderr)
        return 1
    if len(entries) != len(set(entries)):
        duplicates = sorted({entry for entry in entries if entries.count(entry) > 1})
        print(f"i18n: duplicate StringId entries: {', '.join(duplicates)}", file=sys.stderr)
        return 1
    if "LYRA_I18N_FALLBACK" not in text and "LYRA_I18N_ENTRY" not in text:
        print("i18n: catalog has no explicit locale entries", file=sys.stderr)
        return 1
    used = set()
    for source in SOURCE_ROOT.rglob("*"):
        if source.suffix not in {".cpp", ".h"}:
            continue
        used.update(re.findall(r"StringId::(\w+)", source.read_text(encoding="utf-8")))
    missing = sorted(used - set(entries) - {"Count"})
    if missing:
        print(f"i18n: source references missing StringId entries: {', '.join(missing)}",
              file=sys.stderr)
        return 1
    raw_labels = raw_label_literals()
    if raw_labels:
        print("i18n: direct make_label literals must be cataloged or explicitly allowlisted:",
              file=sys.stderr)
        for source, literal in raw_labels:
            print(f"  {source.relative_to(SOURCE_ROOT.parents[0])}: {literal!r}",
                  file=sys.stderr)
        return 1
    print(f"i18n: {len(entries)} StringId entries checked; explicit fallbacks are allowed in M1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
