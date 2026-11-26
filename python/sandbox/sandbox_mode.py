"""箱庭のコマンドとアニメーションの状態。"""

from dataclasses import dataclass

from common.agent import Agent, run_agent_json
from common.llm_client import CancelToken, LLMOptions

DUSK = 0.55
COMMANDS = {
    "day": "晴れた昼の港全体に戻す。「昼の港に戻して」「明るい昼にして」",
    "blizzard_night": "吹雪の夜の港全体にする。「吹雪の夜にして」",
    "dusk": "時刻だけ夕暮れにする。「夕暮れにして」",
    "snow": "天気だけ雪にする。「雪を降らせて」「吹雪にして」",
    "clear_weather": "天気だけ晴れにする。「雪だけ止めて」「霧を晴らして」",
    "fog": "天気だけ霧にする。「霧を出して」",
    "lighthouse_on": "灯台だけ点灯する。「灯台をつけて」",
    "lighthouse_off": "灯台だけ消灯する。「灯台を消して」",
    "house_lights_on": "家々の窓だけ点灯する。「家の明かりをつけて」",
    "house_lights_off": "家々の窓だけ消灯する。「家のあかりを消して」",
    "sail": "船を走らせる。「船を出して」",
    "dock": "船を港に戻して停泊させる。「船を港に戻して」",
    "unsupported": "複数の異なる操作、一覧にない操作、不明な指示。「雨にして」",
}
EXAMPLES = "例：吹雪の夜にして、雪だけ止めて、家の明かりを消して、船を港に戻して"


def interpret(text: str, cancel: CancelToken) -> str:
    prompt = (
        "港町への指示を、次の操作のうち1つに分類し、JSONのcommandに入れてください。\n"
        + "\n".join(f"{key}: {description}" for key, description in COMMANDS.items())
    )
    agent = Agent(
        name="箱庭",
        system_prompt=prompt,
        options=LLMOptions(
            max_tokens=256, temperature=0.0, disable_thinking=True,
            schema={
                "type": "object",
                "properties": {"command": {"type": "string", "enum": list(COMMANDS)}},
                "required": ["command"],
                "additionalProperties": False,
            },
        ),
    )
    result = run_agent_json(agent, [{"role": "user", "content": text}], cancel)
    if isinstance(result, dict) and result.keys() == {"command"}:
        command = result["command"]
        if isinstance(command, str) and command in COMMANDS:
            return command
    raise ValueError("風景の指定を読み取れませんでした")


def _approach(current: float, target: float, step: float) -> float:
    if abs(target - current) <= step:
        return target
    return current + step if current < target else current - step


@dataclass
class Harbor:
    time_of_day: float = 0.0
    weather: str = "clear"
    lighthouse_on: bool = False
    house_lights_on: bool | None = None
    sailing: bool = False

    darkness: float = 0.0
    snow: float = 0.0
    fog: float = 0.0
    beam: float = 0.0
    house_light: float = 0.0
    boat_position: float = 0.0
    boat_direction: int = 1
    time: float = 0.0

    def apply(self, command: str) -> None:
        if command in ("day", "blizzard_night"):
            blizzard = command == "blizzard_night"
            self.time_of_day = float(blizzard)
            self.weather = "snow" if blizzard else "clear"
            self.lighthouse_on = blizzard
            self.house_lights_on = None
            self.sailing = blizzard
        elif command == "dusk":
            self.time_of_day = DUSK
        elif command in ("snow", "fog", "clear_weather"):
            self.weather = "clear" if command == "clear_weather" else command
        elif command in ("lighthouse_on", "lighthouse_off"):
            self.lighthouse_on = command == "lighthouse_on"
        elif command in ("house_lights_on", "house_lights_off"):
            self.house_lights_on = command == "house_lights_on"
        elif command in ("sail", "dock"):
            self.sailing = command == "sail"

    @property
    def house_light_target(self) -> float:
        return self.time_of_day if self.house_lights_on is None else float(self.house_lights_on)

    @property
    def animating(self) -> bool:
        return (
            self.darkness != self.time_of_day or self.house_light != self.house_light_target
            or self.weather == "snow" or self.snow > 0
            or self.fog != float(self.weather == "fog")
            or self.lighthouse_on or self.beam > 0 or self.sailing or self.boat_position > 0
        )

    def advance(self, elapsed: float) -> None:
        self.time += elapsed
        step = elapsed * 0.9
        self.darkness = _approach(self.darkness, self.time_of_day, step)
        self.house_light = _approach(self.house_light, self.house_light_target, step)
        self.snow = _approach(self.snow, float(self.weather == "snow"), step)
        self.fog = _approach(self.fog, float(self.weather == "fog"), step)
        self.beam = _approach(self.beam, float(self.lighthouse_on), step)

        distance = elapsed * 0.23
        if self.sailing:
            if self.boat_position >= 1:
                self.boat_direction = -1
            elif self.boat_position <= 0:
                self.boat_direction = 1
            self.boat_position = max(0.0, min(1.0, self.boat_position + distance * self.boat_direction))
        elif self.boat_position > 0:
            self.boat_direction = -1
            self.boat_position = max(0.0, self.boat_position - distance)
