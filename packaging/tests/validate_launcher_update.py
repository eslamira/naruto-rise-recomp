"""Opt-in UI -> verified download -> real Inno update, using a private test AppId.

Build narutorise_launcher and narutorise_update_ui_test first. No release is
packaged or published. The UI fixture streams a locally compiled test installer;
--live in narutorise_update_installer_test separately verifies GitHub transport.
Logs and fixtures remain under out/update-validation-* for review.
"""
import ctypes as c
import atexit
import argparse
from ctypes import wintypes as w
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import tomllib
import uuid
import winreg

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "app/out/build/win-amd64-release"
USER = c.WinDLL("user32", use_last_error=True)
KERNEL = c.WinDLL("kernel32", use_last_error=True)
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
KERNEL.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
KERNEL.CreateToolhelp32Snapshot.restype = w.HANDLE
KERNEL.CloseHandle.argtypes = [w.HANDLE]
KERNEL.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
KERNEL.OpenProcess.restype = w.HANDLE
KERNEL.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
KERNEL.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
USER.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
USER.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
USER.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
USER.IsWindowVisible.argtypes = [w.HWND]
USER.IsWindowEnabled.argtypes = [w.HWND]
USER.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
USER.EnumWindows.argtypes = [CALLBACK, w.LPARAM]
USER.EnumChildWindows.argtypes = [w.HWND, CALLBACK, w.LPARAM]


class ProcessEntry(c.Structure):
    _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD),
                ("heap", c.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                ("parent", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD),
                ("name", w.WCHAR * 260)]


KERNEL.Process32FirstW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
KERNEL.Process32NextW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]


def processes():
    handle = KERNEL.CreateToolhelp32Snapshot(2, 0)
    if handle == c.c_void_p(-1).value:
        raise OSError(c.get_last_error())
    entry = ProcessEntry()
    entry.size = c.sizeof(entry)
    result = []
    ok = KERNEL.Process32FirstW(handle, c.byref(entry))
    while ok:
        result.append((entry.pid, entry.parent, entry.name))
        ok = KERNEL.Process32NextW(handle, c.byref(entry))
    KERNEL.CloseHandle(handle)
    return result


def process_path(pid):
    handle = KERNEL.OpenProcess(0x1000, False, pid)
    if not handle:
        return ""
    buffer = c.create_unicode_buffer(32768)
    size = w.DWORD(len(buffer))
    ok = KERNEL.QueryFullProcessImageNameW(handle, 0, buffer, c.byref(size))
    KERNEL.CloseHandle(handle)
    return buffer.value if ok else ""


def windows(parent=None):
    result = []

    @CALLBACK
    def visit(hwnd, unused):
        pid = w.DWORD()
        USER.GetWindowThreadProcessId(hwnd, c.byref(pid))
        text, cls = c.create_unicode_buffer(8192), c.create_unicode_buffer(256)
        USER.GetWindowTextW(hwnd, text, len(text))
        USER.GetClassNameW(hwnd, cls, len(cls))
        result.append({"hwnd": hwnd, "pid": pid.value, "class": cls.value, "text": text.value,
                       "visible": bool(USER.IsWindowVisible(hwnd)), "enabled": bool(USER.IsWindowEnabled(hwnd))})
        return True

    if parent is None:
        USER.EnumWindows(visit, 0)
    else:
        USER.EnumChildWindows(parent, visit, 0)
    return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def registration(key):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as handle:
            result = {}
            for i in range(winreg.QueryInfoKey(handle)[1]):
                name, value, kind = winreg.EnumValue(handle, i)
                result[name] = value.hex() if isinstance(value, bytes) else value
            return result
    except FileNotFoundError:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable", action="store_true", help="Exercise explicit portable conversion consent")
    args = parser.parse_args()
    run = ROOT / "out" / ("update-validation-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
    run.mkdir(parents=True)
    if not args.portable:
        (run / "registered-mode").touch()
    print(f"Validation directory: {run}", flush=True)
    appid = "{" + str(uuid.uuid4()).upper() + "}"
    test_key = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + appid + "_is1"
    prod_key = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{E8B610A5-927A-412E-9C64-42841FA6DE51}_is1"
    prod_before = registration(prod_key)
    prod_files = {}
    if prod_before:
        prod_dir = Path(prod_before["Inno Setup: App Path"])
        for name in ("narutorise.exe", "narutorise_launcher.exe", "narutorise.toml", "narutorise.version"):
            path = prod_dir / name
            if path.is_file():
                prod_files[str(path)] = sha(path)
    (run / "production-before.json").write_text(json.dumps({"registry": prod_before, "files": prod_files}, indent=2), encoding="utf-8")
    dest = run / "instalação teste"
    (run / "fixture.json").write_text(json.dumps({"app_id": appid, "destination": str(dest),
        "uninstall_key": test_key}, indent=2), encoding="utf-8")

    def cleanup():
        # Only processes rooted in this invocation's unique fixture directory.
        snapshot = processes()
        owned = {pid for pid, _, _ in snapshot if process_path(pid).lower().startswith(str(run).lower() + "\\")}
        for _ in range(4):
            owned.update(pid for pid, parent, _ in snapshot if parent in owned)
        for pid in owned:
            handle = KERNEL.OpenProcess(1, False, pid)
            if handle:
                KERNEL.TerminateProcess(handle, 0)
                KERNEL.CloseHandle(handle)
        if owned:
            time.sleep(.3)
        uninstall_path = dest / "unins000.exe"
        if uninstall_path.is_file():
            subprocess.run([str(uninstall_path), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=60)
        unchanged = registration(prod_key) == prod_before and all(sha(Path(path)) == digest for path, digest in prod_files.items())
        (run / "cleanup.json").write_text(json.dumps({"test_registration_removed": registration(test_key) is None,
            "production_unchanged": unchanged}, indent=2), encoding="utf-8")

    atexit.register(cleanup)
    compiler = Path(os.environ["LOCALAPPDATA"]) / "Programs/Inno Setup 6/ISCC.exe"
    source = (ROOT / "packaging/installer.iss").read_text(encoding="utf-8")
    source = source.replace("..\\launcher", str(ROOT / "launcher")).replace("..\\tools", str(ROOT / "tools"))
    source = source.replace("DefaultGroupName=NarutoRisePC", "DefaultGroupName=NarutoRiseTest-" + appid.strip("{}"))
    source = re.sub(r"(?ms)^\[Icons\]\n.*?(?=^\[Run\])", "", source)  # Never touch real shortcuts.
    (run / "installer.iss").write_text(source, encoding="utf-8")
    shutil.copytree(ROOT / "packaging/locales", run / "locales")
    shutil.copy2(ROOT / "packaging/version.iss", run / "version.iss")
    package = run / "new-package"
    package.mkdir()
    for name in ("narutorise.exe", "narutorise_ai2c.dll", "rexruntime.dll", "rexgpu-xenos.dll"):
        (package / name).write_bytes(("synthetic NEW binary: " + name).encode())
    shutil.copy2(BUILD / "narutorise_launcher.exe", package / "narutorise_launcher.exe")
    for subdir in ("shader_cache", "licenses", "mods/pt-br"):
        (package / subdir).mkdir(parents=True)
    (package / "shader_cache/555307E5.xsh").write_bytes(b"synthetic shader cache")
    (package / "licenses/test.txt").write_text("Test fixture only")
    shutil.copy2(ROOT / "mods/pt-br/mod.toml", package / "mods/pt-br/mod.toml")
    shutil.copy2(ROOT / "mods/pt-br/texts.json", package / "mods/pt-br/texts.json")
    (package / "README.txt").write_text("LOCAL VALIDATION FIXTURE - NOT A RELEASE")
    (package / "narutorise.toml").write_text('fullscreen = true\nlauncher_language = "en"\nuser_language = 1\n')
    old = run / "old-package"
    shutil.copytree(package, old)
    (old / "narutorise.exe").write_bytes(b"synthetic OLD binary")
    shutil.copy2(BUILD / "narutorise_update_ui_test.exe", old / "narutorise_launcher.exe")

    for name, version, payload in (("old", "0.2.0-beta.1", old), ("new", "0.2.0-beta.2", package)):
        output = run / name
        output.mkdir()
        result = subprocess.run([str(compiler), "/Q", f"/O{output}", f"/DPackageDir={payload}",
            f"/DHelperPath={BUILD / 'narutorise_setup_helper.exe'}", f"/DMyAppVersion={version}",
            f"/DSetupAppId={appid.replace('{', '{{')}",
            f"/DLegacyUninstallKey=Software\\NarutoRiseTestLegacy\\{appid}",
            f"/DDefaultInstallDir={dest}", str(run / "installer.iss")], capture_output=True, timeout=120)
        (run / f"compile-{name}.log").write_bytes(result.stdout + result.stderr)
        result.check_returncode()
        print(f"Compiled private {version} test installer", flush=True)
    new_setup = run / "new/NarutoRiseInstaller.exe"
    manifest = {"tag_name": "v0.2.0-beta.2", "draft": False, "prerelease": False,
        "assets": [{"name": "NarutoRiseInstaller.exe", "state": "uploaded", "size": new_setup.stat().st_size,
                    "digest": "sha256:" + sha(new_setup), "browser_download_url":
                    "https://github.com/jorjbarbosa/naruto-rise-recomp/releases/download/v0.2.0-beta.2/NarutoRiseInstaller.exe"}]}
    (run / "test-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    result = subprocess.run([str(run / "old/NarutoRiseInstaller.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES",
        "/NORESTART", "/NOICONS", "/TASKS=", "/MODE=install", f"/DIR={dest}",
        f"/LOG={run / 'initial-install.log'}"], timeout=60)
    result.check_returncode()
    assert registration(test_key)["DisplayVersion"] == "0.2.0-beta.1"

    files = {
        "game/default.xex": b"XEX2 synthetic data", "game/original.bin": b"original files",
        "game_root/default.xex": b"legacy original data", "dlc/pending.bin": b"pending DLC",
        "personal.txt": b"personal data", "logs/session.log": b"existing log",
        "mods/community/mod.toml": b'[mod]\nname = "Personal"\n',
        "mods/community/texts.json": b'{"1":"Personal translation"}',
        "mods/pt-br/review-notes.txt": b"personal bundled mod notes",
        "test-user-data/saves/save.bin": b"save data",
        "test-user-data/0000000000000000/555307E5/00000002/dlc.bin": b"installed DLC",
    }
    for name, data in files.items():
        path = dest / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (dest / "narutorise.toml").write_text('fullscreen = false\nlauncher_language = "pt_BR"\nuser_language = 3\n'
        f'user_data_root = "{(dest / "test-user-data").as_posix()}"\npersonal_setting = "preserve"\n', encoding="utf-8")
    original_options = tomllib.loads((dest / "narutorise.toml").read_text(encoding="utf-8"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    probe_output = (run / "launcher-output.log").open("wb")
    probe = subprocess.Popen([str(dest / "narutorise_launcher.exe")], cwd=dest, startupinfo=startup,
                             stdout=probe_output, stderr=subprocess.STDOUT)
    print(f"Driving the real launcher UI in PID {probe.pid}", flush=True)
    known = {probe.pid}
    reopened = None
    before_setup = None
    clicked = []
    deadline = time.monotonic() + 100
    while time.monotonic() < deadline:
        all_processes = processes()
        for _ in range(4):
            known.update(pid for pid, parent, _ in all_processes if parent in known)
        if probe.poll() is not None:
            events = (run / "launcher-events.log").read_text().splitlines()
            assert probe.returncode == 0 and "installer_handoff_started" in events, events
            if before_setup is None:
                before_setup = (dest / "narutorise.toml").read_bytes()
                print("Both UI confirmations exercised; launcher closed after installer handoff", flush=True)
        for window in windows():
            if window["pid"] not in known:
                continue
            if window["class"] == "TWizardForm":
                controls = windows(window["hwnd"])
                buttons = [item for item in controls if item["visible"] and item["enabled"] and
                           item["class"] == "TNewButton" and item["text"].replace("&", "").strip().rstrip(" >") in
                           ("Avançar", "Instalar", "Concluir", "Next", "Install", "Finish")]
                if buttons and before_setup is not None:
                    button = buttons[0]
                    caption = button["text"].replace("&", "")
                    page = [item["text"] for item in controls if item["visible"] and item["text"]]
                    with (run / "wizard-pages.jsonl").open("a", encoding="utf-8") as output:
                        output.write(json.dumps({"button": caption, "page": page}, ensure_ascii=False) + "\n")
                    clicked.append(caption)
                    USER.PostMessageW(button["hwnd"], 0xF5, 0, 0)  # BM_CLICK, scoped to this test's child.
                    time.sleep(.4)
            elif window["class"] == "SDL_app" and window["pid"] != probe.pid:
                if Path(process_path(window["pid"])) == dest / "narutorise_launcher.exe":
                    reopened = window
        if reopened:
            break
        time.sleep(.1)
    assert reopened, "Updated launcher did not reopen; inspect logs in " + str(run)
    assert before_setup == (dest / "narutorise.toml").read_bytes(), "Setup changed user configuration"
    after_options = tomllib.loads((dest / "narutorise.toml").read_text(encoding="utf-8"))
    for key, value in original_options.items():
        assert after_options[key] == value, key
    for name, data in files.items():
        assert (dest / name).read_bytes() == data, name
    assert (dest / "narutorise.version").read_text().strip() == "0.2.0-beta.2"
    for name in ("narutorise.exe", "narutorise_launcher.exe", "rexruntime.dll", "mods/pt-br/mod.toml", "mods/pt-br/texts.json"):
        assert sha(dest / name) == sha(package / name), name
    assert registration(test_key)["DisplayVersion"] == "0.2.0-beta.2"
    USER.PostMessageW(reopened["hwnd"], 0x10, 0, 0)  # WM_CLOSE on the reopened test launcher only.
    for _ in range(50):
        if not process_path(reopened["pid"]):
            break
        time.sleep(.1)
    uninstall = subprocess.run([str(dest / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=60)
    uninstall.check_returncode()
    assert registration(test_key) is None
    for name, data in files.items():
        assert (dest / name).read_bytes() == data, "uninstall: " + name
    assert registration(prod_key) == prod_before, "Production registry changed"
    for path, digest in prod_files.items():
        assert sha(Path(path)) == digest, "Production file changed: " + path
    report = {"result": "passed", "mode": "portable" if args.portable else "registered",
              "from": "0.2.0-beta.1", "to": "0.2.0-beta.2", "test_app_id": appid,
              "ui_events": (run / "launcher-events.log").read_text().splitlines(),
              "installer_buttons": clicked, "launcher_reopened": True, "preserved_files": list(files),
              "configuration_values_preserved": True, "setup_preserved_config_bytes": True,
              "production_unchanged": True, "test_uninstalled": True,
              "transport": "local fixture through production download/verification pipeline; GitHub verified separately"}
    (run / "result.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
