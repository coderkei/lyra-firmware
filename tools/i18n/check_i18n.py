#!/usr/bin/env python3
"""Validate the complete, embedded Lyra GUI translation catalog."""

from pathlib import Path
import ast
import re
import sys


CATALOG = Path(__file__).with_name("lyra_i18n_catalog.inc")
SOURCE_ROOT = CATALOG.parents[2] / "main"
GUI_ROOT = SOURCE_ROOT / "gui"
LOCALES = ("en", "fr", "de", "es", "it", "ja", "ko", "ru", "zh-Hans", "zh-Hant")
MACRO_ARITIES = {"LYRA_I18N_ENTRY": 11, "LYRA_I18N_COUNT": 22}

# These values are intentionally not translation strings. They are live-data
# placeholders, symbols, or fixed product/hardware identifiers.
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


def macro_invocations(text: str):
    """Yield (macro name, raw argument text) for balanced catalog calls."""
    pattern = re.compile(r"(?m)^(LYRA_I18N_[A-Z_]+)\(")
    for match in pattern.finditer(text):
        macro = match.group(1)
        depth = 1
        index = match.end()
        in_string = False
        escaped = False
        while index < len(text) and depth:
            char = text[index]
            if in_string:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_string = False
            elif char == '"':
                in_string = True
            elif char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
                if depth == 0:
                    yield macro, text[match.end():index]
                    break
            index += 1


def split_arguments(raw: str) -> list[str]:
    """Split macro arguments without treating commas inside strings as separators."""
    arguments: list[str] = []
    start = 0
    depth = 0
    in_string = False
    escaped = False
    for index, char in enumerate(raw):
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
        elif char == ',' and depth == 0:
            arguments.append(raw[start:index].strip())
            start = index + 1
    arguments.append(raw[start:].strip())
    return arguments


def decode_string_argument(argument: str) -> str | None:
    """Decode adjacent C string literals and reject expressions/macros."""
    token_pattern = re.compile(r'"(?:\\.|[^"\\])*"', re.DOTALL)
    tokens = list(token_pattern.finditer(argument))
    remainder = token_pattern.sub("", argument).strip()
    if not tokens or remainder:
        return None
    try:
        return "".join(ast.literal_eval(token.group(0)) for token in tokens)
    except (SyntaxError, ValueError):
        return None


def placeholder_signature(value: str) -> tuple[tuple[str, str], ...]:
    """Return ordered printf argument types, preserving length modifiers."""
    pattern = re.compile(
        r"%(?!%)[-+#0 '\\d]*(?:\.\\d+)?(?:hh|h|ll|l|L)?([diuoxXfFeEgGaAcsp])"
    )
    signatures: list[tuple[str, str]] = []
    for match in pattern.finditer(value):
        token = match.group(0)
        conversion = match.group(1)
        length = ""
        for candidate in ("hh", "ll", "h", "l", "L"):
            if candidate in token:
                length = candidate
                break
        signatures.append((length, conversion))
    return tuple(signatures)


def newline_signature(value: str) -> int:
    """Return the number of intentional line breaks in a message."""
    return value.count("\n")


def parse_catalog(text: str) -> tuple[list[str], list[str]]:
    entries: list[str] = []
    errors: list[str] = []
    for macro, raw in macro_invocations(text):
        if macro == "LYRA_I18N_FALLBACK" or macro == "LYRA_I18N_COUNT_RU":
            errors.append(f"{macro} is a development-only macro")
            continue
        if macro not in MACRO_ARITIES:
            errors.append(f"unknown catalog macro {macro}")
            continue
        arguments = split_arguments(raw)
        expected = MACRO_ARITIES[macro]
        if len(arguments) != expected:
            errors.append(f"{macro}: expected {expected} arguments, found {len(arguments)}")
            continue
        identifier = arguments[0]
        if not re.fullmatch(r"[A-Za-z_]\w*", identifier):
            errors.append(f"{macro}: invalid StringId {identifier!r}")
            continue
        entries.append(identifier)
        values = [decode_string_argument(argument) for argument in arguments[1:]]
        if any(value is None for value in values):
            errors.append(f"{identifier}: every locale value must be a C string literal")
            continue
        decoded = [value for value in values if value is not None]
        if any(not value for value in decoded):
            errors.append(f"{identifier}: empty locale value")
        if macro == "LYRA_I18N_ENTRY":
            source = decoded[0]
            for locale, value in zip(LOCALES[1:], decoded[1:]):
                if placeholder_signature(value) != placeholder_signature(source):
                    errors.append(f"{identifier}/{locale}: printf placeholders differ from English")
                if newline_signature(value) != newline_signature(source):
                    errors.append(f"{identifier}/{locale}: newline layout differs from English")
        else:
            plural_forms = [
                ("en", decoded[0], decoded[1]),
                ("fr", decoded[2], decoded[3]),
                ("de", decoded[4], decoded[5]),
                ("es", decoded[6], decoded[7]),
                ("it", decoded[8], decoded[9]),
                ("ja", decoded[10], decoded[11]),
                ("ko", decoded[12], decoded[13]),
                ("ru", decoded[14], decoded[15], decoded[16]),
                ("zh-Hans", decoded[17], decoded[18]),
                ("zh-Hant", decoded[19], decoded[20]),
            ]
            source_one, source_many = decoded[:2]
            for locale, *forms in plural_forms:
                one, many = forms[0], forms[1]
                if placeholder_signature(one) != placeholder_signature(source_one):
                    errors.append(f"{identifier}/{locale} one: printf placeholders differ from English")
                if placeholder_signature(many) != placeholder_signature(source_many):
                    errors.append(f"{identifier}/{locale} many: printf placeholders differ from English")
                if newline_signature(one) != newline_signature(source_one):
                    errors.append(f"{identifier}/{locale} one: newline layout differs from English")
                if newline_signature(many) != newline_signature(source_many):
                    errors.append(f"{identifier}/{locale} many: newline layout differs from English")
                if len(forms) == 3:
                    if placeholder_signature(forms[2]) != placeholder_signature(source_many):
                        errors.append(f"{identifier}/{locale} many: printf placeholders differ from English")
                    if newline_signature(forms[2]) != newline_signature(source_many):
                        errors.append(f"{identifier}/{locale} many: newline layout differs from English")
    return entries, errors


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
                    if char == "\\" and index + 1 < len(text):
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
    try:
        text = CATALOG.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        print(f"i18n: catalog is not valid UTF-8: {error}", file=sys.stderr)
        return 1

    entries, errors = parse_catalog(text)
    if not entries:
        errors.append("catalog contains no entries")
    if len(entries) != len(set(entries)):
        duplicates = sorted({entry for entry in entries if entries.count(entry) > 1})
        errors.append(f"duplicate StringId entries: {', '.join(duplicates)}")

    used = set()
    for source in SOURCE_ROOT.rglob("*"):
        if source.suffix not in {".cpp", ".h"}:
            continue
        used.update(re.findall(r"StringId::(\w+)", source.read_text(encoding="utf-8")))
    missing = sorted(used - set(entries) - {"Count"})
    if missing:
        errors.append(f"source references missing StringId entries: {', '.join(missing)}")

    raw_labels = raw_label_literals()
    if raw_labels:
        errors.append("direct make_label literals must be cataloged or explicitly allowlisted:")
        errors.extend(f"  {source.relative_to(SOURCE_ROOT.parents[0])}: {literal!r}"
                      for source, literal in raw_labels)

    if errors:
        for error in errors:
            print(f"i18n: {error}", file=sys.stderr)
        return 1

    print(f"i18n: {len(entries)} StringId entries checked across {len(LOCALES)} locales")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
