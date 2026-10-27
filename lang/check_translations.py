#!/usr/bin/env python3
"""Check coverage, substitutions, plural forms and document safety in the .po files.

Uses only Python's standard library. Run extract_strings.py first after changing
English interface text. Exit status is nonzero if any catalogue needs attention.
"""
import ast
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys
import unicodedata

from extract_strings import LANG_DIR, ROOT


@dataclass
class Entry:
    text: str = ""
    plural: str | None = None
    forms: dict = field(default_factory=dict)
    places: list = field(default_factory=list)
    fuzzy: bool = False


def read_catalogue(path):
    entries = {}
    entry = Entry()
    current = None
    started = False

    def store():
        if started:
            if entry.text in entries:
                raise ValueError(f"duplicate msgid: {entry.text!r}")
            entries[entry.text] = entry

    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            store()
            entry, current, started = Entry(), None, False
            continue
        if line.startswith("#:"):
            entry.places.extend(line[2:].split())
            continue
        if line.startswith("#,") and "fuzzy" in line:
            entry.fuzzy = True
        if line.startswith("#"):
            continue
        match = re.fullmatch(r'(msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+(".*")', line)
        if match:
            directive, index, literal = match.groups()
            if directive == "msgid":
                if started:
                    raise ValueError(f"line {number}: entries must be separated by a blank line")
                started = True
                current = "text"
            elif directive == "msgid_plural":
                current = "plural"
            else:
                current = int(index or 0)
                if current in entry.forms:
                    raise ValueError(f"line {number}: repeated msgstr index {current}")
            value = ast.literal_eval(literal)
        elif line.startswith('"') and current is not None:
            value = ast.literal_eval(line)
            value = (entry.forms.get(current, "") if isinstance(current, int) else getattr(entry, current) or "") + value
        else:
            raise ValueError(f"line {number}: unsupported catalogue syntax")
        if not isinstance(value, str):
            raise ValueError(f"line {number}: expected quoted text")
        if isinstance(current, int):
            entry.forms[current] = value
        else:
            setattr(entry, current, value)
    store()
    return entries


def substitutions(text):
    return Counter(re.findall(r"%[1-9][0-9]*|%n", text))


def check(path, template):
    errors = []
    entries = read_catalogue(path)
    header = entries.get("", Entry()).forms.get(0, "")
    language = re.search(r"^Language:\s*(\S+)\s*$", header, re.M)
    if not language or language[1] != path.stem:
        errors.append("Language header does not match the filename")
    if "Content-Type: text/plain; charset=UTF-8\n" not in header:
        errors.append("missing UTF-8 Content-Type")
    plural = re.search(r"^Plural-Forms: nplurals=(\d+); plural=(.+);$", header, re.M)
    if not plural:
        errors.append("missing Plural-Forms header")
    count = int(plural[1]) if plural else 0
    if count not in (1, 2, 3):
        errors.append(f"unexpected plural form count: {count}")
    missing, extra = template.keys() - entries.keys(), entries.keys() - template.keys()
    errors.extend(f"missing text: {text!r}" for text in sorted(missing))
    errors.extend(f"obsolete text: {text!r}" for text in sorted(extra))
    for text in template.keys() & entries.keys() - {""}:
        source, translated = template[text], entries[text]
        if source.plural != translated.plural:
            errors.append(f"changed English plural: {text!r}")
        expected = set(range(count)) if source.plural is not None else {0}
        if translated.forms.keys() != expected:
            errors.append(f"incorrect msgstr indices: {text!r}")
        if translated.fuzzy:
            errors.append(f"unconfirmed (fuzzy) translation: {text!r}")
        for form, value in translated.forms.items():
            if not value.strip():
                errors.append(f"empty form {form}: {text!r}")
            if any(unicodedata.category(character) == "Cf" for character in value):
                errors.append(f"invisible formatting character in form {form}: {text!r}")
            if substitutions(value) != substitutions(text):
                errors.append(f"changed substitutions in form {form}: {text!r}")
            if any(place.endswith(".rml") for place in source.places) and ("<" in value or "{{" in value):
                errors.append(f"unsafe document text: {text!r}")
            if "&" in text and value.count("&") != text.count("&"):
                errors.append(f"missing or extra menu mnemonic: {text!r}")
            if value.count("\n") != text.count("\n"):
                errors.append(f"changed line breaks in form {form}: {text!r}")
    return errors


def main():
    template = read_catalogue(LANG_DIR / "nxemu.pot")
    files = sorted(LANG_DIR.glob("*.po"))
    registry = (ROOT / "src" / "nxemu-core" / "i18n" / "translation.h").read_text(encoding="utf-8")
    registered = set(re.findall(r'\{"([a-z]{2})", "', registry)) - {"en"}
    present = {path.stem for path in files}
    failed = False
    if present != registered:
        print(f"Catalogue/selector mismatch: missing {registered - present}, unregistered {present - registered}", file=sys.stderr)
        failed = True
    for path in files:
        try:
            errors = check(path, template)
        except (ValueError, SyntaxError) as error:
            errors = [str(error)]
        for error in errors:
            print(f"{path.name}: {error}", file=sys.stderr)
        if errors:
            failed = True
        else:
            print(f"{path.name}: {len(template) - 1} texts, complete")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
