"""Convert the Flappy Beans sprites in assets/flappy/ into src/ui/flappy_img.c.

Sprites with transparency become ARGB8888 (LVGL can rotate those, which the
bean needs); the opaque background and ground become RGB565. Some sprites are
cropped or scaled to fit the 320x240 landscape screen, see SPRITES.
Regenerate after changing a sprite or a size here:

    uv run --with pillow python3 tools/flappy_to_lvgl.py
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "flappy"
OUT = ROOT / "src" / "ui" / "flappy_img.c"

SCREEN_W = 320
FIELD_H = 216  # play field above the ground; must match FlappyConfig::height
GROUND_H = 240 - FIELD_H
JAR_LEN = 180  # visible jar length is at most ~130 px; the far end stays off screen

# name -> (file, crop box or None, scale or target (w, h) or None)
SPRITES = {
    "fb_bean_0": ("bean-0", None, (32, 32)),
    "fb_bean_1": ("bean-1", None, (32, 32)),
    "fb_bean_2": ("bean-2", None, (32, 32)),
    "fb_bean_dead": ("bean-dead", None, (32, 32)),
    # Jars hang from the top (lid at the bottom) or stand on the ground (lid on top).
    "fb_jar_dark_top": ("jar-dark-top", "bottom", None),
    "fb_jar_dark_bottom": ("jar-dark-bottom", "top", None),
    "fb_jar_light_top": ("jar-light-top", "bottom", None),
    "fb_jar_light_bottom": ("jar-light-bottom", "top", None),
    "fb_panel": ("panel", None, None),
    "fb_button_play": ("button-play", "bbox", 0.5),
    "fb_button_board": ("button-board", "bbox", 0.5),
    "fb_button_ok": ("button-ok", None, None),
    "fb_button_pause": ("button-pause", None, None),
    "fb_button_resume": ("button-resume", None, None),
    "fb_new": ("new", None, None),
    "fb_medal_silver": ("medal-silver", None, None),
    "fb_medal_gold": ("medal-gold", None, None),
    "fb_hint_arrow": ("hint-arrow", None, None),
    "fb_hint_hand": ("hint-hand", None, None),
    "fb_hint_tap_left": ("hint-tap-left", None, None),
    "fb_hint_tap_right": ("hint-tap-right", None, None),
    "fb_get_ready": ("get-ready", "bbox", 0.75),
    "fb_game_over": ("game-over", "bbox", 0.8),
    "fb_title": ("title", "bbox", 0.55),
    "fb_bean_icon": ("bean-0", None, (22, 23)),  # screensaver button
    **{f"fb_digit_{i}": (f"digit-{i}", None, None) for i in range(10)},
}


def load(name: str) -> Image.Image:
    return Image.open(SRC / f"{name}.png").convert("RGBA")


def fit(img: Image.Image, crop, scale) -> Image.Image:
    if crop == "bbox":
        img = img.crop(img.getchannel("A").getbbox())
    elif crop == "top":
        img = img.crop((0, 0, img.width, JAR_LEN))
    elif crop == "bottom":
        img = img.crop((0, img.height - JAR_LEN, img.width, img.height))
    if isinstance(scale, float):
        size = (round(img.width * scale), round(img.height * scale))
        img = img.resize(size, Image.Resampling.LANCZOS)
    elif isinstance(scale, tuple):
        # Bean frames differ by a pixel or two: centre them on one canvas so
        # switching frames doesn't wobble.
        if img.width <= scale[0] and img.height <= scale[1]:
            canvas = Image.new("RGBA", scale, (0, 0, 0, 0))
            canvas.alpha_composite(img, ((scale[0] - img.width) // 2,
                                         (scale[1] - img.height) // 2))
            img = canvas
        else:
            img = img.resize(scale, Image.Resampling.LANCZOS)
    return img


# Blank button widths: "TRY AGAIN" fits the first, "PAY INSTEAD" the second
# (font_pixel_8 is 8 px per character, plus room for the rounded outline).
TEXT_BUTTONS = {"fb_button_text": 96, "fb_button_text_wide": 120}


def text_button(width: int) -> Image.Image:
    """Blank button in the style of the play/board sprites, for text labels:
    the play button with its triangle cut out and its plain middle stretched."""
    play = fit(load("button-play"), "bbox", 0.5)
    left = play.crop((0, 0, 15, play.height))  # outline, up to the triangle
    right = play.crop((37, 0, play.width, play.height))  # past the triangle
    middle = play.crop((15, 0, 16, play.height))
    img = Image.new("RGBA", (width, play.height), (0, 0, 0, 0))
    img.paste(left, (0, 0))
    for x in range(left.width, width - right.width):
        img.paste(middle, (x, 0))
    img.paste(right, (width - right.width, 0))
    return img


def background() -> Image.Image:
    """Sky, clouds and grass for the play field (opaque, SCREEN_W x FIELD_H)."""
    src = load("background")
    sky = src.getpixel((5, 100))
    # Clouds start at row 312 of the portrait background, grass at ~355. The
    # sprite has a 1 px frame; leave it out.
    strip = src.crop((2, 312, src.width - 2, 312 + 95))
    strip = strip.resize((SCREEN_W, strip.height), Image.Resampling.NEAREST)
    img = Image.new("RGBA", (SCREEN_W, FIELD_H), sky)
    img.alpha_composite(strip, (0, FIELD_H - strip.height))
    return img


def ground() -> tuple[Image.Image, int]:
    """Striped ground band, one stripe period wider than the screen so it can
    scroll by moving left and jumping back by `period`."""
    src = load("ground")
    band = src.crop((0, 0, src.width, GROUND_H))
    # The sprite is anti-aliased, so take the shift that matches best.
    row = [band.getpixel((x, 10)) for x in range(band.width)]

    def mismatch(p: int) -> float:
        n = band.width - p
        return sum(sum(abs(a - b) for a, b in zip(row[x], row[x + p])) for x in range(n)) / n

    period = min(range(6, 64), key=mismatch)
    w = SCREEN_W + period
    img = Image.new("RGBA", (w, GROUND_H), src.getpixel((5, 60)))
    for x in range(0, w, period):
        img.alpha_composite(band.crop((0, 0, period, GROUND_H)), (x, 0))
    return img, period


def c_array(name: str, data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    return f"static const uint8_t {name}_map[] = {{\n" + "\n".join(lines) + "\n};\n"


def descriptor(name: str, w: int, h: int, cf: str, bpp: int) -> str:
    return f"""const lv_image_dsc_t {name} = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_{cf},
        .w = {w},
        .h = {h},
        .stride = {w * bpp},
    }},
    .data_size = sizeof({name}_map),
    .data = {name}_map,
}};
"""


def argb8888(img: Image.Image) -> bytes:
    r, g, b, a = img.split()
    return Image.merge("RGBA", (b, g, r, a)).tobytes()  # little-endian ARGB


def rgb565(img: Image.Image) -> bytes:
    rgb = img.convert("RGB").tobytes()
    out = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((v & 0xFF, v >> 8))
    return bytes(out)


def main() -> None:
    parts = ["// Generated by tools/flappy_to_lvgl.py from assets/flappy/. Do not edit.\n"
             "// Art: \"Flappy Beans\" by 1UP.capsule, CC BY 4.0, see assets/flappy/LICENSE.txt.\n"
             "#include <lvgl.h>\n"]
    total = 0
    for name, img in (("fb_background", background()), ("fb_ground", ground()[0])):
        data = rgb565(img)
        parts += [c_array(name, data), descriptor(name, img.width, img.height, "RGB565", 2)]
        total += len(data)
    for name, (file, crop, scale) in SPRITES.items():
        img = fit(load(file), crop, scale)
        data = argb8888(img)
        parts += [c_array(name, data),
                  descriptor(name, img.width, img.height, "ARGB8888", 4)]
        total += len(data)
    for name, width in TEXT_BUTTONS.items():
        img = text_button(width)
        parts += [c_array(name, argb8888(img)),
                  descriptor(name, img.width, img.height, "ARGB8888", 4)]
        total += img.width * img.height * 4
    period = ground()[1]
    parts.append(f"const int32_t fb_ground_period = {period};\n")
    OUT.write_text("\n".join(parts))
    print(f"wrote {OUT.relative_to(ROOT)} ({len(SPRITES) + 2} images, {total // 1024} KB, "
          f"ground period {period})")


if __name__ == "__main__":
    main()
