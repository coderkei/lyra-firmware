#!/usr/bin/env python3
"""Local HTTP server and launcher for the Lyra WASM emulator."""

import argparse
import gzip
import http.server
import mimetypes
import os
import re
import shutil
import sys
import threading
import uuid
import urllib.parse
import webbrowser
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
EMULATOR_ROOT = SCRIPT_DIR.parent
APP_ROOT = EMULATOR_ROOT / "app"
STORAGE_ROOT = EMULATOR_ROOT / "storage"
SD_PATH = STORAGE_ROOT / "virtual-sd.img"
FLASH_PATH = STORAGE_ROOT / "internal-flash.bin"
FIRMWARE_HASH_PATH = STORAGE_ROOT / "firmware.sha256"
DEFAULT_FIRMWARE_PATH = EMULATOR_ROOT.parent.parent / "lyra_firmware_merged.bin"


class EmulatorServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, handler, firmware_path):
        super().__init__(address, handler)
        self.firmware_path = firmware_path
        self.storage_lock = threading.RLock()
        self.flash_generation = 0


class EmulatorHandler(http.server.BaseHTTPRequestHandler):
    server_version = "LyraEmulator/1.0"

    def log_message(self, _format, *_args):
        # The launcher runs the local server in the background.
        pass

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        super().end_headers()

    def _content_length(self):
        if self.headers.get("Transfer-Encoding"):
            raise ValueError("Chunked request bodies are not supported.")
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise ValueError("Invalid Content-Length.") from error
        if length < 0:
            raise ValueError("Invalid Content-Length.")
        return length

    def _read_exact(self, count):
        chunks = []
        remaining = count
        while remaining:
            chunk = self.rfile.read(min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError("The request body ended unexpectedly.")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _send(self, status, body=b"", content_type="text/plain; charset=utf-8", content_encoding=None, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if content_encoding:
            self.send_header("Content-Encoding", content_encoding)
        for name, value in (extra_headers or {}).items():
            self.send_header(name, str(value))
        self.end_headers()
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def _send_file(self, path, content_type=None, content_encoding=None):
        size = path.stat().st_size
        if content_type is None:
            content_type = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
            if path.suffix.lower() in (".js", ".mjs"):
                content_type = "text/javascript; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        if content_encoding:
            self.send_header("Content-Encoding", content_encoding)
        self.end_headers()
        if self.command != "HEAD":
            with path.open("rb") as source:
                shutil.copyfileobj(source, self.wfile, 1024 * 1024)

    def _send_text(self, status, text):
        self._send(status, text.encode("utf-8"))

    def _ensure_sd_card(self):
        if SD_PATH.is_file():
            return
        compressed_path = APP_ROOT / "assets" / "lyra-demo-sd.img.gz"
        if not compressed_path.is_file():
            raise FileNotFoundError("The default virtual MicroSD image is missing.")
        temporary_path = STORAGE_ROOT / (".virtual-sd-" + uuid.uuid4().hex + ".tmp")
        try:
            with gzip.open(compressed_path, "rb") as source, temporary_path.open("wb") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)
            size = temporary_path.stat().st_size
            if size < 512 or size % 512:
                raise ValueError("The default virtual MicroSD image has an invalid size.")
            os.replace(temporary_path, SD_PATH)
        finally:
            temporary_path.unlink(missing_ok=True)

    def _save_request_file(self, destination, minimum, maximum, alignment=1):
        length = self._content_length()
        if length < minimum or length > maximum or length % alignment:
            raise ValueError("The uploaded storage image has an invalid size.")
        temporary_path = STORAGE_ROOT / (".upload-" + uuid.uuid4().hex + ".tmp")
        try:
            remaining = length
            with temporary_path.open("xb") as target:
                while remaining:
                    chunk = self.rfile.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError("The request body ended unexpectedly.")
                    target.write(chunk)
                    remaining -= len(chunk)
                target.flush()
                os.fsync(target.fileno())
            os.replace(temporary_path, destination)
        finally:
            temporary_path.unlink(missing_ok=True)

    def _apply_sd_patches(self, length):
        if not SD_PATH.is_file() or length % 520:
            raise ValueError("Invalid MicroSD patch request.")
        with SD_PATH.open("r+b") as card:
            for _ in range(length // 520):
                record = self._read_exact(520)
                sector = int.from_bytes(record[:4], "little")
                offset = sector * 512
                if offset + 512 > os.fstat(card.fileno()).st_size:
                    raise ValueError("A MicroSD patch is outside the card image.")
                card.seek(offset)
                card.write(record[8:520])
            card.flush()
            os.fsync(card.fileno())

    def _apply_flash_patches(self, length):
        if not FLASH_PATH.is_file() or length % 4104:
            raise ValueError("Invalid internal flash patch request.")
        with FLASH_PATH.open("r+b") as flash:
            flash_size = os.fstat(flash.fileno()).st_size
            for _ in range(length // 4104):
                record = self._read_exact(4104)
                offset = int.from_bytes(record[:4], "little")
                if offset % 4096 or offset + 4096 > flash_size:
                    raise ValueError("An internal flash patch is outside the flash image.")
                flash.seek(offset)
                flash.write(record[8:4104])
            flash.flush()
            os.fsync(flash.fileno())

    def _handle_get(self):
        path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        if path == "/api/sd-card":
            with self.server.storage_lock:
                self._ensure_sd_card()
                self._send_file(SD_PATH)
            return
        if path == "/api/internal-flash":
            with self.server.storage_lock:
                if not FLASH_PATH.is_file():
                    self._send_text(404, "Not found")
                    return
                self._send_file(FLASH_PATH)
            return
        if path == "/api/flash-generation":
            with self.server.storage_lock:
                self._send_text(200, str(self.server.flash_generation))
            return
        if path == "/api/firmware-hash":
            with self.server.storage_lock:
                if not FIRMWARE_HASH_PATH.is_file():
                    self._send_text(404, "Not found")
                    return
                self._send_file(FIRMWARE_HASH_PATH, "text/plain; charset=utf-8")
            return
        if path == "/firmware.bin":
            if not self.server.firmware_path or not self.server.firmware_path.is_file():
                self._send_text(404, "Firmware image unavailable")
                return
            self._send_file(self.server.firmware_path)
            return

        relative = "index.html" if path == "/" else path.lstrip("/")
        candidate = (APP_ROOT / relative).resolve()
        try:
            candidate.relative_to(APP_ROOT.resolve())
        except ValueError:
            self._send_text(404, "Not found")
            return
        if not candidate.is_file():
            self._send_text(404, "Not found")
            return
        if candidate.name.lower() == "esp32sim.wasm.gz":
            self._send_file(candidate, "application/wasm", "gzip")
        else:
            self._send_file(candidate)

    def do_GET(self):
        try:
            self._handle_get()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self._send_text(400, str(error))

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        response_headers = None
        try:
            with self.server.storage_lock:
                length = self._content_length()
                if path == "/api/sd-replace":
                    self._save_request_file(SD_PATH, 512, 2147483648, 512)
                elif path == "/api/sd-patches":
                    self._apply_sd_patches(length)
                elif path == "/api/flash-init":
                    if FLASH_PATH.exists():
                        self._send_text(409, "Flash image already exists")
                        return
                    self._save_request_file(FLASH_PATH, 1, 33554432)
                elif path == "/api/flash-replace":
                    self._save_request_file(FLASH_PATH, 16777216, 16777216)
                    self.server.flash_generation += 1
                    response_headers = {"X-Flash-Generation": self.server.flash_generation}
                elif path == "/api/firmware-hash":
                    if length != 64:
                        raise ValueError("Invalid firmware fingerprint.")
                    fingerprint = self._read_exact(64)
                    if not re.fullmatch(rb"[a-f0-9]{64}", fingerprint):
                        raise ValueError("Invalid firmware fingerprint.")
                    FIRMWARE_HASH_PATH.write_bytes(fingerprint)
                elif path == "/api/flash-patches":
                    try:
                        generation = int(self.headers.get("X-Flash-Generation", ""))
                    except ValueError:
                        generation = -1
                    if generation != self.server.flash_generation:
                        self._send_text(409, "Flash changed; stale writes were discarded.")
                        return
                    self._apply_flash_patches(length)
                else:
                    self._send_text(404, "Not found")
                    return
            self._send(204, b"", extra_headers=response_headers)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self._send_text(400, str(error))


def choose_firmware(path):
    if path:
        firmware = Path(path).expanduser().resolve()
        if not firmware.is_file():
            raise FileNotFoundError("Firmware file not found: " + str(firmware))
        return firmware
    return DEFAULT_FIRMWARE_PATH if DEFAULT_FIRMWARE_PATH.is_file() else None


def launch(firmware_argument):
    firmware = choose_firmware(firmware_argument)
    STORAGE_ROOT.mkdir(parents=True, exist_ok=True)
    server = EmulatorServer(("127.0.0.1", 0), EmulatorHandler, firmware)
    try:
        url = f"http://127.0.0.1:{server.server_address[1]}/"
        if firmware:
            url += "?name=" + urllib.parse.quote(firmware.name)
        print("Emulator server is running: " + url, flush=True)
        print("Keep this window open. Press Ctrl+C here to stop the server.", flush=True)
        if not webbrowser.open(url, new=2):
            print("Open the URL above in a browser to use the emulator.")
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        print("\nEmulator server stopped.", flush=True)
    finally:
        server.server_close()


def main():
    if sys.version_info < (3, 8):
        raise RuntimeError("Python 3.8 or later is required to run the emulator.")
    parser = argparse.ArgumentParser(description="Launch the local Lyra firmware emulator.")
    parser.add_argument("firmware", nargs="?", help="optional merged firmware .bin image")
    arguments = parser.parse_args()
    launch(arguments.firmware)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("Emulator launch failed: " + str(error), file=sys.stderr)
        sys.exit(1)
