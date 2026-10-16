# FORK NOTICE

This repository is an **unofficial fork** of
[Thorium/NppTreeSitter](https://github.com/Thorium/NppTreeSitter). It is **not affiliated
with, endorsed by, or supported by** the upstream author.

## Why this fork exists

Notepad++ highlighting for **1C:Enterprise** languages: the BSL language and the 1C query
language (SDBL). It carries a small set of local fixes and an opt-in feature that are not in
upstream:

- **BSL auto-detect** for `.bsl` / `.os` (upstream #7).
- **Code folding fix** for headers containing a nested expression (upstream #8).
- **Style/font fix**: when a lexer is attached directly (auto-detect / session reload),
  inherit the editor font instead of rendering unreadably tiny text.
- **SDBL highlighting**: 1C query fragments inside BSL string literals (including `+`
  concatenations) and standalone `.sdbl` / `.1cquery` documents, using a dedicated query
  palette.

The exact set of changes is on the branches listed below; the tag tied to a release points at
the commit the corresponding binary was built from.

## Branches

| Branch | Contents |
|---|---|
| `master` | Unmodified mirror of upstream (tracked against `upstream/master`). |
| `sonnix/bsl-detect` | `.bsl`/`.os` extension mapping (upstream PR #11). |
| `sonnix/styles` | Editor-font inheritance fix (upstream PR #10). |
| `sonnix/folding` | Folding fix (upstream PR #9). |
| `sonnix/injections` | SDBL query fragments inside BSL strings + query palette. |
| `sonnix/sdbl` | Standalone `treesitter.sdbl` language (`.sdbl`/`.1cquery`). |
| `sonnix/personal` | Integration of all of the above; the Extended build is made from here. |

## License

Upstream ships **no `LICENSE` file**. Its `README.md` declares `GPL-2.0-or-later`, while
`NppTreeSitter.vcxproj` carries `Unlicense` / `Public Domain` (a project-template leftover).
These conflict.

To stay on the safe side, this fork **complies with GPL-2.0-or-later** — the stricter
interpretation. Satisfying GPL also satisfies the more permissive reading, so this is safe
whichever way the upstream intent is resolved. The full license text is in [`LICENSE`](LICENSE).

Compliance for released binaries:

- **Source of the exact binary**: this public repository at the release tag (see Releases).
- **Build scripts / installers**: included in this repository.
- **Authors' notices preserved**: upstream copyright and the original `README.md` are kept.

If upstream clarifies its license (upstream still has no `LICENSE` file as of this notice),
this section will be updated accordingly; it does not change the fact that GPL-2.0-or-later
obligations are met.

## Building

See the upstream `README.md` (Build the plugin / Build grammar DLLs sections). The plugin is
built with MSBuild; grammar DLLs are built with `grammars/build-grammars.ps1`. The Extended
distributable used by the 1C highlighting project is assembled separately (see that project).
