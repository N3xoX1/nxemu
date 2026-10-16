# Interface colours

The light and dark palettes live in `../host/ui_theme.cpp`, in `kColours`.
Styles refer to semantic colours such as `var(--surface)`, `var(--text)` and
`var(--border)`. Change the palette to adjust all components together.

RmlUi does not support CSS custom properties itself. `UiThemeFileInterface`
resolves these tokens when it reads an RCSS file, before the normal RmlUi
parser compiles it. Only palette tokens are supported, without CSS fallbacks
or inheritance. Changing themes reloads styles without rebuilding documents;
there is no token evaluation in the rendering loop.

The general settings offer System default, Light and Dark, saved globally as
`UI.InterfaceTheme`. System default reads the OS preference at startup and
when the application regains focus. Missing preferences fall back to dark.
Linux activation checks run on a background worker and report through an SDL
event, so a slow desktop portal does not stall input or rendering.
macOS window controls follow the same choice. Game/profile artwork, the pause
overlay and performance graphs retain their own colours.
