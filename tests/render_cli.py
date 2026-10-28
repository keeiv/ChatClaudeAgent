"""Render replay_cli.cjs cell data as a PNG for visual inspection.

This is a recording preview using Consolas and Microsoft JhengHei. Actual font
metrics, IME composition and mouse selection depend on the user's terminal.
"""
import json
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont


def main():
    source = Path(sys.argv[1])
    data = json.loads(source.read_text(encoding="utf-8"))
    cell_w, cell_h, padding = 10, 24, 16
    image = Image.new("RGB", (data["cols"] * cell_w + padding * 2,
                              data["rows"] * cell_h + padding * 2), "#0c0c0c")
    draw = ImageDraw.Draw(image)
    fonts = {
        (False, False): ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 18),
        (False, True): ImageFont.truetype("C:/Windows/Fonts/consolab.ttf", 18),
        (True, False): ImageFont.truetype("C:/Windows/Fonts/msjh.ttc", 18),
        (True, True): ImageFont.truetype("C:/Windows/Fonts/msjhbd.ttc", 18),
    }
    symbols = ImageFont.truetype("C:/Windows/Fonts/seguisym.ttf", 18)
    rgb = lambda value: ((value >> 16) & 255, (value >> 8) & 255, value & 255)
    for y, row in enumerate(data["screen"]):
        for x, cell in enumerate(row):
            px, py = padding + x * cell_w, padding + y * cell_h
            draw.rectangle((px, py, px + cell_w - 1, py + cell_h - 1), fill=rgb(cell["bg"]))
    for y, row in enumerate(data["screen"]):
        for x, cell in enumerate(row):
            px, py = padding + x * cell_w, padding + y * cell_h
            fg, bg = rgb(cell["fg"]), rgb(cell["bg"])
            text = cell["chars"]
            if not text or cell["width"] == 0:
                continue
            if text in ("█", "▀", "▄"):
                top = py if text != "▄" else py + cell_h // 2
                bottom = py + cell_h - 1 if text != "▀" else py + cell_h // 2 - 1
                draw.rectangle((px, top, px + cell_w - 1, bottom), fill=fg)
            else:
                font = fonts[(cell["width"] == 2, cell.get("bold", False))]
                if cell["width"] == 1 and any(ord(c) > 0x2000 for c in text):
                    font = symbols
                draw.text((px, py + 19), text, font=font, anchor="ls", fill=fg)
    target = source.with_suffix(".png")
    image.save(target)
    print(target.name)


if __name__ == "__main__":
    main()
