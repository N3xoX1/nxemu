# Interface translations

The English text lives in the C++ sources and RML documents. `nxemu.pot` is
extracted from those files; each `.po` catalogue contains the translated text.
No gettext tools or network access are needed to build or run NXEmu.

These catalogues are shared by every frontend, so a text used by more than one
is translated once. Catalogue loading, `Tr()` and the language list live in
`src/nxemu-core/i18n`; each frontend reads the `.po` files its own way and
translates its own documents.

The interface supports English plus German (`de`), Spanish (`es`), French (`fr`),
Italian (`it`), Dutch (`nl`), Portuguese (`pt`), Russian (`ru`), Japanese (`ja`),
Korean (`ko`) and Chinese (`zh`, written in simplified characters). There is one
interface catalogue per language, with no regional variants. Console language
choices retain their original regional values because games use those values.
Both console and interface language selectors always show native language names
(Français, English, 日本語, etc.), independently of the current interface language.

By default the interface follows the global console language in **Settings →
General**. Enable the switch beside **Interface language** to choose a language
independently, or disable it to follow the console again. Changes apply
immediately, including to the open settings dialog. Regional console variants
share the same interface translation.

The choice is saved as `InterfaceLanguage` in the UI settings; an empty value
follows the console language. English is the fallback for missing translations.

On Android the console language is in **Settings → Console** and the interface
language in **Interface → Language**, where the first choice follows the console.
On the first run the console takes the device's language, as on the desktop. The
Gradle build copies these catalogues into the APK as `assets/ui/lang`. Android's
document sets its texts from code, so they are found in its C++ sources.

## Updating text

From the repository root:

```sh
python3 lang/extract_strings.py
python3 lang/check_translations.py
```

Update the `.po` entries when changing English text. Preserve `%1`, `%2`, `%n`,
line breaks and the `&` menu mnemonic marker. Translator notes belong immediately
before C++ calls, in a `// Translators:` comment. Never insert RML markup or
`{{` expressions into document translations.

Plural rules are language-specific: French treats zero like one; Russian has
three forms; Japanese, Korean and Chinese have one. The other supplied languages
have two forms. All forms must be supplied, including those that have identical
text. A `fuzzy` translation is ignored by the runtime and rejected by the checks.

Register a new interface language in `Translation::Languages` in
`src/nxemu-core/i18n/translation.h`. Catalogues are copied to `ui/lang` beside the
application or under its macOS resources, including when building an AppImage.
Add a frontend's sources to `SOURCE_DIRS` and its documents to `DOCUMENT_DIRS`
in `extract_strings.py`.

## Translation sources and review

Matching translations were recovered from Eden's `dist/languages/*.ts` files,
from checkout `cd651e02af80c30d2cffdcfd1c27f99a608e89c7`. The original locale files
are `de`, `es`, `fr`, `it`, `nl`, `pt_PT`, `ru_RU`, `ja_JP`, `ko_KR` and `zh_CN`.
Only complete, unambiguous matches were imported. Entries retaining the imported
wording are marked with a comment. Eden and NXEmu use GPLv3; credit belongs to
Eden's translation contributors.

New French, Spanish, Italian and German text was reviewed throughout. The other
six languages were completed using automatic translation, then corrected for
interface labels, save data, rendering terminology, menu mnemonics and plurals.
Further review by fluent speakers is welcome, particularly for longer technical
hints. The completeness checks validate structure rather than linguistic quality.

The Android interface's texts were added in all ten languages using automatic
translation, following each catalogue's existing terms and form of address. They
need the same review.

## Fonts

Lato Latin remains the bundled primary font. Scripts it does not cover use
installed system fonts: Core Text selects them on macOS, Windows fonts are read
from the Windows Fonts directory, and Linux uses Fontconfig when available.
Android reads its system font directory. Collection face indices are preserved.
No Asian font is bundled or downloaded by NXEmu.

Asian interface text and game titles require the corresponding fonts to be
installed on the host. Missing optional fonts do not prevent startup.
