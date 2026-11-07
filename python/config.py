"""config/ の設定ファイル (JSON) の読み込み"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any

# リポジトリのフォルダ (このスクリプトの1つ上)
BASE_DIRECTORY = Path(__file__).resolve().parent.parent


class ConfigError(Exception):
    pass


def config_directory() -> Path:
    """環境変数 LOCALLLM_CONFIG_DIR、無ければリポジトリの config/"""

    if env := os.environ.get("LOCALLLM_CONFIG_DIR"):
        return Path(env)

    return BASE_DIRECTORY / "config"


def load(file_name: str) -> dict[str, Any]:
    """設定ファイルを読む
    呼ぶたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても反映される"""

    path = config_directory() / file_name

    try:
        data = json.loads(path.read_bytes())
    except OSError as e:
        raise ConfigError(f"設定ファイルを開けません: {path}") from e
    except ValueError as e:
        raise ConfigError(f"設定ファイルを JSON として読めません: {path}") from e

    if not isinstance(data, dict):
        raise ConfigError(f"設定ファイルを JSON として読めません: {path}")

    return data


def load_or_empty(file_name: str) -> dict[str, Any]:
    """設定ファイルを読む。読めなければ表示して空の dict を返す
    (画面の設定など、読めなくても既定値で動かしたいもの向け)"""

    try:
        return load(file_name)
    except ConfigError as e:
        print(f"{e} (既定値を使います)", file=sys.stderr)
        return {}


def get_string(file_name: str, key: str) -> str:
    """設定ファイルの key の文字列を読む"""

    value = load(file_name).get(key)

    if not isinstance(value, str):
        raise ConfigError(f"設定ファイル {file_name} に {key} (文字列) がありません")

    return value


def load_for_edit(file_name: str) -> dict[str, Any]:
    """設定画面で書き換えるために読む
    ファイルが無ければ空の dict、JSON のオブジェクトとして読めなければ ConfigError
    (壊れたファイルを上書きして中身を消さないように、読めないときは保存させない)"""

    if not (config_directory() / file_name).exists():
        return {}

    return load(file_name)


def save(file_name: str, data: dict[str, Any]) -> None:
    """設定ファイルに書く
    一時ファイルに書いてから置き換えるので、途中で失敗しても元のファイルは残る"""

    directory = config_directory()
    path = directory / file_name
    temporary = path.with_name(path.name + ".tmp")

    try:
        directory.mkdir(parents=True, exist_ok=True)
        temporary.write_text(
            json.dumps(data, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8", newline="\n")
        os.replace(temporary, path)
    except OSError as e:
        temporary.unlink(missing_ok=True)
        raise ConfigError(f"設定ファイルを書けません: {path}") from e
