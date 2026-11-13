"""llama-server (チャット用と埋め込み用) を子プロセスとして起動・監視・停止する

設定は config/server.json の managed。専用のスレッドで動く (C++ 版の server_manager と同じ動き)
"""

from __future__ import annotations

import hashlib
import http.client
import json
import logging
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable

from common import config
from common.llm_client import CancelToken, Cancelled, LLMError

log = logging.getLogger(__name__)

SERVER_CONFIG = "server.json"
BINARY_NAME = "llama-server.exe" if sys.platform == "win32" else "llama-server"

# 取得する公式リリース (C++ 版のビルドと共有)
RELEASE_FILE = config.BASE_DIRECTORY / "llama-release.json"
RELEASE_URL = "https://github.com/ggml-org/llama.cpp/releases/download"

# 確認のために llama-server を実行するときに待つ時間 (秒)
CHECK_TIMEOUT = 15

# 普通に止めるのを待つ時間 (秒。過ぎたら強制終了)
STOP_TIMEOUT = 5

# 画面に出すログの長さ
MAX_DETAIL_CHARS = 80
LOG_TAIL_LINES = 8

# llama-server が使わないので展開後に消すファイル (ggml-* のライブラリは実行時に読み込まれるので消さない)
_UNUSED_FILE = re.compile(
    r"^(llama(\.exe)?|llama-(?!server(\.exe)?$)[^.]*(\.exe)?"
    r"|ggml-rpc-server.*|ggml-metal-tuning.*"
    r"|(lib)?llama-(?!server-impl)[a-z-]*-impl\..*)$")

_ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


class ServerStatus(Enum):
    OFF = "起動しない"
    STARTING = "ダウンロードまたは読み込み中"
    READY = "使えます"
    EXTERNAL = "使えます (外部のサーバー)"
    FAILED = "失敗"


@dataclass(frozen=True)
class ServerView:
    status: ServerStatus = ServerStatus.OFF
    detail: str = ""
    log_tail: str = ""


@dataclass
class ManagerView:
    """画面に出す状態"""

    enabled: bool = False
    chat: ServerView = ServerView()
    embed: ServerView = ServerView()
    binary_path: str = ""
    binary_source: str = ""
    warning: str = ""
    log_directory: str = ""


@dataclass
class ManagedSettings:
    # 前の版の設定ファイルをそのまま読めるように、managed が無ければ自動起動しない
    enabled: bool = False
    server_path: str = ""
    chat_model: str = "unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M"

    # 空なら埋め込みサーバーを起動しない
    embed_model: str = "ggml-org/bge-m3-Q8_0-GGUF"
    context: int = 4096
    embed_context: int = 8192
    gpu_layers: int = 99


def load_managed_settings() -> ManagedSettings:
    """server.json の managed (書いていない・型が違う項目は既定値)"""

    settings = ManagedSettings()
    managed = config.load_or_empty(SERVER_CONFIG).get("managed")

    if not isinstance(managed, dict):
        return settings

    def number(key: str, fallback: int) -> int:
        value = managed.get(key, fallback)
        valid = isinstance(value, (int, float)) and not isinstance(value, bool)
        return int(value) if valid else fallback

    def string(key: str, fallback: str) -> str:
        value = managed.get(key, fallback)
        return value if isinstance(value, str) else fallback

    settings.enabled = managed.get("enabled") is True
    settings.server_path = string("server_path", settings.server_path)
    settings.chat_model = string("chat_model", settings.chat_model)
    settings.embed_model = string("embed_model", settings.embed_model)
    settings.context = number("context", settings.context)
    settings.embed_context = number("embed_context", settings.embed_context)
    settings.gpu_layers = number("gpu_layers", settings.gpu_layers)
    return settings


@dataclass
class _Spec:
    """起動するサーバーの内容 (変わったら起動し直す)"""

    run: bool = False
    port: int = 0
    server_path: str = ""
    args: list[str] = field(default_factory=list)


@dataclass
class _Binary:
    path: str = ""
    source: str = ""
    error: str = ""


class _Slot:
    """管理用のスレッドだけが触る"""

    def __init__(self, file_name: str) -> None:
        self.file_name = file_name
        self.spec = _Spec()
        self.process: subprocess.Popen[bytes] | None = None
        self.view = ServerView()


def _log_directory() -> Path:
    return config.user_data_directory() / "logs"


def _log_path(slot: _Slot) -> Path:
    return _log_directory() / f"{slot.file_name}.log"


def _local_port(url: str) -> int | None:
    """http://127.0.0.1:8080/... の形ならポートを返す (ローカルでなければ None)"""

    try:
        parts = urllib.parse.urlsplit(url)
        port = parts.port
    except ValueError:
        return None

    if parts.scheme != "http" or parts.hostname not in ("127.0.0.1", "localhost"):
        return None

    return port if port else 80


def _is_executable(path: Path) -> bool:
    return path.is_file() and (sys.platform == "win32" or os.access(path, os.X_OK))


def _search_path() -> _Binary | None:
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        directory = Path(entry)

        # 空や相対の項目は、カレントディレクトリの実行ファイルを拾うので見ない
        if entry and directory.is_absolute() and _is_executable(directory / BINARY_NAME):
            return _Binary(str(directory / BINARY_NAME), "PATH")

    if sys.platform == "darwin":
        # Finder から起動すると PATH に Homebrew が入らない
        for directory in (Path("/opt/homebrew/bin"), Path("/usr/local/bin")):
            if _is_executable(directory / BINARY_NAME):
                return _Binary(str(directory / BINARY_NAME), "Homebrew")

    return None


def _llama_directory() -> Path:
    if env := os.environ.get("LLAMA_DIR"):
        return Path(env)

    return Path.home() / "Repos" / "llama.cpp"


def _release() -> dict[str, Any]:
    return json.loads(RELEASE_FILE.read_text(encoding="utf-8"))


def _platform_key() -> str:
    machine = platform.machine().lower()
    cpu = {"amd64": "x64", "x86_64": "x64", "aarch64": "arm64"}.get(machine, machine)
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    return f"{system}-{cpu}"


def _bundled_directory(tag: str) -> Path:
    return config.user_data_directory() / "llama" / tag


def _find_binary(server_path: str) -> _Binary:
    if server_path:
        if _is_executable(Path(server_path)):
            return _Binary(str(Path(server_path).absolute()), "設定")

        return _Binary(error=f"設定の llama-server が見つかりません: {server_path}")

    if env := os.environ.get("LLAMA_SERVER"):
        if _is_executable(Path(env)):
            return _Binary(str(Path(env).absolute()), "環境変数 LLAMA_SERVER")

        return _Binary(error=f"環境変数 LLAMA_SERVER の llama-server が見つかりません: {env}")

    try:
        bundled = _bundled_directory(_release()["tag"]) / BINARY_NAME

        if _is_executable(bundled):
            return _Binary(str(bundled), "ダウンロード済み")
    except (OSError, ValueError, KeyError) as e:
        log.warning("llama-release.json を読めません: %s", e)

    directory = _llama_directory() / "build" / "bin"

    if sys.platform == "win32":
        directory /= "Release"

    if _is_executable(directory / BINARY_NAME):
        return _Binary(str(directory / BINARY_NAME), "LLAMA_DIR のビルド")

    return _search_path() or _Binary()


def _safe_tar_members(archive: tarfile.TarFile) -> list[tarfile.TarInfo]:
    """展開先の外に書くメンバーがあれば拒否する"""

    members = archive.getmembers()

    for member in members:
        path = Path(member.name)

        if path.is_absolute() or ".." in path.parts:
            raise LLMError(f"アーカイブの中身が正しくありません: {member.name}")

        if member.issym() and ("/" in member.linkname or ".." in member.linkname):
            raise LLMError(f"アーカイブの中身が正しくありません: {member.name}")

        if not (member.isfile() or member.isdir() or member.issym()):
            raise LLMError(f"アーカイブの中身が正しくありません: {member.name}")

    return members


def _extract(archive_path: Path, destination: Path) -> None:
    if archive_path.name.endswith(".zip"):
        with zipfile.ZipFile(archive_path) as archive:
            # extractall は ".." や絶対パスを展開先の中に収める
            archive.extractall(destination)
        return

    with tarfile.open(archive_path, "r:gz") as archive:
        members = _safe_tar_members(archive)

        if hasattr(tarfile, "data_filter"):
            archive.extractall(destination, members=members, filter="data")
        else:
            archive.extractall(destination, members=members)


def _terminate(process: subprocess.Popen[bytes]) -> None:
    try:
        process.kill()
    except OSError:
        pass

    process.wait()


# --- アプリが異常終了したときの後始末 ---

if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    class _BasicLimit(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_int64),
            ("PerJobUserTimeLimit", ctypes.c_int64),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class _IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class _ExtendedLimit(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _BasicLimit),
            ("IoInfo", _IoCounters),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    _kernel32.SetInformationJobObject.argtypes = (
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    _kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)

    _job: Any = None

    def _remember_process(_slot: _Slot, process: subprocess.Popen[bytes]) -> None:
        """アプリが終わると一緒に終わるように、ジョブに入れる"""

        global _job

        if _job is None:
            _job = _kernel32.CreateJobObjectW(None, None)
            info = _ExtendedLimit()
            info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            _kernel32.SetInformationJobObject(
                _job, _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                ctypes.byref(info), ctypes.sizeof(info))

        if _job:
            _kernel32.AssignProcessToJobObject(_job, int(process._handle))  # type: ignore[attr-defined]

    def _forget_process(_slot: _Slot) -> None:
        pass

    def _clean_up_orphan(_slot: _Slot) -> None:
        pass

else:
    import signal

    # macOS には親の終了を子に知らせる仕組みが無いので、Linux も含めて PID ファイルで片付ける
    # 中身は「サーバーの PID アプリの PID」(C++ 版と同じ形式)
    def _pid_file(slot: _Slot) -> Path:
        return config.user_data_directory() / "run" / f"{slot.file_name}.pid"

    def _remember_process(slot: _Slot, process: subprocess.Popen[bytes]) -> None:
        path = _pid_file(slot)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"{process.pid} {os.getpid()}\n", encoding="ascii")

    def _forget_process(slot: _Slot) -> None:
        _pid_file(slot).unlink(missing_ok=True)

    def _is_alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
        except PermissionError:
            return True
        except OSError:
            return False

        return True

    def _is_llama_server(pid: int) -> bool:
        """その PID のプロセスが llama-server か (PID は再利用されるので確かめる)"""

        result = subprocess.run(
            ["ps", "-p", str(pid), "-o", "comm="],
            capture_output=True, text=True, check=False)
        return "llama-server" in result.stdout

    def _clean_up_orphan(slot: _Slot) -> None:
        """前回のアプリが異常終了して残ったサーバーを止める
        (アプリがまだ生きていれば、C++ 版と Python 版の同時起動なので触らない)"""

        try:
            server, owner = map(int, _pid_file(slot).read_text(encoding="ascii").split())
        except FileNotFoundError:
            return
        except (OSError, ValueError):
            _forget_process(slot)
            return

        if server <= 0 or owner <= 0:
            _forget_process(slot)
            return

        if _is_alive(owner):
            return

        if _is_alive(server) and _is_llama_server(server):
            log.info("残っていた %s を止めます (PID %d)", slot.file_name, server)
            os.kill(server, signal.SIGTERM)
            deadline = time.monotonic() + STOP_TIMEOUT

            while _is_alive(server) and time.monotonic() < deadline:
                time.sleep(0.05)

            if _is_alive(server):
                os.kill(server, signal.SIGKILL)

        _forget_process(slot)


def _spec(settings: ManagedSettings, url: str, model: str, extra: list[str]) -> _Spec:
    port = _local_port(url)

    if not settings.enabled or not model or port is None:
        return _Spec()

    return _Spec(
        run=True, port=port, server_path=settings.server_path,
        args=["-hf", model, "-ngl", str(settings.gpu_layers), *extra,
              "--host", "127.0.0.1", "--port", str(port)])


def _probe_health(port: int) -> str:
    """"none" (応答なし)、"loading" (読み込み中)、"ready" のどれか"""

    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=1.5)

    try:
        connection.request("GET", "/health")
        status = connection.getresponse().status
    except (OSError, http.client.HTTPException):
        return "none"
    finally:
        connection.close()

    return {200: "ready", 503: "loading"}.get(status, "none")


def _last_lines(path: Path, count: int) -> list[str]:
    """ログの最後の行 (\\r で上書きする進捗も 1 行として扱う)"""

    try:
        with path.open("rb") as file:
            file.seek(0, os.SEEK_END)
            file.seek(max(0, file.tell() - 16 * 1024))
            text = file.read().decode("utf-8", errors="replace")
    except OSError:
        return []

    lines = [_ANSI_ESCAPE.sub("", line).strip()
             for line in re.split(r"[\r\n]+", text)]
    return [line for line in lines if line][-count:]


def _shorten(text: str) -> str:
    return text if len(text) <= MAX_DETAIL_CHARS else text[:MAX_DETAIL_CHARS] + "…"


def _has_gpu_device(output: str) -> bool:
    """--list-devices の「Available devices:」の後に、BLAS 以外のデバイスがあるか"""

    _before, found, after = output.partition("Available devices:")
    return bool(found) and any(
        line.strip() and not line.strip().startswith("BLAS")
        for line in after.splitlines())


class _Manager:
    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._thread: threading.Thread | None = None
        self._notify: Callable[[], None] = lambda: None

        self._stop = False
        self._reload = True
        self._retry = False

        # 読み直した設定を、すべてのサーバーに反映し終わったか
        self._settled = False
        self._view = ManagerView()

        # 管理用のスレッドだけが触る
        self._chat = _Slot("chat-server")
        self._embed = _Slot("embed-server")
        self._binary: _Binary | None = None
        self._verified_path = ""
        self._gpu_warning = ""

    # --- 画面から呼ぶ ---

    def start(self, notify: Callable[[], None]) -> None:
        self._notify = notify
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        with self._condition:
            self._stop = True
            self._condition.notify_all()

        if self._thread is not None:
            self._thread.join()
            self._thread = None

    def reload(self) -> None:
        with self._condition:
            self._reload = True
            self._settled = False
            self._condition.notify_all()

    def retry(self) -> None:
        with self._condition:
            self._retry = True
            self._condition.notify_all()

    def view(self) -> ManagerView:
        with self._condition:
            return ManagerView(**vars(self._view))

    def is_chat_available(self) -> bool:
        with self._condition:
            return self._settled and self._view.chat.status not in (
                ServerStatus.STARTING, ServerStatus.FAILED)

    def wait_for_embed(self, cancel: CancelToken) -> None:
        """埋め込みサーバーが使えるようになるまで待つ"""

        with self._condition:
            while True:
                if cancel.is_set():
                    raise Cancelled()

                if self._settled:
                    embed = self._view.embed

                    if embed.status == ServerStatus.FAILED:
                        raise LLMError(f"埋め込みサーバーを起動できません: {embed.detail}")

                    if embed.status != ServerStatus.STARTING:
                        return

                self._condition.wait(0.2)

    # --- 管理用のスレッド ---

    def _stop_requested(self) -> bool:
        with self._condition:
            return self._stop

    def _publish(self, slot: _Slot) -> None:
        attribute = "chat" if slot is self._chat else "embed"

        with self._condition:
            if getattr(self._view, attribute) == slot.view:
                return

            setattr(self._view, attribute, slot.view)
            self._condition.notify_all()

        self._notify()

    def _set_view(
            self, slot: _Slot, status: ServerStatus, detail: str = "",
            log_tail: str = "") -> None:
        slot.view = ServerView(status, detail, log_tail)
        self._publish(slot)

    def _run_check(self, path: str, option: str) -> tuple[bool, str]:
        """実行して終わるのを待つ。(正常に終わったか, 出力)"""

        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

        try:
            process = subprocess.Popen(
                [path, option], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, creationflags=flags)
        except OSError:
            return False, ""

        try:
            output, _ = process.communicate(timeout=CHECK_TIMEOUT)
        except subprocess.TimeoutExpired:
            _terminate(process)
            return False, ""

        return process.returncode == 0, output.decode("utf-8", errors="replace")

    def _download(self, slot: _Slot) -> _Binary:
        """公式リリースをダウンロードして、ユーザーごとのフォルダーに展開する"""

        release = _release()
        tag = release["tag"]
        key = _platform_key()
        asset = release["assets"].get(key)

        if asset is None:
            return _Binary(
                error=f"{key} 向けの llama-server はありません。設定画面のサーバーのタブで場所を指定してください")

        destination = _bundled_directory(tag)
        destination.parent.mkdir(parents=True, exist_ok=True)

        with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
            archive_path = Path(temporary) / asset["file"]
            url = f"{RELEASE_URL}/{tag}/{asset['file']}"
            digest = hashlib.sha256()

            self._set_view(slot, ServerStatus.STARTING, "llama-server をダウンロード中")

            with urllib.request.urlopen(url, timeout=30) as response, \
                    archive_path.open("wb") as file:
                received = 0

                while chunk := response.read(1024 * 1024):
                    if self._stop_requested():
                        raise Cancelled()

                    file.write(chunk)
                    digest.update(chunk)
                    received += len(chunk)
                    self._set_view(
                        slot, ServerStatus.STARTING,
                        f"llama-server をダウンロード中 {received // (1024 * 1024)} MB")

            if digest.hexdigest() != asset["sha256"]:
                return _Binary(error="ダウンロードした llama-server のハッシュが一致しません")

            extracted = Path(temporary) / "extracted"
            _extract(archive_path, extracted)

            # tar.gz は 1 つのフォルダーに入っている
            entries = list(extracted.iterdir())
            root = entries[0] if len(entries) == 1 and entries[0].is_dir() else extracted

            for path in root.iterdir():
                if _UNUSED_FILE.match(path.name):
                    path.unlink()

            binary = root / BINARY_NAME

            if not binary.is_file():
                return _Binary(error="リリースの中に llama-server がありません")

            if sys.platform != "win32":
                binary.chmod(binary.stat().st_mode | stat.S_IXUSR)

            shutil.rmtree(destination, ignore_errors=True)
            root.replace(destination)

        return _Binary(str(destination / BINARY_NAME), "ダウンロード済み")

    def _resolve_binary(self, slot: _Slot) -> _Binary:
        """使う llama-server を探して、動くことを確かめる (結果は設定が変わるまで使い回す)"""

        if self._binary is not None and not self._binary.error:
            return self._binary

        found = _find_binary(slot.spec.server_path)

        if not found.path and not found.error:
            found = self._download(slot)

        if not found.error and not self._run_check(found.path, "--version")[0]:
            found = _Binary(error=f"llama-server を実行できません: {found.path}")

        if not found.error and found.path != self._verified_path:
            self._verified_path = found.path
            devices = self._run_check(found.path, "--list-devices")[1]
            self._gpu_warning = (
                "" if _has_gpu_device(devices)
                else "GPU が見つかりません。CPU で動くので遅くなります")

        self._binary = found

        with self._condition:
            self._view.binary_path = found.path
            self._view.binary_source = found.source
            self._view.warning = "" if found.error else self._gpu_warning

        self._notify()
        return found

    def _launch(self, slot: _Slot) -> None:
        """llama-server を起動する (出力はログファイルへ)"""

        binary = self._resolve_binary(slot)

        if binary.error:
            self._set_view(slot, ServerStatus.FAILED, binary.error)
            return

        _log_directory().mkdir(parents=True, exist_ok=True)
        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

        try:
            with _log_path(slot).open("wb") as log_file:
                slot.process = subprocess.Popen(
                    [binary.path, *slot.spec.args], stdin=subprocess.DEVNULL,
                    stdout=log_file, stderr=subprocess.STDOUT, creationflags=flags)
        except OSError as e:
            self._set_view(slot, ServerStatus.FAILED, f"llama-server を起動できません: {e}")
            return

        _remember_process(slot, slot.process)
        self._set_view(slot, ServerStatus.STARTING, "起動中")

    def _stop_processes(self, slots: list[_Slot]) -> None:
        """自分が起動したサーバーを止める (まとめて止めて、待つ時間を 1 回分にする)"""

        running = [slot for slot in slots if slot.process is not None]

        for slot in running:
            slot.process.terminate()

        deadline = time.monotonic() + STOP_TIMEOUT

        for slot in running:
            try:
                slot.process.wait(max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                _terminate(slot.process)

            slot.process = None
            _forget_process(slot)

    def _step(self, slot: _Slot) -> None:
        """1つのサーバーの状態を確かめて、必要なら起動する (_run から定期的に呼ぶ)"""

        if not slot.spec.run:
            self._stop_processes([slot])
            self._set_view(slot, ServerStatus.OFF)
            return

        # 自分で起動したサーバーの監視: 落ちていないか、準備ができたか
        if slot.process is not None:
            code = slot.process.poll()

            if code is not None:
                slot.process = None
                _forget_process(slot)
                tail = "\n".join(_last_lines(_log_path(slot), LOG_TAIL_LINES))
                self._set_view(
                    slot, ServerStatus.FAILED,
                    f"llama-server が終了しました (コード {code})", tail)
                return

            if slot.view.status == ServerStatus.READY:
                return

            if _probe_health(slot.spec.port) == "ready":
                self._set_view(slot, ServerStatus.READY)
                return

            # モデルのダウンロード中は /health が応答しないことがあるので、
            # 時間では打ち切らず、プロセスが生きている間は待つ
            lines = _last_lines(_log_path(slot), 1)
            self._set_view(
                slot, ServerStatus.STARTING,
                _shorten(lines[-1]) if lines else "起動中")
            return

        # 失敗したら、再試行を頼まれるまで起動しない
        if slot.view.status == ServerStatus.FAILED:
            return

        # すでに動いているサーバーがあれば、それを使う
        health = _probe_health(slot.spec.port)

        if health == "ready":
            self._set_view(
                slot, ServerStatus.EXTERNAL, "すでに動いているサーバーを使っています")
        elif health == "loading":
            self._set_view(
                slot, ServerStatus.STARTING, "すでに動いているサーバーが読み込み中です")
        else:
            self._launch(slot)

    def _apply_settings(self) -> None:
        """server.json を読み直し、内容が変わったサーバーを止める (次の _step で起動し直す)"""

        server = config.load_or_empty(SERVER_CONFIG)
        settings = load_managed_settings()

        def url(key: str) -> str:
            value = server.get(key)
            return value if isinstance(value, str) else ""

        embed_context = str(settings.embed_context)
        chat = _spec(
            settings, url("chat_url"), settings.chat_model,
            ["-c", str(settings.context), "--alias", "local"])
        embed = _spec(
            settings, url("embed_url"), settings.embed_model,
            ["--embeddings", "-c", embed_context, "-b", embed_context,
             "-ub", embed_context, "--alias", "embed"])

        if (chat.server_path != self._chat.spec.server_path or
                embed.server_path != self._embed.spec.server_path):
            self._binary = None

        changed = []

        for slot, spec in ((self._chat, chat), (self._embed, embed)):
            if slot.spec != spec:
                changed.append(slot)
                slot.spec = spec

        self._stop_processes(changed)

        for slot in changed:
            slot.view = ServerView()

        with self._condition:
            self._view.enabled = settings.enabled

    def _run(self) -> None:
        """管理用のスレッドの本体: 要求を待ち (最長 1 秒)、設定を反映して、各サーバーの _step を呼ぶ"""

        try:
            with self._condition:
                self._view.log_directory = str(_log_directory())
        except config.ConfigError as e:
            log.warning("%s", e)

        first = True
        slots = (self._chat, self._embed)

        while True:
            with self._condition:
                self._condition.wait_for(
                    lambda: self._stop or self._reload or self._retry, timeout=1.0)

                if self._stop:
                    break

                reload_settings, retry = self._reload, self._retry
                self._reload = self._retry = False

            try:
                # 前回の異常終了で残ったサーバーを片付ける
                if first:
                    first = False

                    for slot in slots:
                        _clean_up_orphan(slot)

                if reload_settings:
                    self._apply_settings()

                if retry:
                    self._binary = None

                    for slot in slots:
                        if slot.view.status == ServerStatus.FAILED:
                            slot.view = ServerView()
            except Exception as e:
                log.warning("サーバーの管理に失敗: %s", e)

            for slot in slots:
                try:
                    self._step(slot)
                except Cancelled:
                    pass
                except Exception as e:
                    self._set_view(slot, ServerStatus.FAILED, str(e))

            with self._condition:
                # 待っている間に読み直しを頼まれていたら、次の周で反映してから知らせる
                if not self._reload:
                    self._settled = True

                self._condition.notify_all()

            self._notify()

        self._stop_processes(list(slots))


_manager = _Manager()

start = _manager.start
stop = _manager.stop
reload = _manager.reload
retry = _manager.retry
view = _manager.view
is_chat_available = _manager.is_chat_available
wait_for_embed = _manager.wait_for_embed
