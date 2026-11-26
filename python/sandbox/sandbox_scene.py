"""Tk Canvas に港町を描く。座標は 800×450 の画面を基準にする。"""

import math
import tkinter as tk

from sandbox.sandbox_mode import DUSK, Harbor

WIDTH = 800
HEIGHT = 450


def _mix(first: str, second: str, amount: float) -> str:
    channels = (
        round(int(first[i:i + 2], 16) * (1 - amount) + int(second[i:i + 2], 16) * amount)
        for i in range(0, 6, 2)
    )
    return "".join(f"{channel:02x}" for channel in channels)


class HarborCanvas(tk.Canvas):
    def draw(self, harbor: Harbor) -> None:
        self.delete("harbor")
        width, height = self.winfo_width(), self.winfo_height()
        scale = min(width / WIDTH, height / HEIGHT)
        origin_x = (width - WIDTH * scale) / 2
        origin_y = (height - HEIGHT * scale) / 2

        def polygon(color: str, *points: tuple[float, float]) -> None:
            coordinates = [(origin_x + x * scale, origin_y + y * scale) for x, y in points]
            self.create_polygon(*coordinates, fill="#" + color, tags="harbor")

        def rect(color: str, x: float, y: float, w: float, h: float) -> None:
            self.create_rectangle(
                origin_x + x * scale, origin_y + y * scale,
                origin_x + (x + w) * scale, origin_y + (y + h) * scale,
                fill="#" + color, width=0, tags="harbor",
            )

        def oval(color: str, x: float, y: float, w: float, h: float) -> None:
            self.create_oval(
                origin_x + x * scale, origin_y + y * scale,
                origin_x + (x + w) * scale, origin_y + (y + h) * scale,
                fill="#" + color, width=0, tags="harbor",
            )

        def line(color: str, x: float, y: float, end_x: float, end_y: float) -> None:
            self.create_line(
                origin_x + x * scale, origin_y + y * scale,
                origin_x + end_x * scale, origin_y + end_y * scale,
                fill="#" + color, width=max(1.0, scale), tags="harbor",
            )

        night = harbor.darkness

        def shade(day: str, dark: str) -> str:
            return _mix(day, dark, night)

        def palette(day: str, dusk: str, dark: str) -> str:
            if night <= DUSK:
                return _mix(day, dusk, night / DUSK)
            return _mix(dusk, dark, (night - DUSK) / (1 - DUSK))

        sky = palette("74b9dc", "8174a9", "111b36")
        horizon = _mix(palette("f4deac", "ffac88", "536686"), "bbcbd7", harbor.fog * 0.65)
        sea = palette("2296ab", "526d96", "142743")
        shore = palette("89c7ca", "be9eb4", "395775")
        for row in range(12):
            weight = row / 11
            rect(_mix(sky, horizon, weight), 0, 16 * row, WIDTH, 16)
            rect(_mix(shore, sea, weight), 0, 192 + 21.5 * row, WIDTH, 21.5)

        oval(_mix(shade("fff2b7", "dbeaff"), horizon, harbor.fog * 0.8), 621, 54, 58, 58)
        hills = _mix(shade("7fa89e", "2c3d59"), horizon, harbor.fog * 0.85)
        polygon(hills, (0, 192), (92, 120), (164, 161), (222, 132), (324, 192))
        polygon(hills, (493, 192), (558, 159), (616, 174), (696, 133), (800, 192))
        ripple = shade("78c2ca", "41617f")
        for row in range(34):
            x = (row * 173) % 745
            y = 201 + (row * 47) % 236
            line(ripple, x, y, x + 12 + row % 25, y)

        polygon(shade("69777e", "27354d"), (48, 263), (294, 356), (470, 282),
                (470, 305), (294, 380), (48, 287))
        polygon(shade("e0c19a", "5a6d85"), (48, 263), (216, 189), (470, 282), (294, 356))

        houses = ((207, 194, 50), (268, 213, 58), (114, 230, 58), (178, 253, 64), (253, 278, 65))
        for index, (x, y, w) in enumerate(houses):
            rect(shade("f9d5aa", "495772"), x, y, w, 46)
            polygon(shade("c8967e", "354360"), (x + w, y), (x + w + 18, y - 11),
                    (x + w + 18, y + 35), (x + w, y + 46))
            polygon(shade("d57869", "633c55"), (x, y), (x + w / 2, y - 28), (x + w, y))
            polygon(shade("ad5360", "45364f"), (x + w / 2, y - 28), (x + w / 2 + 18, y - 39),
                    (x + w + 18, y - 11), (x + w, y))
            light = max(0.0, min(1.0, harbor.house_light * 2 - index * 0.13 - 0.32))
            for offset in (0.23, 0.62):
                rect(_mix("48677b", "ffda83", light), x + w * offset, y + 10, 10, 13)

        if harbor.beam > 0:
            sweep = math.sin(harbor.time * 0.85)
            beam_x = 690 + sweep * 95
            beam_y = 240 + sweep * 42
            spread = 19 + 14 * harbor.fog
            beam = _mix(shore, "ffe4a8", harbor.beam * 0.48)
            polygon(beam, (389, 171), (beam_x, beam_y - spread), (beam_x, beam_y + spread))
            oval(beam, beam_x - 22, beam_y - 3, 44, 6)

        tower = shade("f6e5c4", "a4b4c9")
        polygon(tower, (377, 185), (401, 185), (410, 286), (368, 286))
        polygon(shade("b95456", "75475c"), (375, 211), (403, 211), (405, 231), (373, 231))
        rect("37445e", 374, 158, 30, 28)
        rect(_mix("7196a7", "ffe59b", harbor.beam), 379, 163, 20, 16)
        polygon(shade("ab5560", "673e56"), (368, 158), (389, 142), (410, 158))

        boat_x = 474 + 224 * harbor.boat_position
        boat_y = 319 + 55 * harbor.boat_position
        direction = harbor.boat_direction
        if harbor.boat_position > 0:
            wake = _mix(sea, "b9dce4", min(0.8, harbor.boat_position * 9))
            for index in range(3):
                x = boat_x - direction * (24 + 14 * index)
                y = boat_y + 8 + 5 * index
                line(wake, x, y, x - direction * (10 + 4 * index), y + 3)
        bow = direction * 6
        haze = harbor.fog * harbor.boat_position * 0.7
        polygon(_mix("c85e61", horizon, haze), (boat_x - 24 + bow, boat_y), (boat_x + 24 + bow, boat_y),
                (boat_x + 17, boat_y + 9), (boat_x - 17, boat_y + 9))
        rect(_mix(tower, horizon, haze), boat_x - 12, boat_y - 11, 24, 11)

        for index in range(round(100 * harbor.snow)):
            x = 795 - ((index * 193 + harbor.time * (40 + index % 70)) % 790)
            y = (index * 101 + harbor.time * (20 + index % 37)) % 445
            line("d9eaf6", x, y, x - 2 - index % 4, y + 3)
