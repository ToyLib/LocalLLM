"""設定ファイル (JSON) の読み込みと保存

既定値はリポジトリの config/ (git で管理し、アプリからは書き換えない)。
初めて使うときに、git で無視する user-config/ へコピーして、以後はそちらを読み書きする。
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import threading
from pathlib import Path
from typing import Any

# リポジトリのフォルダ (このスクリプトの1つ上)
BASE_DIRECTORY = Path(__file__).resolve().parent.parent

# 既定値の設定ファイル
DEFAULT_DIRECTORY = BASE_DIRECTORY / "config"

# 設定画面で書き換える設定ファイル (.gitignore で除外している)
USER_DIRECTORY = BASE_DIRECTORY / "user-config"

_copy_lock = threading.Lock()
_copied = False


class ConfigError(Exception):
    pass


def _copy_defaults(overwrite: bool) -> None:
    """config/ の設定ファイルを user-config/ にコピーする
    overwrite が False なら、user-config/ に無いファイルだけをコピーする
    (あとから config/ に増えたファイルも、次の起動でコピーされる)"""

    try:
        USER_DIRECTORY.mkdir(exist_ok=True)

        for source in sorted(DEFAULT_DIRECTORY.glob("*.json")):
            destination = USER_DIRECTORY / source.name

            if overwrite or not destination.exists():
                shutil.copyfile(source, destination)
    except OSError as e:
        raise ConfigError(
            f"設定ファイルを {USER_DIRECTORY} にコピーできません") from e


def config_directory() -> Path:
    """環境変数 LOCALLLM_CONFIG_DIR、無ければリポジトリの user-config/
    (user-config/ は初めて使うときに config/ からコピーして作る)"""

    global _copied

    if env := os.environ.get("LOCALLLM_CONFIG_DIR"):
        return Path(env)

    # ワーカースレッドからも呼ばれるので、コピーは1回だけ
    with _copy_lock:
        if not _copied:
            _copy_defaults(overwrite=False)
            _copied = True

    return USER_DIRECTORY


def reset_to_defaults() -> None:
    """設定を既定値 (config/) に戻す
    LOCALLLM_CONFIG_DIR を指定しているときは、そのフォルダが既定値なので何もしない"""

    if os.environ.get("LOCALLLM_CONFIG_DIR"):
        return

    with _copy_lock:
        _copy_defaults(overwrite=True)


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
