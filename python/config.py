"""設定ファイル (JSON) の読み込みと保存

既定値はリポジトリの config/ (git で管理し、アプリからは書き換えない)。
初めて使うときに、ユーザーごとのフォルダーの config/ へコピーして、以後はそちらを読み書きする
(リポジトリの外。C++ 版と共有。場所は user_config_directory)。
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

_copy_lock = threading.Lock()
_copied = False


class ConfigError(Exception):
    pass


def user_data_directory() -> Path:
    """ユーザーごとのフォルダー (リポジトリの外。設定ファイルと API キーの秘密ファイルを置く)
    macOS: ~/Library/Application Support/LocalLLM
    Linux: $XDG_CONFIG_HOME/LocalLLM (無ければ ~/.config/LocalLLM)
    Windows: %APPDATA%\\LocalLLM"""

    try:
        if sys.platform == "win32":
            base = os.environ.get("APPDATA")
            directory = Path(base) if base else None
        elif sys.platform == "darwin":
            directory = Path.home() / "Library" / "Application Support"
        else:
            xdg = os.environ.get("XDG_CONFIG_HOME")
            directory = Path(xdg) if xdg else Path.home() / ".config"
    except RuntimeError:
        directory = None

    if directory is None:
        raise ConfigError("ユーザーごとのフォルダーが分かりません")

    return directory / "LocalLLM"


def user_config_directory() -> Path:
    """設定画面で書き換える設定ファイルのフォルダー (C++ 版と共有)"""

    return user_data_directory() / "config"


def _copy_defaults(overwrite: bool) -> None:
    """config/ の設定ファイルをユーザーごとのフォルダーにコピーする
    overwrite が False なら、コピー先に無いファイルだけをコピーする
    (あとから config/ に増えたファイルも、次の起動でコピーされる)"""

    directory = user_config_directory()

    try:
        directory.mkdir(parents=True, exist_ok=True)

        for source in sorted(DEFAULT_DIRECTORY.glob("*.json")):
            destination = directory / source.name

            if overwrite or not destination.exists():
                shutil.copyfile(source, destination)
    except OSError as e:
        raise ConfigError(
            f"設定ファイルを {directory} にコピーできません") from e


def config_directory() -> Path:
    """環境変数 LOCALLLM_CONFIG_DIR、無ければユーザーごとのフォルダーの config/
    (無いファイルは初めて使うときに config/ からコピーする)"""

    global _copied

    if env := os.environ.get("LOCALLLM_CONFIG_DIR"):
        return Path(env)

    # ワーカースレッドからも呼ばれるので、コピーは1回だけ
    with _copy_lock:
        if not _copied:
            _copy_defaults(overwrite=False)
            _copied = True

    return user_config_directory()


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


# --- API キー (ChatGPT など) ---
# キーの値は設定ファイルに書かず、名前 (例: OPENAI_API_KEY) で探す
#   1. 同じ名前の環境変数
#   2. ユーザーごとの秘密ファイル (secrets_file_path。C++ 版と同じ場所)

API_KEY_NONE = "none"
API_KEY_ENVIRONMENT = "environment"
API_KEY_SECRETS_FILE = "secrets_file"


def secrets_file_path() -> Path:
    """秘密ファイルの場所 (リポジトリやビルドフォルダーの外。環境変数 LOCALLLM_SECRETS_FILE で変えられる)
    macOS: ~/Library/Application Support/LocalLLM/secrets.json
    Linux: $XDG_CONFIG_HOME/LocalLLM/secrets.json (無ければ ~/.config/LocalLLM/secrets.json)
    Windows: %APPDATA%\\LocalLLM\\secrets.json"""

    if env := os.environ.get("LOCALLLM_SECRETS_FILE"):
        return Path(env)

    return user_data_directory() / "secrets.json"


def _load_secrets(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}

    try:
        data = json.loads(path.read_bytes())
    except OSError as e:
        raise ConfigError(f"秘密ファイルを開けません: {path}") from e
    except ValueError as e:
        raise ConfigError(f"秘密ファイルを JSON として読めません: {path}") from e

    if not isinstance(data, dict):
        raise ConfigError(f"秘密ファイルを JSON として読めません: {path}")

    return data


def find_api_key(name: str) -> tuple[str, str]:
    """API キーを探す。(値, 見つけた場所) を返す。見つからなければ ("", API_KEY_NONE)"""

    if not name:
        return "", API_KEY_NONE

    if value := os.environ.get(name):
        return value, API_KEY_ENVIRONMENT

    try:
        value = _load_secrets(secrets_file_path()).get(name)
    except ConfigError as e:
        # キーの値は出さない
        print(e, file=sys.stderr)
        return "", API_KEY_NONE

    if isinstance(value, str) and value:
        return value, API_KEY_SECRETS_FILE

    return "", API_KEY_NONE


def save_api_key(name: str, value: str) -> None:
    """秘密ファイルに API キーを書く (value が空なら消す)
    macOS / Linux では、ファイルを本人だけが読み書きできるようにする (0600)"""

    path = secrets_file_path()
    secrets = _load_secrets(path)

    if value:
        secrets[name] = value
    elif name in secrets:
        del secrets[name]
    else:
        return

    temporary = path.with_name(path.name + ".tmp")

    try:
        if not path.parent.exists():
            path.parent.mkdir(parents=True, mode=0o700)

        # 中身を書く前に、本人だけが読み書きできるファイルとして作る
        temporary.unlink(missing_ok=True)
        descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)

        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as file:
            file.write(json.dumps(secrets, ensure_ascii=False, indent=2) + "\n")

        os.replace(temporary, path)
    except OSError as e:
        temporary.unlink(missing_ok=True)
        raise ConfigError(f"秘密ファイルを書けません: {path}") from e
