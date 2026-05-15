#!/usr/bin/env python3
"""
Render every UI screen of the FC Wängi 1967 Anzeigetafel firmware as a PNG.

Outputs to mockups/{controller,wallbox}/*.png plus an index.html so you can
flip through them like a Figma board.

Run from repo root:
    python3 tools/make_mockups.py
    open mockups/index.html
"""
from __future__ import annotations

import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageOps

# ---------------------------------------------------------------------------
#  Layout / palette — matches config.h on both projects
# ---------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parent.parent
OUT  = ROOT / "mockups"
CREST_SRC = ROOT / "tools" / "crest_src.png"

# Controller is 320x240 landscape, wallbox is 170x320 portrait.
CTRL_W, CTRL_H = 320, 240
WB_W, WB_H     = 170, 320
HEADER_H       = 28

# Palette (hex strings for PIL — RGB565 doesn't apply at the Python level).
C_PRIMARY = "#be1c1c"
C_ACCENT  = "#fdc500"
C_BG      = "#0a0a0a"
C_TEXT    = "#ffffff"
C_DIM     = "#888888"
C_SUCCESS = "#44dd44"
C_WARN    = "#ffaa00"
C_ERROR   = "#ff3333"
C_PANEL   = "#1a1a1a"

# macOS font paths.
FONT_BOLD       = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FONT_REGULAR    = "/System/Library/Fonts/Supplemental/Arial.ttf"
FONT_BLACK      = "/System/Library/Fonts/Supplemental/Arial Black.ttf"

def font(size: int, bold: bool = False, black: bool = False) -> ImageFont.FreeTypeFont:
    path = FONT_BLACK if black else (FONT_BOLD if bold else FONT_REGULAR)
    return ImageFont.truetype(path, size)


# ---------------------------------------------------------------------------
#  Canvas + primitives
# ---------------------------------------------------------------------------
def canvas(w: int, h: int) -> Image.Image:
    return Image.new("RGB", (w, h), C_BG)


def text_size(d: ImageDraw.ImageDraw, text: str, fnt: ImageFont.FreeTypeFont) -> tuple[int, int]:
    bbox = d.textbbox((0, 0), text, font=fnt)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def centred(d: ImageDraw.ImageDraw, cx: int, y: int, text: str, fnt: ImageFont.FreeTypeFont, color: str):
    """Top-centred — anchor is the middle-top of the text bounding box."""
    d.text((cx, y), text, fill=color, font=fnt, anchor="mt")


def vcentred(d: ImageDraw.ImageDraw, cx: int, cy: int, text: str, fnt: ImageFont.FreeTypeFont, color: str):
    """Middle-centred — anchor is the visual middle of the text glyph."""
    d.text((cx, cy), text, fill=color, font=fnt, anchor="mm")


def header(img: Image.Image, title_left: str, title_right: str = "", *, with_crest: bool = True):
    """
    Header layout (right→left so the icons can claim their fixed strip):
      [crest 20px] title_left          title_right [bars 12px] [battery 18px]
    The right-edge icon strip is fixed at 38px (bars + gap + battery).
    Title_right gets everything between title_left and that strip.
    """
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, HEADER_H], fill=C_PRIMARY)
    if with_crest:
        crest = _crest_small()
        if crest:
            img.paste(crest, (6, (HEADER_H - crest.height) // 2), crest)
    x_left = 28 if with_crest else 6
    vcentred(d, x_left + 80, HEADER_H // 2, title_left, font(12, bold=True), C_TEXT)
    # Status icons claim the right 38px.
    icon_strip_x = img.width - 40
    _link_icon(d, icon_strip_x, 8, linked=True)
    _battery_icon(d, img.width - 22, 9, level=87)
    if title_right:
        # End the right-side title 6px before the icon strip starts.
        end_x = icon_strip_x - 6
        bbox = d.textbbox((0, 0), title_right, font=font(10))
        tw = bbox[2] - bbox[0]
        d.text((end_x - tw, (HEADER_H - (bbox[3] - bbox[1])) // 2),
               title_right, fill=C_TEXT, font=font(10))


def _link_icon(d: ImageDraw.ImageDraw, x: int, y: int, linked: bool):
    """Three signal bars, no RSSI number — keeps the header strip narrow."""
    color = C_SUCCESS if linked else C_WARN
    for i in range(3):
        h = 2 + i * 2
        d.rectangle([x + i * 4, y + 8 - h, x + i * 4 + 3, y + 8], fill=color)


def _battery_icon(d: ImageDraw.ImageDraw, x: int, y: int, level: int = 87):
    color = C_ERROR if level < 20 else C_TEXT
    d.rectangle([x, y, x + 15, y + 7], outline=color)
    d.rectangle([x + 16, y + 2, x + 17, y + 5], fill=color)
    fill = (level * 13) // 100
    if fill > 0:
        d.rectangle([x + 1, y + 1, x + 1 + fill, y + 6], fill=color)


def button(d: ImageDraw.ImageDraw, x: int, y: int, w: int, h: int,
           label: str, bg: str, fg: str, *, large: bool = False, border: bool = True):
    d.rounded_rectangle([x, y, x + w, y + h], radius=6, fill=bg,
                        outline=fg if border else None)
    f = font(18 if large else 12, bold=True)
    vcentred(d, x + w // 2, y + h // 2, label, f, fg)
    return (x, y, w, h)


def score_block(d: ImageDraw.ImageDraw, cx: int, cy: int, value: int, color: str):
    """Big bold score numeral, scoreboard-style. Wrap warning is shown
    once per screen (not per side) — see _draw_board_wrap_warning."""
    text = str(value)
    f = font(72, bold=True)
    vcentred(d, cx, cy, text, f, color)


def _state_label_with_wrap(base: str, home: int, away: int) -> tuple[str, str]:
    """If either score exceeds 9, fold a 'Tafel X:Y' note into the state
    label and switch the colour to warn — keeps everything on one line."""
    if home > 9 or away > 9:
        return f"{base}  ·  Tafel {home % 10}:{away % 10}", C_WARN
    return base, ""  # caller picks default colour


def clock_text(d: ImageDraw.ImageDraw, cx: int, cy: int, total_sec: int, color: str, *, big: bool = True):
    mm = (total_sec // 60) % 100
    ss = total_sec % 60
    s = f"{mm:02d}:{ss:02d}"
    f = font(48 if big else 28, bold=True)
    vcentred(d, cx, cy, s, f, color)


# ---------------------------------------------------------------------------
#  Crest loader
# ---------------------------------------------------------------------------
_crest_cache: dict[int, Image.Image] = {}

def _crest_for(size: int) -> Image.Image | None:
    if not CREST_SRC.exists():
        return None
    if size not in _crest_cache:
        im = Image.open(CREST_SRC).convert("RGBA")
        im = im.resize((size, size), Image.LANCZOS)
        _crest_cache[size] = im
    return _crest_cache[size]


def _crest_small() -> Image.Image | None:
    return _crest_for(20)


# ---------------------------------------------------------------------------
#  Controller screens (320x240 landscape)
# ---------------------------------------------------------------------------
def ctrl_splash() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    crest = _crest_for(72)
    if crest:
        img.paste(crest, ((CTRL_W - 72) // 2, 16), crest)
    centred(d, CTRL_W // 2, 100, "FC Wängi 1967", font(18, bold=True), C_TEXT)
    centred(d, CTRL_W // 2, 128, "Anzeigetafel-System", font(13), C_ACCENT)
    centred(d, CTRL_W // 2, 152, "v1.0.0  ·  seit 1967", font(9), C_DIM)
    # Progress bar
    bx, by, bw, bh = 40, 200, 240, 10
    d.rounded_rectangle([bx, by, bx + bw, by + bh], radius=4, outline=C_DIM)
    d.rounded_rectangle([bx + 2, by + 2, bx + 150, by + bh - 2], radius=3, fill=C_PRIMARY)
    return img


def ctrl_setup() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "SETUP")
    d.text((10, 36), "MATCH KONFIGURIEREN", fill=C_TEXT, font=font(13, bold=True))
    d.text((10, 64), "Spieltyp:", fill=C_DIM, font=font(10))
    button(d, 10, 78, CTRL_W - 20, 32, "1. Mannschaft", C_BG, C_TEXT)
    centred(d, CTRL_W // 2, 116, "Aktive · 2 × 45 Min · 2. Liga", font(10), C_ACCENT)
    d.text((10, 140), "Heim:", fill=C_DIM, font=font(10))
    d.text((70, 138), "FC Wängi 1", fill=C_TEXT, font=font(13, bold=True))
    d.text((10, 165), "Gegner:", fill=C_DIM, font=font(10))
    button(d, 70, 160, CTRL_W - 80, 26, "FC Tobel-Affeltrangen", C_BG, C_TEXT)
    button(d, 10, 198, 200, 36, "MATCH STARTEN >", C_PRIMARY, C_TEXT, large=True)
    button(d, 220, 198, CTRL_W - 230, 36, "Countdown", C_BG, C_ACCENT)
    return img


def ctrl_setup_preset_picker() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "SPIELTYP")
    presets = [
        "1. Mannschaft",
        "2. / 3. Mannschaft",
        "Senioren 30+ / 40+",
        "B-Junioren",
        "Ca-Junioren",
    ]
    for i, label in enumerate(presets):
        y = 36 + i * 36
        button(d, 10, y, CTRL_W - 60, 32, label, C_BG, C_TEXT)
    button(d, CTRL_W - 44, 36, 36, 32, "^", C_DIM, C_TEXT)
    button(d, CTRL_W - 44, 72, 36, 32, "v", C_DIM, C_TEXT)
    button(d, 10, 220, CTRL_W - 20, 16, "Abbrechen", C_BG, C_WARN)
    return img


def ctrl_setup_keyboard() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "Gegner-Name")
    # Edit field
    d.rounded_rectangle([10, 36, CTRL_W - 10, 66], radius=4, fill="#2c2c2c", outline=C_ACCENT)
    d.text((16, 42), "FC TOBEL-AFFELTRANGEN", fill=C_TEXT, font=font(15, bold=True))
    rows = [
        list("ABCDEFGHIJ"),
        list("KLMNOPQRST"),
        ["U", "V", "W", "X", "Y", "Z", "Ä", "Ö", "Ü", "ß"],
        ["-", ".", ",", "1", "2", "3", "4", "5", "6", " "],
    ]
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            x = 8 + c * 30
            y = 72 + r * 26
            button(d, x, y, 28, 24, ch if ch != " " else "␣", C_BG, C_TEXT)
    button(d, 10, 180, 90, 28, "<-", C_BG, C_WARN)
    button(d, 115, 180, 90, 28, "leeren", C_BG, C_WARN)
    button(d, 220, 180, 90, 28, "OK", C_SUCCESS, C_BG)
    return img


def _match_top_chips(d: ImageDraw.ImageDraw, stoppage: int, show_undo: bool):
    """Stoppage top-right (right of GAST label), undo top-left (left of HEIM label)."""
    button(d, CTRL_W - 38, 32, 32, 16, f"+{stoppage}", C_BG,
           C_ACCENT if stoppage > 0 else C_DIM)
    if show_undo:
        button(d, 6, 32, 28, 16, "<<", C_BG, C_ACCENT)


def ctrl_match(*, running: bool, home: int = 3, away: int = 1, clock_s: int = 23 * 60 + 45,
               stoppage: int = 0, show_undo: bool = False, label: str | None = None) -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "LÄUFT" if running else "PAUSIERT")
    _match_top_chips(d, stoppage, show_undo)
    # Symmetric: HEIM at x=60, GAST at x=260 (both 60 px from their side).
    centred(d, 60, 50, "HEIM", font(11), C_DIM)
    centred(d, 260, 50, "GAST", font(11), C_DIM)
    score_block(d, 60, 100, home, C_TEXT)
    score_block(d, 260, 100, away, C_TEXT)
    # +/- mirrored around centre. Clock fits in the middle gap (x=110-210).
    button(d, 10, 134, 40, 30, "−", C_BG, C_WARN)
    button(d, 70, 134, 40, 30, "+", C_BG, C_SUCCESS)
    button(d, 210, 134, 40, 30, "−", C_BG, C_WARN)
    button(d, 270, 134, 40, 30, "+", C_BG, C_SUCCESS)
    # Clock
    clock_text(d, CTRL_W // 2, 152, clock_s, C_ACCENT if running else C_DIM, big=False)
    if running:
        d.ellipse([CTRL_W // 2 - 3, 170, CTRL_W // 2 + 3, 176], fill=C_SUCCESS)
    state_label = label or ("1. HALBZEIT · LÄUFT" if running else "1. HALBZEIT · PAUSIERT")
    centred(d, CTRL_W // 2, 180, state_label, font(9), C_SUCCESS if running else C_WARN)
    # Bottom buttons
    if running:
        button(d, 10, 198, 140, 36, "PAUSE", C_WARN, C_BG, large=True)
    else:
        button(d, 10, 198, 140, 36, "WEITER", C_SUCCESS, C_BG, large=True)
    button(d, 170, 198, 140, 36, "HALBZEIT", C_PRIMARY, C_TEXT, large=True)
    return img


def ctrl_match_overflow_score() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "LÄUFT")
    _match_top_chips(d, 0, False)
    centred(d, 60, 50, "HEIM", font(11), C_DIM)
    centred(d, 260, 50, "GAST", font(11), C_DIM)
    score_block(d, 60, 100, 10, C_TEXT)
    score_block(d, 260, 100, 3, C_TEXT)
    button(d, 10, 134, 40, 30, "−", C_BG, C_WARN)
    button(d, 70, 134, 40, 30, "+", C_BG, C_SUCCESS)
    button(d, 210, 134, 40, 30, "−", C_BG, C_WARN)
    button(d, 270, 134, 40, 30, "+", C_BG, C_SUCCESS)
    clock_text(d, CTRL_W // 2, 152, 79 * 60 + 12, C_ACCENT, big=False)
    state_text, override = _state_label_with_wrap("2. HALBZEIT · LÄUFT", 10, 3)
    centred(d, CTRL_W // 2, 180, state_text, font(9), override or C_SUCCESS)
    button(d, 10, 198, 140, 36, "PAUSE", C_WARN, C_BG, large=True)
    button(d, 170, 198, 140, 36, "MATCH ENDE", C_PRIMARY, C_TEXT, large=True)
    return img


def ctrl_match_connection_lost() -> Image.Image:
    img = ctrl_match(running=True)
    d = ImageDraw.Draw(img)
    d.rectangle([0, HEADER_H, CTRL_W, HEADER_H + 16], fill=C_WARN)
    centred(d, CTRL_W // 2, HEADER_H + 1, "FUNK VERLOREN", font(11, bold=True), C_BG)
    return img


def ctrl_numpad() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "HEIM-Stand")
    # value display
    d.rounded_rectangle([40, 32, CTRL_W - 40, 68], radius=6, fill="#181818", outline=C_ACCENT)
    vcentred(d, CTRL_W // 2, 50, "3", font(22, bold=True), C_TEXT)
    # Compact 4x3 grid, horizontally centred on the canvas.
    kw, kh, gap = 58, 26, 4
    grid_w = 3 * kw + 2 * gap
    kx0 = (CTRL_W - grid_w) // 2
    ky0 = 76
    digits = "123456789"
    for i, ch in enumerate(digits):
        r, c = divmod(i, 3)
        button(d, kx0 + c * (kw + gap), ky0 + r * (kh + gap), kw, kh, ch, C_BG, C_TEXT)
    button(d, kx0,               ky0 + 3 * (kh + gap), kw, kh, "C",   C_BG, C_WARN)
    button(d, kx0 + (kw + gap),  ky0 + 3 * (kh + gap), kw, kh, "0",   C_BG, C_TEXT)
    # "<-" instead of U+25C0 — that arrow glyph isn't in Arial Bold.
    button(d, kx0 + 2*(kw+gap),  ky0 + 3 * (kh + gap), kw, kh, "<-",  C_BG, C_WARN)
    # Action row well clear of the keypad.
    button(d, 10, 210, 130, 24, "Abbrechen", C_BG, C_WARN)
    button(d, CTRL_W - 140, 210, 130, 24, "OK", C_SUCCESS, C_BG)
    return img


def ctrl_halftime() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "HALBZEIT")
    centred(d, CTRL_W // 2, 36, "ENDSTAND 1. HALBZEIT", font(13, bold=True), C_ACCENT)
    score_block(d, 60, 92, 3, C_TEXT)
    score_block(d, 260, 92, 1, C_TEXT)
    clock_text(d, CTRL_W // 2, 92, 45 * 60 + 23, C_DIM, big=False)
    centred(d, CTRL_W // 2, 136, "Pausenzeit", font(10), C_DIM)
    centred(d, CTRL_W // 2, 150, "12:34", font(28, bold=True), C_TEXT)
    button(d, 10, 198, 150, 36, "2.HZ ab 45:00", C_DIM, C_TEXT)
    button(d, 170, 198, 140, 36, "2.HZ STARTEN", C_PRIMARY, C_TEXT, large=True)
    return img


def ctrl_ended(draw_state: bool) -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "ENDSTAND")
    h_s, a_s = (2, 2) if draw_state else (3, 1)
    score_block(d, 60, 78, h_s, C_TEXT)
    score_block(d, 260, 78, a_s, C_TEXT)
    clock_text(d, CTRL_W // 2, 78, 92 * 60 + 18, C_DIM, big=False)
    centred(d, CTRL_W // 2, 130, "FC Wängi 1  vs.  FC Tobel-Affeltrangen", font(10), C_DIM)
    button(d, 10, 150, 150, 32, "Neues Match", C_BG, C_TEXT)
    button(d, 170, 150, 140, 32, "Tafel löschen", C_BG, C_WARN)
    enabled = draw_state
    button(d, 10, 188, 150, 32, "Verlängerung",
           C_PRIMARY if enabled else C_BG,
           C_TEXT if enabled else C_DIM)
    button(d, 170, 188, 140, 32, "Penaltys",
           C_PRIMARY if enabled else C_BG,
           C_TEXT if enabled else C_DIM)
    return img


def ctrl_penalty_toss() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "PENALTY-TOSS")
    centred(d, CTRL_W // 2, 44, "WER SCHIESST ZUERST?",
            font(16, bold=True), C_TEXT)
    centred(d, CTRL_W // 2, 76, "Münze werfen, dann hier auswählen",
            font(9), C_DIM)
    button(d, 20,  100, 130, 80, "HEIM", C_SUCCESS, C_BG, large=True)
    button(d, 170, 100, 130, 80, "GAST", C_PRIMARY, C_TEXT, large=True)
    button(d, 10, 200, CTRL_W - 20, 30, "< Abbrechen", C_BG, C_WARN)
    return img


def ctrl_penalty() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "PENALTYS")

    # Compact scoreline at the top — 4 — 3, with labels below.
    f_score = font(48, bold=True)
    vcentred(d, CTRL_W // 2 - 60, 64, "4", f_score, C_TEXT)
    vcentred(d, CTRL_W // 2,      64, "—", font(28, bold=True), C_DIM)
    vcentred(d, CTRL_W // 2 + 60, 64, "3", f_score, C_TEXT)
    centred(d, CTRL_W // 2 - 60, 96, "HEIM", font(10), C_DIM)
    centred(d, CTRL_W // 2 + 60, 96, "GAST", font(10), C_DIM)

    # Kick rows — label inline with the boxes, all uniform.
    def kick_row(y: int, label: str, kicks: list, active: bool):
        # Side label, fixed left padding
        centred(d, 32, y + 4, label, font(10, bold=True),
                C_ACCENT if active else C_DIM)
        if active:
            d.text((58, y + 1), "►", fill=C_ACCENT, font=font(12, bold=True))
        for i, scored in enumerate(kicks):
            cx = 80 + i * 32
            # outline
            d.rounded_rectangle([cx, y, cx + 26, y + 26], radius=3, outline=C_DIM)
            if scored is None:
                # Not yet taken — gray-dotted placeholder so size matches.
                d.rounded_rectangle([cx + 2, y + 2, cx + 24, y + 24], radius=2, fill="#222222")
                vcentred(d, cx + 13, y + 13, "·", font(16, bold=True), C_DIM)
            else:
                fill = C_SUCCESS if scored else C_ERROR
                d.rounded_rectangle([cx + 2, y + 2, cx + 24, y + 24], radius=2, fill=fill)
                vcentred(d, cx + 13, y + 13, "+" if scored else "X",
                         font(13, bold=True), C_TEXT)

    kick_row(120, "HEIM",
             [True, False, True, True, True], active=False)
    kick_row(154, "GAST",
             [True, True, False, True, None], active=True)

    # Action row: three equal-width buttons.
    bw, gap = 96, 6
    bx = 10
    button(d, bx,                198, bw, 30, "Tor +",         C_SUCCESS, C_BG)
    button(d, bx + bw + gap,     198, bw, 30, "Daneben X",     C_ERROR,   C_BG)
    button(d, bx + 2*(bw + gap), 198, bw, 30, "ENDE",          C_PRIMARY, C_TEXT)
    return img


def ctrl_settings() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "EINSTELL.")
    d.text((8, 36), "Funk:", fill=C_DIM, font=font(10))
    d.text((48, 36), "OK  78:21:84:7F:00:42  -42dBm", fill=C_SUCCESS, font=font(10))
    button(d, 8,   60, 150, 30, "Neu koppeln",       C_BG, C_TEXT)
    button(d, 162, 60, 150, 30, "Polaritäts-Test",   C_BG, C_TEXT)
    button(d, 8,   96, 150, 30, "Segment-Übung",     C_BG, C_TEXT)
    button(d, 162, 96, 150, 30, "Tafel löschen",     C_BG, C_WARN)
    button(d, 8,  132, 150, 30, "Match-Verlauf",     C_BG, C_TEXT)
    button(d, 162, 132, 150, 30, "Vorgaben",         C_BG, C_TEXT)
    button(d, 8,  168, CTRL_W - 16, 24,
           "Werkseinstellung (löscht alles)", C_BG, C_ERROR)
    d.text((8, 196), "Akku 87%  v1.0.0  MAC 78:21:84:7F:00:42",
           fill=C_DIM, font=font(8))
    button(d, 10, 210, CTRL_W - 20, 24, "< ZURÜCK", C_PRIMARY, C_TEXT)
    return img


def ctrl_history() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "VERLAUF")
    items = [
        ("3-1 vs FC Tobel-Affeltrangen",  "1. Mannschaft · 92:18"),
        ("2-2 vs FC Bichelsee-Balterswil", "1. Mannschaft · 91:04"),
        ("5-0 vs FC Aadorf",               "1. Mannschaft · 90:00"),
        ("1-1 vs FC Sirnach b",            "2. Mannschaft · 90:00"),
        ("4-2 vs FC Münchwilen",           "Senioren 30+ · 90:00"),
    ]
    for i, (line1, line2) in enumerate(items):
        y = HEADER_H + 6 + i * 32
        d.rounded_rectangle([8, y, CTRL_W - 60, y + 28], radius=4,
                            fill=C_PANEL, outline=C_DIM)
        d.text((14, y + 4), line1, fill=C_TEXT, font=font(10))
        d.text((14, y + 16), line2, fill=C_DIM, font=font(9))
    button(d, CTRL_W - 44, 36, 36, 32, "^", C_DIM, C_TEXT)
    button(d, CTRL_W - 44, 72, 36, 32, "v", C_DIM, C_TEXT)
    button(d, 10, 210, 140, 26, "Verlauf löschen", C_BG, C_WARN)
    button(d, 170, 210, 140, 26, "< Zurück", C_PRIMARY, C_TEXT)
    return img


def ctrl_defaults() -> Image.Image:
    img = canvas(CTRL_W, CTRL_H)
    d = ImageDraw.Draw(img)
    header(img, "FC Wängi 1967", "VORGABEN")

    def row(y_baseline: int, label: str, value: str, *, with_buttons: bool, toggle: bool = False, toggle_on: bool = False):
        # Label is left-aligned, vertically centred at y_baseline.
        d.text((10, y_baseline - 8), label, fill=C_TEXT, font=font(12, bold=True))
        if with_buttons:
            button(d, 158, y_baseline - 13, 26, 24, "−", C_BG, C_WARN)
            vcentred(d, 232, y_baseline, value, font(13, bold=True), C_ACCENT)
            button(d, 282, y_baseline - 13, 26, 24, "+", C_BG, C_SUCCESS)
        elif toggle:
            label_text = "AN" if toggle_on else "AUS"
            color_bg = C_SUCCESS if toggle_on else C_DIM
            button(d, 200, y_baseline - 13, 110, 24, label_text, color_bg, C_BG)

    row( 60, "Halbzeit-Länge:",     "45 min", with_buttons=True)
    row( 98, "Halbzeitpause:",      "15 min", with_buttons=True)
    row(140, "Auto-löschen:",       "",       with_buttons=False, toggle=True, toggle_on=True)
    row(178, "Torschütze fragen:",  "",       with_buttons=False, toggle=True, toggle_on=False)

    button(d, 10, 208, CTRL_W - 20, 28, "< Speichern + Zurück", C_PRIMARY, C_TEXT)
    return img


# ---------------------------------------------------------------------------
#  Wallbox screens (170x320 portrait)
# ---------------------------------------------------------------------------
def wb_header(img: Image.Image, sub: str = "TAFEL"):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 36], fill=C_PRIMARY)
    centred(d, img.width // 2, 4, "FC WÄNGI 1967", font(11, bold=True), C_TEXT)
    centred(d, img.width // 2, 20, sub, font(9), C_TEXT)


def wb_footer(img: Image.Image, *, polarity_ok: bool, linked: bool, rssi: int, tx_active: bool):
    d = ImageDraw.Draw(img)
    y = 250
    d.text((6, y), f"Pol: {'OK' if polarity_ok else '?'}",
           fill=C_SUCCESS if polarity_ok else C_WARN, font=font(9))
    y += 12
    if linked:
        d.text((6, y), f"Funk: {rssi} dBm", fill=C_SUCCESS, font=font(9))
    else:
        d.text((6, y), "Funk: --", fill=C_DIM, font=font(9))
    y += 12
    d.text((6, y), f"TX: {'(blinkt)' if tx_active else '(idle)'}",
           fill=C_ACCENT if tx_active else C_DIM, font=font(9))
    d.text((6, 304), "v1.0.0  IO14=Menu", fill=C_DIM, font=font(8))


def wb_boot() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    crest = _crest_for(60)
    if crest:
        img.paste(crest, ((WB_W - 60) // 2, 56), crest)
    centred(d, WB_W // 2, 140, "Startet...", font(20, bold=True), C_ACCENT)
    centred(d, WB_W // 2, 180, ". . .", font(20, bold=True), C_DIM)
    wb_footer(img, polarity_ok=False, linked=False, rssi=-127, tx_active=False)
    return img


def wb_pairing() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    centred(d, WB_W // 2, 80, "Warte auf", font(12), C_TEXT)
    centred(d, WB_W // 2, 100, "Controller", font(12), C_TEXT)
    centred(d, WB_W // 2, 140, "·funk··", font(20, bold=True), C_ACCENT)
    centred(d, WB_W // 2, 200, "Halte Boot-", font(8), C_DIM)
    centred(d, WB_W // 2, 212, "Knopf zum Neu", font(8), C_DIM)
    centred(d, WB_W // 2, 224, "Pairen", font(8), C_DIM)
    wb_footer(img, polarity_ok=False, linked=False, rssi=-127, tx_active=False)
    return img


def wb_paired_idle() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    centred(d, WB_W // 2, 80, "Bereit für", font(13), C_TEXT)
    centred(d, WB_W // 2, 102, "Match", font(13), C_TEXT)
    centred(d, WB_W // 2, 160, "78:21:84:7F:00:42", font(8), C_DIM)
    centred(d, WB_W // 2, 180, "-42 dBm", font(8), C_SUCCESS)
    wb_footer(img, polarity_ok=True, linked=True, rssi=-42, tx_active=False)
    return img


def wb_match_live() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    centred(d, 40, 44, "HEIM", font(12), C_DIM)
    centred(d, 130, 44, "GAST", font(12), C_DIM)
    # Big scores
    vcentred(d, 40, 100, "3", font(56, bold=True), C_TEXT)
    vcentred(d, 130, 100, "1", font(56, bold=True), C_TEXT)
    # Divider
    d.line([10, 152, WB_W - 10, 152], fill=C_DIM)
    # Clock
    vcentred(d, WB_W // 2, 188, "23:45", font(36, bold=True), C_ACCENT)
    d.ellipse([WB_W // 2 - 3, 214, WB_W // 2 + 3, 220], fill=C_SUCCESS)
    centred(d, WB_W // 2, 234, "1. HALBZEIT", font(11, bold=True), C_TEXT)
    wb_footer(img, polarity_ok=True, linked=True, rssi=-42, tx_active=True)
    return img


def wb_polarity_test() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    centred(d, WB_W // 2, 60, "POLARITÄTS-", font(13, bold=True), C_PRIMARY)
    centred(d, WB_W // 2, 78, "TEST", font(13, bold=True), C_PRIMARY)
    centred(d, WB_W // 2, 120, "Alle 6 Ziffern", font(12), C_TEXT)
    centred(d, WB_W // 2, 140, "als 8 sichtbar?", font(12), C_TEXT)
    centred(d, WB_W // 2, 180, "IO14 kurz = JA", font(9), C_SUCCESS)
    centred(d, WB_W // 2, 200, "IO14 lang = NEIN", font(9), C_WARN)
    centred(d, WB_W // 2, 230, "(NEIN = Wallbox", font(8), C_DIM)
    centred(d, WB_W // 2, 242, "180° drehen)", font(8), C_DIM)
    wb_footer(img, polarity_ok=False, linked=True, rssi=-42, tx_active=True)
    return img


def wb_segment_exercise() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    centred(d, WB_W // 2, 100, "Wartung", font(20, bold=True), C_ACCENT)
    centred(d, WB_W // 2, 140, "Segment-", font(13, bold=True), C_TEXT)
    centred(d, WB_W // 2, 160, "Übung", font(13, bold=True), C_TEXT)
    centred(d, WB_W // 2, 200, "30 Sek.", font(13), C_DIM)
    wb_footer(img, polarity_ok=True, linked=True, rssi=-42, tx_active=True)
    return img


def wb_connection_lost() -> Image.Image:
    img = canvas(WB_W, WB_H)
    wb_header(img)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 60, WB_W, 110], fill=C_WARN)
    centred(d, WB_W // 2, 65, "FUNK", font(22, bold=True), C_BG)
    centred(d, WB_W // 2, 92, "VERLOREN", font(13, bold=True), C_BG)
    centred(d, WB_W // 2, 150, "Controller-", font(13, bold=True), C_TEXT)
    centred(d, WB_W // 2, 170, "Verbindung", font(13, bold=True), C_TEXT)
    centred(d, WB_W // 2, 190, "weg", font(13, bold=True), C_TEXT)
    wb_footer(img, polarity_ok=True, linked=False, rssi=-127, tx_active=False)
    return img


# ---------------------------------------------------------------------------
#  Main: render all, write HTML index
# ---------------------------------------------------------------------------
CONTROLLER_SCREENS = [
    ("01_splash",                 "Splash / boot",                    ctrl_splash),
    ("02_setup",                  "Match setup",                      ctrl_setup),
    ("03_setup_preset_picker",    "Setup → preset picker",            ctrl_setup_preset_picker),
    ("04_setup_keyboard",         "Setup → opponent keyboard",        ctrl_setup_keyboard),
    ("05_match_live",             "Live match (clock running)",       lambda: ctrl_match(running=True)),
    ("06_match_paused",           "Live match (paused)",              lambda: ctrl_match(running=False)),
    ("07_match_stoppage",         "Live match w/ stoppage time set",
        lambda: ctrl_match(running=True, clock_s=46 * 60 + 12, stoppage=3, label="1. HALBZEIT · LÄUFT")),
    ("08_match_undo_available",   "Live match w/ undo available",
        lambda: ctrl_match(running=True, show_undo=True)),
    ("09_match_overflow_score",   "Score > 9 (board wrap warning)",  ctrl_match_overflow_score),
    ("10_match_link_lost",        "Live match · connection lost",     ctrl_match_connection_lost),
    ("11_numpad",                 "Numeric pad popup",                ctrl_numpad),
    ("12_halftime",               "Halftime (countdown)",             ctrl_halftime),
    ("13_ended_draw",             "Match ended · draw (extra options)", lambda: ctrl_ended(draw_state=True)),
    ("14_ended_decided",          "Match ended · decided",            lambda: ctrl_ended(draw_state=False)),
    ("15a_penalty_toss",          "Penalty — who shoots first?",      ctrl_penalty_toss),
    ("15_penalty",                "Penalty shootout",                 ctrl_penalty),
    ("16_settings",               "Settings",                         ctrl_settings),
    ("17_history",                "Match history (scrollable)",       ctrl_history),
    ("18_defaults",               "Match defaults editor",            ctrl_defaults),
]

WALLBOX_SCREENS = [
    ("01_boot",               "Boot",                       wb_boot),
    ("02_pairing",            "Pairing — waiting",          wb_pairing),
    ("03_paired_idle",        "Paired — bereit",            wb_paired_idle),
    ("04_match_live",         "Match live",                 wb_match_live),
    ("05_polarity_test",      "Polarity test (first boot)", wb_polarity_test),
    ("06_segment_exercise",   "Segment exercise (wartung)", wb_segment_exercise),
    ("07_connection_lost",    "Connection lost",            wb_connection_lost),
]


def write_index_html(controller_files: list[tuple[str, str]],
                     wallbox_files: list[tuple[str, str]]):
    def card(rel: str, title: str, w: int, h: int) -> str:
        # Display at 2x for legibility on retina screens.
        return (
            f'<figure>'
            f'<img src="{rel}" width="{w*2}" height="{h*2}" '
            f'style="image-rendering:pixelated;background:#0a0a0a;border:1px solid #333">'
            f'<figcaption>{title}</figcaption>'
            f'</figure>'
        )
    ctrl_cards = "\n".join(card(f"controller/{n}.png", t, CTRL_W, CTRL_H)
                           for n, t in controller_files)
    wb_cards = "\n".join(card(f"wallbox/{n}.png", t, WB_W, WB_H)
                         for n, t in wallbox_files)
    html = f"""<!doctype html>
<html lang="de"><head>
<meta charset="utf-8">
<title>FC Wängi 1967 — Anzeigetafel UI Mockups</title>
<style>
  body {{ font-family:-apple-system,Segoe UI,sans-serif; background:#0a0a0a; color:#fff;
         margin:0; padding:32px; }}
  header {{ background:#be1c1c; padding:16px 24px; border-radius:8px; margin-bottom:32px; }}
  h1 {{ margin:0; letter-spacing:.05em; }}
  h2 {{ margin-top:48px; color:#fdc500; letter-spacing:.04em;
        border-bottom:1px solid #333; padding-bottom:8px; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fill, minmax(680px, 1fr)); gap:32px; }}
  .grid.wb {{ grid-template-columns:repeat(auto-fill, minmax(360px, 1fr)); }}
  figure {{ margin:0; text-align:center; }}
  figcaption {{ margin-top:10px; color:#888; font-size:.9rem; }}
  footer {{ margin-top:48px; color:#666; font-size:.8rem; text-align:center; }}
</style></head><body>
<header><h1>FC Wängi 1967 — Anzeigetafel UI Mockups</h1></header>

<h2>Controller (M5Stack Core2 · 320×240 landscape)</h2>
<div class="grid">{ctrl_cards}</div>

<h2>Wall-box (LilyGO T-Display-S3 · 170×320 portrait)</h2>
<div class="grid wb">{wb_cards}</div>

<footer>
  Generated by tools/make_mockups.py · WÄNGI · 1967 NEVER GIVE UP
</footer>
</body></html>
"""
    (OUT / "index.html").write_text(html, encoding="utf-8")


def main() -> int:
    OUT.mkdir(exist_ok=True)
    (OUT / "controller").mkdir(exist_ok=True)
    (OUT / "wallbox").mkdir(exist_ok=True)

    ctrl_done = []
    for name, title, fn in CONTROLLER_SCREENS:
        img = fn()
        path = OUT / "controller" / f"{name}.png"
        img.save(path)
        ctrl_done.append((name, title))
        print(f"  {path.relative_to(ROOT)}")

    wb_done = []
    for name, title, fn in WALLBOX_SCREENS:
        img = fn()
        path = OUT / "wallbox" / f"{name}.png"
        img.save(path)
        wb_done.append((name, title))
        print(f"  {path.relative_to(ROOT)}")

    write_index_html(ctrl_done, wb_done)
    print(f"\nIndex: {(OUT / 'index.html').relative_to(ROOT)}")
    print(f"Open with:  open mockups/index.html")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
