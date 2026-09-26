# Terminal Sequence Matrix

This matrix records supported, ignored, rejected, and unsupported terminal
sequence behavior. It is a policy artifact, not an implementation. Tests and
fixtures may cover narrower cases, but they must not silently change the
behavior recorded here.

Each sequence record uses these exact fields:

- `id`
- `family`
- `sequence`
- `feature`
- `status`
- `action_category`
- `behavior`
- `host_policy`
- `payload_limit`
- `recovery`
- `reply`
- `diagnostic`
- `oracle`

Valid status values are `supported`, `ignored`, `rejected`, and
`unsupported-discard`.

## osc-payload-limit

id: osc-payload-limit
family: OSC
sequence: OSC string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## dcs-payload-limit

id: dcs-payload-limit
family: DCS
sequence: DCS string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard; sixel data is never buffered and is bounded by dcs-sixel-decoded-limit instead
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## apc-payload-limit

id: apc-payload-limit
family: APC
sequence: APC string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## pm-payload-limit

id: pm-payload-limit
family: PM
sequence: PM string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## sos-payload-limit

id: sos-payload-limit
family: SOS
sequence: SOS string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## dcs-unsupported-discard

id: dcs-unsupported-discard
family: DCS
sequence: unsupported DCS
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state; a header with an intermediate, a private marker, or a final byte other than q, such as DECRQSS $q and XTGETTCAP +q, is not sixel
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported DCS diagnostic
oracle: product-decision-vnm-terminal

## dcs-sixel-image

id: dcs-sixel-image
family: DCS
sequence: DCS P1 ; P2 ; P3 q sixel data ST, as ESC P and ESC \ or as C1 controls
feature: sixel graphics image
status: supported
action_category: screen-mutation
behavior: a header of digits and semicolons ending in q streams the data to a decoder as it arrives, and ST yields one decoded image of RGBA8 pixels in sixel device pixels with its extent, final sixel cursor row top, and pixel aspect ratio; data characters ? to ~ are six pixels with the least significant bit on top; ! repeats the next data character; # selects, or defines in HLS or RGB percent and selects, one of 256 color registers private to the image and starting from the VT340 default color map; " sets the aspect ratio and the background raster size; $ returns to the left of the sixel line and - moves to the next one; P1 picks the aspect ratio from the manual table, P2 1 leaves undrawn pixels transparent while 0, 2 and other values paint them and the declared raster with register 0, and P3 is ignored; the image is placed with its upper-left corner at the cursor, each text row taking the band of pixel rows that falls on it; the scroll region scrolls as far as the image's final sixel row needs to fit, and the text cursor ends on the row the top of the final sixel row falls in, a trailing graphics new line included, at the image's first column; DECSDM set places images per dec-private-80 instead
host_policy: the cell pixel size (csi-window-op-16) sets the pixel rows each text row takes
payload_limit: raw sixel data unbounded and never buffered; decoded image bounded by dcs-sixel-decoded-limit
recovery: ESC [ or C1 CSI inside the data abandons the image and resets to ground
reply: no-reply
diagnostic: DCS recovery diagnostic when a CSI abandons the image; limit diagnostic per dcs-sixel-decoded-limit; unsupported DCS sixel diagnostic for an image that arrives with no cell pixel size
oracle: dec-vt330-vt340-graphics-manual

## dcs-sixel-product-decisions

id: dcs-sixel-product-decisions
family: DCS
sequence: DCS P1 ; P2 ; P3 q sixel data ST
feature: sixel behavior the manual leaves open
status: supported
action_category: screen-mutation
behavior: raster attributes apply only before the first data character or graphics new line, so an image has one aspect ratio; Pan/Pad is rounded up, as OpenConsole does, where the manual says nearest, and an omitted or zero Pad keeps the ratio; P1 above 9 is 1:1; data drawn before a color is selected uses register 15; color numbers past 255 wrap; registers 16 to 255 start with the VT340 map repeated; a register color is taken when a sixel is drawn, while the background takes register 0 as it stands at ST; the extent covers the drawn pixels and, with a background, the declared raster; numeric parameters saturate at 32767; other bytes are ignored; with no cell pixel size known the image is dropped, cursor movement included; an image that would start below the bottom margin is dropped, as OpenConsole drops it; the image is clipped at the right margin, and a band with no drawn pixel leaves its row alone; a cell that receives a drawn pixel loses its text and hyperlink and keeps its style, a wide glyph whole; an image over an earlier image on the same row draws its drawn pixels over the earlier ones, which are first resampled to the new cell pixel size if theirs differs; the aspect ratio is not clamped to the scroll region height, which OpenConsole does, so the cursor can differ from OpenConsole's only for ratios above (region rows x 20) / 6; each text row keeps at most one image, which moves and dies with the row and reaches history with it, and a row whose history record would exceed the record limit (dcs-sixel-decoded-limit) keeps its text and drops its image, at append and when the ring shrinks; printing into a cell and erasing it with EL, ECH or ED 0 and 1 clear the image pixels of that cell, text or not, and a row image left with no drawn pixel is dropped; ED 2 drops the images of the screen; ICH and DCH move image columns with the cells, losing those pushed past the right margin or deleted; placing an image hard-terminates the soft wraps into and out of the rows it covers, and text wrapping onto a row that shows an image wraps hard, so an image row always starts its logical line; reflow keeps a row image on the first row of its logical line, its columns never make continuation rows, and widening back restores it unchanged; a repaint that recovers rows into history keeps the images those rows showed
host_policy: none
payload_limit: decoded image bounded by dcs-sixel-decoded-limit
recovery: CAN or SUB inside the data abandons the image without a diagnostic and returns to ground; other string families keep CAN and SUB as payload
reply: no-reply
diagnostic: none for a cancelled image
oracle: product-decision-vnm-terminal

## dcs-sixel-decoded-limit

id: dcs-sixel-decoded-limit
family: DCS
sequence: DCS P1 ; P2 ; P3 q sixel data ST
feature: decoded sixel image hard limit
status: supported
action_category: payload-limit
behavior: caps an image at the retained history's largest record, ring capacity / 8, following capacity changes; the decoded size is extent width x height x 4 bytes and is checked whenever the extent grows and again at ST, never against declared raster attributes alone; an image over the cap keeps no pixels but still ends with its extent, final sixel cursor row top, and aspect ratio
host_policy: the retained history capacity sets the cap
payload_limit: decoded image up to retained history capacity / 8 bytes, 8388608 at the default 67108864 byte ring
recovery: drop the pixels and keep decoding geometry until ST or a recovery boundary
reply: no-reply
diagnostic: DCS sixel payload-limit diagnostic with the decoded size and the cap
oracle: product-decision-vnm-terminal

## apc-unsupported-discard

id: apc-unsupported-discard
family: APC
sequence: unsupported APC
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported APC diagnostic
oracle: product-decision-vnm-terminal

## pm-unsupported-discard

id: pm-unsupported-discard
family: PM
sequence: unsupported PM
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported PM diagnostic
oracle: product-decision-vnm-terminal

## sos-unsupported-discard

id: sos-unsupported-discard
family: SOS
sequence: unsupported SOS
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported SOS diagnostic
oracle: product-decision-vnm-terminal

## osc-0-title

id: osc-0-title
family: OSC
sequence: OSC 0
feature: icon and window title set
status: supported
action_category: notification
behavior: updates terminal icon name and terminal title, emitting both notifications
host_policy: host observes terminal icon name and title signals
payload_limit: 4096 decoded Unicode scalars
recovery: reject over-limit title and preserve previous icon name and title
reply: no-reply
diagnostic: title overflow diagnostic
oracle: xterm-409-reference

## osc-1-icon-name

id: osc-1-icon-name
family: OSC
sequence: OSC 1
feature: icon name set
status: supported
action_category: notification
behavior: updates terminal icon name and emits icon name notification without changing terminal title
host_policy: host observes terminal icon name signal
payload_limit: 4096 decoded Unicode scalars
recovery: reject over-limit icon name and preserve previous icon name
reply: no-reply
diagnostic: title overflow diagnostic
oracle: xterm-409-reference

## osc-2-title

id: osc-2-title
family: OSC
sequence: OSC 2
feature: window title set
status: supported
action_category: notification
behavior: updates terminal title and emits title notification without changing terminal icon name
host_policy: host observes terminal title signal
payload_limit: 4096 decoded Unicode scalars
recovery: reject over-limit title and preserve previous title
reply: no-reply
diagnostic: title overflow diagnostic
oracle: xterm-409-reference

## osc-title-overflow

id: osc-title-overflow
family: OSC
sequence: OSC 0, OSC 1, or OSC 2 over 4096 scalars
feature: title or icon-name overflow
status: rejected
action_category: rejected-with-recovery
behavior: rejects whole title or icon name and preserves previous affected state
host_policy: no title or icon name notification
payload_limit: 4096 decoded Unicode scalars
recovery: recover at ST and keep previous title or icon name
reply: no-reply
diagnostic: title overflow diagnostic
oracle: product-decision-vnm-terminal

## osc-8-open

id: osc-8-open
family: OSC
sequence: OSC 8 open
feature: hyperlink open
status: supported
action_category: screen-mutation
behavior: begins hyperlink metadata for subsequent cells
host_policy: activation remains host UI policy
payload_limit: 1048576 raw bytes, of which 8192 raw bytes may be hyperlink body
recovery: malformed parameters ignored with diagnostic, oversized body refused with diagnostic and clears the active hyperlink
reply: no-reply
diagnostic: malformed OSC 8 diagnostic
oracle: xterm-409-reference

## osc-8-close

id: osc-8-close
family: OSC
sequence: OSC 8 close
feature: hyperlink close
status: supported
action_category: screen-mutation
behavior: ends active hyperlink metadata
host_policy: activation remains host UI policy
payload_limit: 1048576 raw bytes, of which 8192 raw bytes may be hyperlink body
recovery: malformed close ignored with diagnostic
reply: no-reply
diagnostic: malformed OSC 8 diagnostic
oracle: xterm-409-reference

## osc-52-write-default-deny

id: osc-52-write-default-deny
family: OSC
sequence: OSC 52 write
feature: clipboard write default denial
status: supported
action_category: host-policy-request
behavior: recognizes write request but mutates no clipboard by default
host_policy: deny by default; missing late or duplicate responses deny
payload_limit: 1048576 raw bytes
recovery: malformed or oversized request denied with diagnostic
reply: no-reply
diagnostic: OSC 52 denied diagnostic
oracle: product-decision-vnm-terminal

## osc-52-write-host-request

id: osc-52-write-host-request
family: OSC
sequence: OSC 52 write
feature: clipboard write host opt-in
status: supported
action_category: host-policy-request
behavior: emits host request carrying id, selection, decoded payload, and raw size
host_policy: explicit host opt-in required before clipboard mutation
payload_limit: 1048576 raw bytes
recovery: malformed or oversized request denied with diagnostic
reply: no-reply
diagnostic: OSC 52 request diagnostic on failure
oracle: product-decision-vnm-terminal

## osc-52-read-deny

id: osc-52-read-deny
family: OSC
sequence: OSC 52 read
feature: clipboard read denial
status: supported
action_category: host-policy-request
behavior: read requests are denied and no wire reply is sent
host_policy: reads disabled
payload_limit: 1048576 raw bytes
recovery: recover at ST after denial
reply: no-reply
diagnostic: OSC 52 read denied diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1

id: dec-private-1
family: CSI
sequence: DECSET/DECRST ?1
feature: application cursor keys
status: supported
action_category: input-mode-mutation
behavior: toggles application cursor key encoding
host_policy: keyboard input encoder observes mode
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-3

id: dec-private-3
family: CSI
sequence: DECSET/DECRST ?3
feature: 132-column mode
status: ignored
action_category: ignored-with-diagnostic
behavior: no column count change; geometry remains host controlled
host_policy: resize follows item geometry only
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply
diagnostic: ignored DEC private mode diagnostic
oracle: product-decision-vnm-terminal

## csi-window-op-8

id: csi-window-op-8
family: CSI
sequence: CSI 8 ; rows ; columns t
feature: xterm text-area resize request
status: supported
action_category: host-policy-request
behavior: applies the requested grid at the sequence point and notifies the host under the application-controlled policy; ignored with no grid change under the disabled policy; with textAreaResizeArbitrationEnabled a captured sequence commits nothing until the host answers, output after it is held, and the answered grid is applied once, the answer deciding that grid alone and not the effect of a C0 control the captured run carries; a sequence the arbitration does not capture, one carrying an embedded C0 control and split across a backend read boundary after that control, keeps the sequence-point commit and the standing text_area_resize_requested notification
host_policy: textAreaResizePolicy selects application-controlled or disabled; hosts disable it while the window manager owns their geometry; textAreaResizeArbitrationEnabled additionally makes a captured request a two-phase transaction the host answers through respond_text_area_resize, and an arbitrating host connects text_area_resize_requested as well because that signal is all an uncaptured request produces; the window resize a host makes while answering is an ordinary geometry change that reaches the grid and the backend and never settles the request
payload_limit: grid must be within the supported screen model bounds; held output is bounded by the arbitration hold limit
recovery: malformed parameters discard the sequence; unsupported grid or disabled policy leaves the grid unchanged and the parser continues; a refused, timed-out or otherwise settled arbitration replays the held output against the grid it leaves unchanged
reply: no-reply
diagnostic: malformed sequence diagnostic; unsupported sequence diagnostic for a rejected grid, a disabled policy or a refused arbitration
oracle: product-decision-vnm-terminal

## csi-window-op-14

id: csi-window-op-14
family: CSI
sequence: CSI 14 t
feature: xterm text-area pixel size report
status: supported
action_category: terminal-reply
behavior: reports the text area as rows times the cell pixel height and columns times the cell pixel width without changing terminal state; unsupported while no cell pixel size is known
host_policy: the cell pixel size is the surface's cell in device pixels on the POSIX backend and a fixed virtual 10x20 cell on the Windows ConPTY backend, the cell OpenConsole places sixel images on (product-platform-matrix); a session with no reported cell and no fixed backend cell has none
payload_limit: none
recovery: malformed parameters discard the sequence and the parser continues
reply: CSI 4 ; height ; width t text-area pixel size reply through same backend write path
diagnostic: malformed sequence diagnostic; unsupported sequence diagnostic without a cell pixel size and for the CSI 14 ; 2 t window form
oracle: xterm-409-reference

## csi-window-op-16

id: csi-window-op-16
family: CSI
sequence: CSI 16 t
feature: xterm character cell pixel size report
status: supported
action_category: terminal-reply
behavior: reports the cell pixel height and width without changing terminal state; unsupported while no cell pixel size is known
host_policy: the cell pixel size is the surface's cell in device pixels on the POSIX backend and a fixed virtual 10x20 cell on the Windows ConPTY backend, the cell OpenConsole places sixel images on (product-platform-matrix); a session with no reported cell and no fixed backend cell has none
payload_limit: none
recovery: malformed parameters discard the sequence and the parser continues
reply: CSI 6 ; height ; width t cell pixel size reply through same backend write path
diagnostic: malformed sequence diagnostic; unsupported sequence diagnostic without a cell pixel size
oracle: xterm-409-reference

## csi-window-op-18

id: csi-window-op-18
family: CSI
sequence: CSI 18 t
feature: xterm text-area size report
status: supported
action_category: terminal-reply
behavior: reports the current grid without changing terminal state
host_policy: none
payload_limit: none
recovery: malformed parameters discard the sequence and the parser continues
reply: text-area size reply through same backend write path
diagnostic: malformed sequence diagnostic; unsupported sequence diagnostic for other window operations
oracle: xterm-409-reference

## dec-private-5

id: dec-private-5
family: CSI
sequence: DECSET/DECRST ?5
feature: reverse video
status: supported
action_category: mode-mutation
behavior: toggles reverse-video render metadata
host_policy: renderer applies snapshot mode
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-6

id: dec-private-6
family: CSI
sequence: DECSET/DECRST ?6
feature: origin mode
status: supported
action_category: mode-mutation
behavior: toggles cursor addressing relative to scroll region
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-7

id: dec-private-7
family: CSI
sequence: DECSET/DECRST ?7
feature: autowrap
status: supported
action_category: mode-mutation
behavior: toggles wrap at right margin
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-25

id: dec-private-25
family: CSI
sequence: DECSET/DECRST ?25
feature: cursor visibility
status: supported
action_category: mode-mutation
behavior: toggles cursor visibility in snapshots
host_policy: renderer observes cursor visibility
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-47

id: dec-private-47
family: CSI
sequence: DECSET/DECRST ?47
feature: alternate screen
status: supported
action_category: mode-mutation
behavior: switches active screen without primary scrollback mutation
host_policy: alternate-screen wheel policy applies
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1000

id: dec-private-1000
family: CSI
sequence: DECSET/DECRST ?1000
feature: mouse button reporting
status: supported
action_category: input-mode-mutation
behavior: enables button press and release reporting
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1002

id: dec-private-1002
family: CSI
sequence: DECSET/DECRST ?1002
feature: mouse drag reporting
status: supported
action_category: input-mode-mutation
behavior: enables button and drag reporting
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1003

id: dec-private-1003
family: CSI
sequence: DECSET/DECRST ?1003
feature: all-motion mouse reporting
status: supported
action_category: input-mode-mutation
behavior: enables all-motion mouse reporting
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1004

id: dec-private-1004
family: CSI
sequence: DECSET/DECRST ?1004
feature: focus reporting
status: supported
action_category: input-mode-mutation
behavior: enables focus-in and focus-out reports
host_policy: focus reports sent only while mode is active
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-80

id: dec-private-80
family: CSI
sequence: DECSET/DECRST ?80
feature: sixel display mode (DECSDM)
status: supported
action_category: mode-mutation
behavior: set places later sixel images at the page home whatever the origin mode and margins say, never scrolls for them, clips them at the bottom of the page and leaves the cursor where it was; reset, the default, places them at the cursor per dcs-sixel-image; the polarity is xterm's and OpenConsole's, where the VT340 manual's chapter 14 wording reads the other way
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply, 1 set and 2 reset
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1070

id: dec-private-1070
family: CSI
sequence: DECSET/DECRST ?1070
feature: private sixel color registers
status: ignored
action_category: ignored-with-diagnostic
behavior: sixel color registers are always private to each image, so setting or resetting the mode changes nothing
host_policy: none
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply, 3 permanently set
diagnostic: unsupported DEC private mode diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1005

id: dec-private-1005
family: CSI
sequence: DECSET/DECRST ?1005
feature: legacy UTF-8 mouse protocol
status: ignored
action_category: ignored-with-diagnostic
behavior: parsed but does not enable UTF-8 mouse reporting
host_policy: SGR 1006 remains the supported coordinate protocol
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply
diagnostic: ignored mouse protocol diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1006

id: dec-private-1006
family: CSI
sequence: DECSET/DECRST ?1006
feature: SGR mouse protocol
status: supported
action_category: input-mode-mutation
behavior: enables SGR mouse coordinate encoding
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1007

id: dec-private-1007
family: CSI
sequence: DECSET/DECRST ?1007
feature: alternate-scroll mode
status: supported
action_category: input-mode-mutation
behavior: tracks alternate-scroll mode for DECRQM and input-mode snapshots
host_policy: surface alternate-screen wheel policy may still translate wheel events to cursor or page keys after DECRST ?1007
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1015

id: dec-private-1015
family: CSI
sequence: DECSET/DECRST ?1015
feature: urxvt mouse protocol
status: ignored
action_category: ignored-with-diagnostic
behavior: parsed but does not enable urxvt mouse reporting
host_policy: SGR 1006 remains the supported coordinate protocol
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply
diagnostic: ignored mouse protocol diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1047

id: dec-private-1047
family: CSI
sequence: DECSET/DECRST ?1047
feature: alternate screen
status: supported
action_category: mode-mutation
behavior: enters alternate buffer and clears alternate contents
host_policy: alternate-screen scrollback policy applies
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1048

id: dec-private-1048
family: CSI
sequence: DECSET/DECRST ?1048
feature: save and restore cursor
status: supported
action_category: mode-mutation
behavior: saves or restores cursor state
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1049

id: dec-private-1049
family: CSI
sequence: DECSET/DECRST ?1049
feature: alternate screen with cursor save
status: supported
action_category: mode-mutation
behavior: combines cursor save with alternate-screen switch
host_policy: alternate-screen scrollback policy applies
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-2004

id: dec-private-2004
family: CSI
sequence: DECSET/DECRST ?2004
feature: bracketed paste mode
status: supported
action_category: input-mode-mutation
behavior: toggles bracketed paste framing for accepted paste input
host_policy: paste policy and write queue capacity apply
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-2026

id: dec-private-2026
family: CSI
sequence: DECSET/DECRST ?2026
feature: synchronized output
status: supported
action_category: mode-mutation
behavior: coalesces snapshot publication only; parser and screen still mutate
host_policy: render invalidation throttling policy applies
payload_limit: none
recovery: timeout or DECRST exits synchronized output with diagnostic if stale
reply: DECRQM private mode reply
diagnostic: stale synchronized-output diagnostic
oracle: product-decision-vnm-terminal

## dec-private-2027

id: dec-private-2027
family: CSI
sequence: DECSET/DECRST ?2027
feature: grapheme-cluster mode
status: rejected
action_category: rejected-with-recovery
behavior: no runtime grapheme-cluster mode is stored or toggled
host_policy: Unicode width policy remains pinned and mode-independent
payload_limit: none
recovery: command rejected and parser continues
reply: no-reply
diagnostic: rejected grapheme-cluster mode diagnostic
oracle: product-decision-vnm-terminal

## c0-bel

id: c0-bel
family: C0
sequence: BEL
feature: bell
status: supported
action_category: notification
behavior: emits bell request without mutating screen cells
host_policy: audible and visual bell policies apply
payload_limit: none
recovery: none
reply: no-reply
diagnostic: no diagnostic
oracle: product-decision-vnm-terminal

## esc-index

id: esc-index
family: ESC
sequence: ESC D
feature: index
status: supported
action_category: screen-mutation
behavior: moves the cursor down one row, scrolling the active region at the bottom margin
host_policy: top-anchored primary-region scrolling appends host scrollback
payload_limit: none
recovery: unsupported ESC controls continue through normal recovery
reply: no-reply
diagnostic: no diagnostic
oracle: xterm-409-reference

## esc-next-line

id: esc-next-line
family: ESC
sequence: ESC E
feature: next line
status: supported
action_category: screen-mutation
behavior: moves the cursor to column zero of the next row, scrolling the active region at the bottom margin
host_policy: top-anchored primary-region scrolling appends host scrollback
payload_limit: none
recovery: unsupported ESC controls continue through normal recovery
reply: no-reply
diagnostic: no diagnostic
oracle: xterm-409-reference

## esc-reverse-index

id: esc-reverse-index
family: ESC
sequence: ESC M
feature: reverse index
status: supported
action_category: screen-mutation
behavior: moves the cursor up one row or scrolls the active region down at the top margin
host_policy: reverse scrolling does not append host scrollback
payload_limit: none
recovery: unsupported ESC controls continue through normal recovery
reply: no-reply
diagnostic: no diagnostic
oracle: xterm-409-reference

## csi-scroll-up

id: csi-scroll-up
family: CSI
sequence: SU / CSI Ps S
feature: scroll region up
status: supported
action_category: screen-mutation
behavior: scrolls the active scroll region up and blanks vacated bottom rows
host_policy: top-anchored primary regions append scrolled rows to host scrollback
payload_limit: none
recovery: malformed sequence ignored with diagnostic
reply: no-reply
diagnostic: malformed sequence diagnostic
oracle: xterm-409-reference

## csi-scroll-down

id: csi-scroll-down
family: CSI
sequence: SD / CSI Ps T single-parameter form; XTHIMOUSE-shaped multi-parameter CSI T
feature: scroll region down
status: supported
action_category: screen-mutation
behavior: single-parameter CSI Ps T scrolls the active scroll region down and blanks vacated top rows; XTHIMOUSE-shaped multi-parameter CSI T mutates no screen state
host_policy: does not append host scrollback
payload_limit: none
recovery: malformed or unsupported CSI T form ignored with diagnostic
reply: no-reply
diagnostic: malformed sequence diagnostic; unsupported diagnostic for XTHIMOUSE-shaped multi-parameter CSI T
oracle: xterm-409-reference

## csi-decsca

id: csi-decsca
family: CSI
sequence: DECSCA
feature: protected cell attribute
status: ignored
action_category: ignored-with-diagnostic
behavior: no protected-cell model state is stored
host_policy: none
payload_limit: none
recovery: command ignored and parser continues
reply: no-reply
diagnostic: ignored DECSCA diagnostic
oracle: product-decision-vnm-terminal

## bracketed-paste-generated-input

id: bracketed-paste-generated-input
family: generated-input
sequence: bracketed paste wrappers
feature: generated paste input
status: supported
action_category: input-mode-mutation
behavior: accepted paste is wrapped only when bracketed paste mode is active
host_policy: paste capacity reservation must include wrappers and payload
payload_limit: write queue capacity
recovery: reject whole paste if full frame cannot be enqueued
reply: backend write bytes
diagnostic: paste rejection diagnostic
oracle: product-decision-vnm-terminal

## mouse-sgr-1006-generated-input

id: mouse-sgr-1006-generated-input
family: generated-input
sequence: SGR 1006 mouse report
feature: generated mouse input
status: supported
action_category: input-mode-mutation
behavior: sends SGR mouse reports when mode and host policy allow
host_policy: mouse reporting policy may suppress terminal reports
payload_limit: write queue capacity
recovery: out-of-bounds events ignored with diagnostic
reply: backend write bytes
diagnostic: mouse report suppression diagnostic
oracle: product-decision-vnm-terminal

## focus-generated-input

id: focus-generated-input
family: generated-input
sequence: focus in and focus out reports
feature: generated focus input
status: supported
action_category: input-mode-mutation
behavior: sends focus reports only while focus reporting mode is active
host_policy: item focus state controls report source
payload_limit: write queue capacity
recovery: disabled mode suppresses reports without backend write
reply: backend write bytes
diagnostic: no diagnostic
oracle: product-decision-vnm-terminal

## reply-da1

id: reply-da1
family: CSI
sequence: DA1
feature: terminal identity reply
status: supported
action_category: terminal-reply
behavior: emits typed DA1 reply action
host_policy: backend write queue capacity applies
payload_limit: none
recovery: malformed query ignored with diagnostic
reply: DA1 reply through same backend write path
diagnostic: malformed query diagnostic
oracle: product-decision-vnm-terminal

## reply-da2

id: reply-da2
family: CSI
sequence: secondary DA
feature: terminal identity reply
status: supported
action_category: terminal-reply
behavior: emits typed DA2 reply action
host_policy: backend write queue capacity applies
payload_limit: none
recovery: malformed query ignored with diagnostic
reply: DA2 reply through same backend write path
diagnostic: malformed query diagnostic
oracle: product-decision-vnm-terminal

## reply-dsr-cursor-position

id: reply-dsr-cursor-position
family: CSI
sequence: DSR 6
feature: cursor position report
status: supported
action_category: terminal-reply
behavior: emits cursor position report for active buffer cursor
host_policy: backend write queue capacity applies
payload_limit: none
recovery: malformed query ignored with diagnostic
reply: DSR cursor reply through same backend write path
diagnostic: malformed query diagnostic
oracle: product-decision-vnm-terminal

## reply-decrqm-private-mode

id: reply-decrqm-private-mode
family: CSI
sequence: DECRQM private mode query
feature: mode status report
status: supported
action_category: terminal-reply
behavior: emits DECRQM reply for known private modes
host_policy: backend write queue capacity applies
payload_limit: none
recovery: unsupported query gets explicit unsupported reply or diagnostic
reply: DECRQM reply through same backend write path
diagnostic: unsupported query diagnostic
oracle: product-decision-vnm-terminal

## reply-osc-color-query

id: reply-osc-color-query
family: OSC
sequence: OSC color query
feature: color state reply
status: supported
action_category: terminal-reply
behavior: emits typed OSC color reply action for supported color queries
host_policy: backend write queue capacity applies
payload_limit: 1048576 raw bytes
recovery: unsupported color query ignored with diagnostic
reply: OSC color reply through same backend write path
diagnostic: unsupported color query diagnostic
oracle: xterm-409-reference

## reply-unsupported-query-no-reply

id: reply-unsupported-query-no-reply
family: CSI
sequence: unsupported terminal query
feature: unsupported query behavior
status: ignored
action_category: ignored-with-diagnostic
behavior: unsupported query mutates no state and emits no wire reply
host_policy: backend write queue receives nothing
payload_limit: none
recovery: query ignored and parser continues
reply: no-reply
diagnostic: unsupported query diagnostic
oracle: product-decision-vnm-terminal
