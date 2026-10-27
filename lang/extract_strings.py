#!/usr/bin/env python3
"""Lists the texts of the interface in nxemu.pot, the template of its translations.

The catalogues are shared by every frontend, so the texts of all of them go
in the one template.

The texts are those the sources give to Tr(), TrCount() and TrMark(), those
Android's Kotlin gives to tr() and trCount(), and those of the documents: their text, and the explanations and hints their
elements have as attributes. An element with translate="no" is left out,
with what it holds.

    extract_strings.py            writes nxemu.pot
    extract_strings.py --check    fails when nxemu.pot is not up to date

A comment that starts with "Translators:" on the lines before a text goes
with it into the template.
"""
import re
import sys
import xml.parsers.expat
from pathlib import Path

LANG_DIR = Path(__file__).resolve().parent
ROOT = LANG_DIR.parent
DOCUMENT_DIRS = [ROOT / "src" / "nxemu" / "ui" / "assets"]
SOURCE_DIRS = [
    ROOT / "src" / "nxemu" / "ui",
    ROOT / "src" / "android" / "app" / "src" / "main" / "jni",
    ROOT / "src" / "android" / "app" / "src" / "main" / "java",
]
TEMPLATE = LANG_DIR / "nxemu.pot"
SHOWN_ATTRIBUTES = ("title", "placeholder")
CALLS = {
    ".kt": re.compile(r"\b(trCount|tr)\s*\("),
    ".cpp": re.compile(r"\b(TrCount|TrMark|Tr)\s*\("),
}
ESCAPES = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "0": "\0", "$": "$"}


class Catalogue:
    def __init__(self):
        self.entries = {}  # by (text, plural): the files it is used in and the comments for its translators

    def add(self, text, place, plural=None, comment=None):
        entry = self.entries.setdefault((text, plural), {"places": [], "comments": []})
        if place not in entry["places"]:
            entry["places"].append(place)
        if comment and comment not in entry["comments"]:
            entry["comments"].append(comment)


def normalised(text):
    return " ".join(text.split())


def has_letters(text):
    return any(character.isalpha() for character in text)


def read_document(path, catalogue):
    """The texts of an RML document, as RmlUi gives them to be translated."""
    name = path.relative_to(ROOT).as_posix()
    parser = xml.parsers.expat.ParserCreate()
    skipped = []  # the elements being read that are left out, the innermost last
    depth = 0
    pending = []  # the pieces of the text node being read

    def flush():
        text = normalised("".join(pending))
        pending.clear()
        if text and has_letters(text) and not skipped:
            catalogue.add(text, name)

    def start(tag, attributes):
        nonlocal depth
        flush()
        depth += 1
        if tag == "head" or attributes.get("translate") == "no":
            skipped.append(depth)
        if skipped:
            return
        for attribute in SHOWN_ATTRIBUTES:
            text = normalised(attributes.get(attribute, ""))
            if text and has_letters(text):
                catalogue.add(text, name)

    def end(tag):
        nonlocal depth
        flush()
        if skipped and skipped[-1] == depth:
            skipped.pop()
        depth -= 1

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = pending.append
    parser.Parse(path.read_text(encoding="utf-8"), True)


def read_literals(source, at):
    """The string literals that follow one another from at, as one text. None when there is none."""
    text = None
    while True:
        while at < len(source) and source[at] in " \t\r\n":
            at += 1
        if at >= len(source) or source[at] != '"':
            return text, at
        at += 1
        piece = bytearray()  # a \x escape is a byte of the UTF-8 text, as the compiler reads it
        while source[at] != '"':
            if source[at] == "\\" and source[at + 1] == "x":
                digits = 0
                while digits < 2 and source[at + 2 + digits] in "0123456789abcdefABCDEF":
                    digits += 1
                piece.append(int(source[at + 2:at + 2 + digits], 16))
                at += 2 + digits
                continue
            if source[at] == "\\":
                at += 1
                piece += ESCAPES.get(source[at], source[at]).encode("utf-8")
            else:
                piece += source[at].encode("utf-8")
            at += 1
        at += 1
        text = (text or "") + piece.decode("utf-8")


def translator_comment(lines, line):
    """The comment for the translators on the lines before the one given."""
    comment = []
    index = line - 2
    while index >= 0 and lines[index].strip().startswith("//"):
        comment.insert(0, lines[index].strip()[2:].strip())
        index -= 1
    text = " ".join(comment)
    at = text.find("Translators:")
    return text[at + len("Translators:"):].strip() if at >= 0 else None


def read_source(path, catalogue):
    """The texts a source gives to the functions that translate."""
    name = path.relative_to(ROOT).as_posix()
    source = path.read_text(encoding="utf-8")
    lines = source.split("\n")
    kotlin = path.suffix == ".kt"
    for match in CALLS[".kt" if kotlin else ".cpp"].finditer(source):
        line = source.count("\n", 0, match.start()) + 1
        text, at = read_literals(source, match.end())
        if kotlin and re.search(r'(?<!\\)\$[{A-Za-z_]', source[match.end():at]):
            sys.exit(f"{name}:{line}: a string template cannot be translated: write %1, %2 and give the values to tr()")
        if text is None:
            continue  # a text kept in a table, marked there
        plural = None
        if match.group(1) in ("TrCount", "trCount"):
            while at < len(source) and source[at] in " \t\r\n,":
                at += 1
            plural, at = read_literals(source, at)
            if plural is None:
                sys.exit(f"{name}:{line}: {match.group(1)}() needs its two texts written out")
        catalogue.add(text, name, plural, translator_comment(lines, line))


def quoted(text):
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    pieces = escaped.split("\n")
    if len(pieces) == 1:
        return f'"{escaped}"'
    # One line of the text per line of the file, as gettext writes them.
    lines = [piece + "\\n" for piece in pieces[:-1]] + ([pieces[-1]] if pieces[-1] else [])
    return '""\n' + "\n".join(f'"{line}"' for line in lines)


def template(catalogue):
    out = [
        "# The texts of the interface of NXEmu.",
        "# Made from the sources by extract_strings.py: not to be edited by hand.",
        "#",
        "# A translation is a copy of this file named after its language, fr.po",
        "# for French, with each text of the language as its msgstr. %1, %2 and %n",
        "# stand for values and stay as they are; & comes before the letter that",
        "# opens a menu from the keyboard.",
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: NXEmu\\n"',
        '"Language: \\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"Plural-Forms: nplurals=2; plural=(n != 1);\\n"',
        "",
    ]
    for (text, plural), entry in catalogue.entries.items():
        for comment in entry["comments"]:
            out.append(f"#. {comment}")
        for place in entry["places"]:
            out.append(f"#: {place}")
        out.append(f"msgid {quoted(text)}")
        if plural is None:
            out.append('msgstr ""')
        else:
            out.append(f"msgid_plural {quoted(plural)}")
            out.append('msgstr[0] ""')
            out.append('msgstr[1] ""')
        out.append("")
    return "\n".join(out)


def main():
    catalogue = Catalogue()
    for directory in DOCUMENT_DIRS:
        for path in sorted(directory.glob("*.rml")):
            read_document(path, catalogue)
    for directory in SOURCE_DIRS:
        for path in sorted(directory.rglob("*")):
            if path.suffix in (".cpp", ".h", ".mm", ".kt") and "i18n" not in path.parts and path.name != "Translation.kt":
                read_source(path, catalogue)
    text = template(catalogue)
    if "--check" in sys.argv:
        if not TEMPLATE.exists() or TEMPLATE.read_text(encoding="utf-8") != text:
            sys.exit("nxemu.pot is not up to date: run extract_strings.py")
        return
    TEMPLATE.write_text(text, encoding="utf-8")
    print(f"{len(catalogue.entries)} texts in {TEMPLATE.relative_to(ROOT).as_posix()}")


if __name__ == "__main__":
    main()
