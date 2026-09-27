# Terminal Conformance Oracles

This file defines which sources may establish terminal behavior for
`vnm_terminal`. Nothing in this file imports external source, comments, test
names, transcripts, byte streams, captured output, fixture bytes, or goldens.

Checked-in expected behavior must cite an approved `oracle_id`. Strong-copyleft
and reference-only material may inform local questions, but never checked-in
source, fixtures, expected output, or golden data.

Each oracle record uses:

- `oracle_id`
- `oracle_type`
- `status`
- `license_posture`
- `pin_or_version`
- `checked_in_output_allowed`
- `allowed_use`
- `forbidden_use`
- `inventory_ref`
- `notes`

## product-decision-vnm-terminal

oracle_id: product-decision-vnm-terminal
oracle_type: product-decision
status: approved
license_posture: project-owned
pin_or_version: current repository commit
checked_in_output_allowed: yes
allowed_use: document vnm_terminal behavior where terminal references diverge
forbidden_use: no silent undocumented behavior change
inventory_ref: none
notes: product decisions must be reviewed before fixtures cite them

## product-platform-matrix

oracle_id: product-platform-matrix
oracle_type: product-decision
status: approved
license_posture: project-owned
pin_or_version: current repository commit
checked_in_output_allowed: yes
allowed_use: platform support and exclusion decisions for vnm_terminal
forbidden_use: no implicit support claims outside the listed matrix
inventory_ref: none
notes: Windows x64, Linux x86_64, and macOS Darwin builds are supported native
targets. Other platforms have no native backend support claim.

## independent-vnm-fixture

oracle_id: independent-vnm-fixture
oracle_type: independently-authored-fixture
status: approved
license_posture: project-owned
pin_or_version: fixture provenance headers
checked_in_output_allowed: yes
allowed_use: authored regression fixtures and generators with provenance headers
forbidden_use: no copied external output, names, comments, byte streams, or goldens
inventory_ref: tests/conformance/README.md
notes: fixture authors must avoid GPL-derived material

## dec-vt330-vt340-graphics-manual

oracle_id: dec-vt330-vt340-graphics-manual
oracle_type: external-standard
status: approved
license_posture: published DEC manual; behavior specification only, no import
pin_or_version: VT330/VT340 Programmer Reference Manual vol. 2, Graphics Programming, EK-VT3XX-GP-002, second edition, May 1988
checked_in_output_allowed: yes
allowed_use: sixel graphics behavior from chapter 14 and the VT340 default color map and HLS hue circle from chapter 2; fixtures and expected results are authored from that text
forbidden_use: no manual text, figures, or examples copied in as fixtures or goldens
inventory_ref: none
notes: read at https://vt100.net/docs/vt3xx-gp/chapter14.html and chapter2.html; where the manual is silent the sixel matrix row records the product decision

## dec-vt520-ansi-color

oracle_id: dec-vt520-ansi-color
oracle_type: external-standard
status: approved
license_posture: published DEC manual; behavior specification only, no import
pin_or_version: VT520/VT525 Programmer Information, EK-VT520-RM, SGR Table 5-15
checked_in_output_allowed: yes
allowed_use: ANSI color SGR 30-37 and 40-47, default color resets 39 and 49, and DA1 extension 22; fixtures and expected results are independently authored from the specification
forbidden_use: no manual text, figures, or examples copied in as fixtures or goldens
inventory_ref: none
notes: read at https://vt100.net/dec/ek-vt520-rm.pdf; palette RGB values are product-owned color-scheme policy

## dec-vt420-rectangular-areas

oracle_id: dec-vt420-rectangular-areas
oracle_type: external-standard
status: approved
license_posture: published DEC manual; behavior specification only, no import
pin_or_version: VT420 Programmer Reference Manual, EK-VT420-RM.002, second edition, February 1992, chapter 9
checked_in_output_allowed: yes
allowed_use: independently authored tests of DECCRA, DECFRA, DECERA, DECSERA, DECSACE, DECCARA and DECRARA coordinates, defaults, protection and visual attributes
forbidden_use: no manual text, figures, or examples copied in as fixtures or goldens
inventory_ref: none
notes: read at https://vt100.net/mirror/mds-199909/cd3/term/vt420rm2.pdf; the screen model exposes one active page and clamps DEC page selectors to that page

## dec-vt420-macros

oracle_id: dec-vt420-macros
oracle_type: external-standard
status: approved
license_posture: published DEC manual; behavior specification only, no import
pin_or_version: VT420 Programmer Reference Manual, EK-VT420-RM.002, second edition, February 1992, pages 44-47 and 240
checked_in_output_allowed: yes
allowed_use: DECDMAC literal and hex definition, repeat syntax, DECINVM ordering, macro-space report, and DA1 extension 32; fixtures and expected results are independently authored
forbidden_use: no manual text, figures, or examples copied in as fixtures or goldens
inventory_ref: none
notes: read at https://vt100.net/mirror/mds-199909/cd3/term/vt420rm2.pdf; recursion and expansion limits are product-owned safety policy

## dec-vt420-horizontal-scrolling

oracle_id: dec-vt420-horizontal-scrolling
oracle_type: external-standard
status: approved
license_posture: published DEC manual; behavior specification only, no import
pin_or_version: VT420 Programmer Reference Manual, EK-VT420-RM-002, chapters 6, 8, 10, and 12
checked_in_output_allowed: yes
allowed_use: DA1 extension 21, DECVSSM, DECSLRM, DECIC, DECDC, DECBI, DECFI, and rectangular scrolling behavior; independently authored fixtures and expected results
forbidden_use: no manual text, figures, or examples copied in as fixtures or goldens
inventory_ref: none
notes: read at https://vt100.net/mirror/mds-199909/cd3/term/vt420rm2.pdf; published pages 133-135, 149-152, 177-178, and 229-231

## xterm-409-reference

oracle_id: xterm-409-reference
oracle_type: reference-only
status: approved-reference
license_posture: permissive reference; no import
pin_or_version: xterm patch 409, 2026-04-13
checked_in_output_allowed: no
allowed_use: public docs, patch notes, and local manual comparison
forbidden_use: no source, transcripts, runtime output, fixture bytes, or goldens
inventory_ref: docs/terminal_reference_inventory.md#xterm-409
notes: authoritative xterm pin for behavior questions

## vttest-reference

oracle_id: vttest-reference
oracle_type: reference-only
status: approved-reference
license_posture: permissive reference; no import
pin_or_version: vttest-20251205.tgz
checked_in_output_allowed: no
allowed_use: local manual behavior exploration and checklist inspiration
forbidden_use: no captured screens, transcripts, byte streams, or goldens
inventory_ref: docs/terminal_reference_inventory.md#vttest
notes: menu output never becomes a repo oracle

## contour-candidate

oracle_id: contour-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: Apache-2.0 candidate; import requires provenance gate
pin_or_version: record exact commit before import
checked_in_output_allowed: no
allowed_use: behavior questions; material import requires provenance approval
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#contour
notes: candidate status is not approval

## libvterm-candidate

oracle_id: libvterm-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: MIT candidate; import requires provenance gate
pin_or_version: libvterm-0.3.3.tar.gz
checked_in_output_allowed: no
allowed_use: behavior questions; material import requires provenance approval
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#libvterm
notes: candidate status is not approval

## wezterm-candidate

oracle_id: wezterm-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: MIT candidate; bundled materials need separate audit
pin_or_version: record exact commit before import
checked_in_output_allowed: no
allowed_use: behavior questions; material import requires provenance approval
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#wezterm
notes: candidate status is not approval

## microsoft-terminal-candidate

oracle_id: microsoft-terminal-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: MIT candidate; bundled materials need separate audit
pin_or_version: record exact commit before import
checked_in_output_allowed: no
allowed_use: Windows and ConPTY behavior questions
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#microsoft-terminal
notes: candidate status is not approval

## iterm2-esctest-reference-only

oracle_id: iterm2-esctest-reference-only
oracle_type: reference-only
status: strong-copyleft-reference
license_posture: GPL-family reference-only
pin_or_version: record commit only in local reference logs
checked_in_output_allowed: no
allowed_use: local exploratory behavior questions only
forbidden_use: no GPL-derived source, names, comments, output, streams, or goldens
inventory_ref: docs/terminal_reference_inventory.md#iterm2-esctest
notes: clean-room authored fixtures only

## strong-copyleft-terminal-rejected

oracle_id: strong-copyleft-terminal-rejected
oracle_type: rejected-reference
status: rejected
license_posture: GPL-family; not a dependency or oracle source
pin_or_version: not adopted
checked_in_output_allowed: no
allowed_use: local behavior questions only if needed
forbidden_use: no source, test names, output, byte streams, fixtures, or goldens
inventory_ref: none
notes: explicitly out of scope as a dependency and oracle source
