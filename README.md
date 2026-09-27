# goed

A minimalist GUI editor for Go, written in C and built with [Tiny C Compiler](https://bellard.org/tcc/).
One window, no menus, keyboard commands only. ~40 KB, no runtime dependencies.

- Go syntax highlighting
- `gofmt` on every save
- `go run` / `go vet` with output in the bottom pane; double-click an error to jump to it
- Auto-indent, find, go to line
- Keeps the file's line endings (LF or CRLF)

## Usage

```
goed.exe [file.go]
```

With no argument it opens an empty buffer. A path that doesn't exist yet opens as a new file.

| Key | Action |
|---|---|
| Ctrl+N | New file |
| Ctrl+O | Open |
| Ctrl+S | Save (then `gofmt`) |
| Ctrl+Shift+S | Save as |
| Ctrl+R | `go run` the current file |
| Ctrl+B | `go vet` the current file |
| Esc | Stop the running program / close the prompt |
| Ctrl+F | Find |
| F3 | Find next (wraps around) |
| Ctrl+G | Go to line |
| Ctrl+Q | Quit (asks to save unsaved changes) |
| F1 | Show this key list |

Double-click a line like `main.go:12:5: undefined: x` in the output pane to jump to line 12, column 5.

Run and vet save the file first. Esc also kills the program `go run` built, not just `go` itself.

If `gofmt` or `go` isn't on `PATH`, saving still works and the output pane says the tool wasn't found.

## Requirements

- Windows 10 or 11 (uses the RichEdit control from `Msftedit.dll`)
- [Go](https://go.dev/dl/) on `PATH`, for `gofmt`, `go run` and `go vet`

To build you also need:

- TCC 0.9.27 (x86_64 Windows) on `PATH`
- PowerShell 7 (`pwsh`), used by the build and icon scripts

## Build

```
pwsh -File build.ps1
```

This runs the unit tests, compiles `goed.exe`, then embeds the icon, stopping at the first step that fails. Close goed before rebuilding, because Windows won't let TCC overwrite a running exe.

The same steps by hand:

```
tcc -DUNICODE -luser32 -lgdi32 -L. -lcomdlg32 -run test_goed.c
tcc -DUNICODE -Wall goed.c -luser32 -lgdi32 -L. -lcomdlg32 -Wl,-subsystem=windows -o goed.exe
pwsh -File icon.ps1
```

TCC can't compile `.rc` resource files, so `icon.ps1` draws the icon, saves `goed.ico`, and writes it into the finished `goed.exe` with the Windows `UpdateResource` API. Run it after every compile, since each build overwrites the exe.

`comdlg32.def` is the import library for the Open/Save dialogs, which TCC doesn't ship. If it's missing, regenerate it:

```
tcc -impdef C:\Windows\System32\comdlg32.dll -o comdlg32.def
```

## Files

| File | Purpose |
|---|---|
| `goed.c` | The editor |
| `test_goed.c` | Unit tests for the lexer, line endings, error parsing and command line |
| `build.ps1` | Test, compile, embed icon |
| `icon.ps1` | Draws `goed.ico` and embeds it in `goed.exe` |
| `comdlg32.def` | Import definitions for the file dialogs |
| `sample.go` | A small file for trying it out |

## Limits

- The whole file is re-highlighted after each edit (debounced). Fine up to a few thousand lines.
- After `gofmt` reformats, the cursor returns to the same character position, so it can shift slightly if the indentation changed.
- One file at a time; no tabs, settings or themes.
