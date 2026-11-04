"""config/ の設定ファイル (JSON) の読み込み"""

from __future__ import annotations

import json
import os
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


def get_string(file_name: str, key: str) -> str:
    """設定ファイルの key の文字列を読む"""

    value = load(file_name).get(key)

    if not isinstance(value, str):
        raise ConfigError(f"設定ファイル {file_name} に {key} (文字列) がありません")

    return value
