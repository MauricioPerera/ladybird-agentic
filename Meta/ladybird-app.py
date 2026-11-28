#!/usr/bin/env python3
"""Developer CLI for initializing, validating, running, and packaging Ladybird App Mode apps."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from urllib.parse import urlparse


APP_ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
SUBDIR_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def sanitize_app_id(raw: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", raw.lower()).strip("-")[:64]
    if not slug or not APP_ID_RE.match(slug):
        raise ValueError(f"Cannot derive valid app id from {raw!r}")
    return slug


def source_bound_storage_id(app_id: str, source_kind: str, canonical_source: str) -> str:
    prefix = f"ladybird-app-storage:v1:{source_kind}:".encode("ascii")
    encoded_source = os.fsencode(canonical_source) if source_kind == "local" else canonical_source.encode("utf-8")
    source = prefix + encoded_source
    source_digest = hashlib.sha256(source).hexdigest()[:32]
    return f"{app_id[:31]}-{source_digest}"


def canonical_remote_origin(parsed_url) -> str:
    host = parsed_url.hostname
    if not host:
        raise ValueError("Remote app URL must include a host")
    try:
        host = host.encode("idna").decode("ascii")
    except UnicodeError as error:
        raise ValueError("Remote app URL contains an invalid host") from error
    serialized_host = f"[{host}]" if ":" in host else host
    port = parsed_url.port
    if port is not None and not ((parsed_url.scheme == "http" and port == 80) or (parsed_url.scheme == "https" and port == 443)):
        serialized_host += f":{port}"
    return f"{parsed_url.scheme.lower()}://{serialized_host.lower()}"


def resolve_confined_file(root: Path, rel_path: str) -> Path:
    if not rel_path or ":" in rel_path or "\x00" in rel_path:
        raise ValueError(f"Invalid relative path: {rel_path!r}")
    if rel_path.startswith(("/", "\\")):
        raise ValueError(f"Path must be relative: {rel_path!r}")
    parts = [p for p in re.split(r"[\\/]+", rel_path) if p and p != "."]
    if not parts or any(p == ".." for p in parts):
        raise ValueError(f"Path traversal not allowed: {rel_path!r}")
    root_real = root.resolve(strict=True)
    target = (root_real.joinpath(*parts)).resolve(strict=True)
    if not target.is_relative_to(root_real):
        raise ValueError(f"Path escapes app root: {rel_path!r}")
    if not target.is_file():
        raise ValueError(f"Path is not a file: {rel_path!r}")
    return target


def validate_manifest_path(target: Path) -> tuple[Path, dict]:
    manifest_file = target / "ladybird-app.json" if target.is_dir() else target
    if not manifest_file.is_file():
        raise ValueError(f"Manifest not found: {manifest_file}")
    root = manifest_file.parent.resolve(strict=True)
    data = json.loads(manifest_file.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Manifest JSON root must be an object")

    name = data.get("name")
    if not isinstance(name, str) or not name.strip() or len(name) > 128:
        raise ValueError("Manifest requires non-empty string 'name' (max 128 chars)")

    app_id = data.get("id")
    if app_id is not None:
        if not isinstance(app_id, str) or not APP_ID_RE.match(app_id):
            raise ValueError("Manifest 'id' must match ^[a-z0-9][a-z0-9_-]{0,63}$")
    else:
        app_id = sanitize_app_id(name)

    entry = data.get("entry", "index.html")
    if not isinstance(entry, str) or not entry:
        raise ValueError("Manifest 'entry' must be a non-empty relative path")
    resolve_confined_file(root, entry)

    icon = data.get("icon")
    if icon is not None:
        if not isinstance(icon, str) or not icon:
            raise ValueError("Manifest 'icon' must be a non-empty relative path")
        resolve_confined_file(root, icon)

    permissions = data.get("permissions", {})
    if permissions is not None:
        if not isinstance(permissions, dict):
            raise ValueError("Manifest 'permissions' must be an object")
        for key in ("fsRead", "fsWrite"):
            subdirs = permissions.get(key, [])
            if not isinstance(subdirs, list):
                raise ValueError(f"permissions.{key} must be an array")
            for sub in subdirs:
                if not isinstance(sub, str) or not SUBDIR_RE.match(sub):
                    raise ValueError(f"Invalid permissions.{key} entry {sub!r}")
        if "dynamicServices" in permissions and permissions["dynamicServices"] is not None and not isinstance(permissions["dynamicServices"], bool):
            raise ValueError("permissions.dynamicServices must be a boolean")

    mcp = data.get("mcp", {})
    if mcp is not None:
        if not isinstance(mcp, dict):
            raise ValueError("Manifest 'mcp' must be an object")
        access = mcp.get("access")
        if access is not None:
            if access not in ("full", "webmcp-only"):
                raise ValueError("Manifest 'mcp.access' must be 'full' or 'webmcp-only'")
            if access == "webmcp-only" and mcp.get("allowWebMCP", True) is False:
                raise ValueError("Manifest 'mcp.access' = 'webmcp-only' requires 'mcp.allowWebMCP' to be true")

    window = data.get("window")
    if window is not None:
        if not isinstance(window, dict):
            raise ValueError("Manifest 'window' must be an object")
        if "showTabBar" in window and window["showTabBar"] is not None and not isinstance(window["showTabBar"], bool):
            raise ValueError("Manifest 'window.showTabBar' must be a boolean")

    services = data.get("services")
    if services is not None:
        if not isinstance(services, dict):
            raise ValueError("Manifest 'services' must be an object")
        for svc_id, cfg in services.items():
            if not isinstance(svc_id, str) or not APP_ID_RE.match(svc_id):
                raise ValueError(f"Invalid service id {svc_id!r}")
            if not isinstance(cfg, dict):
                raise ValueError(f"Service '{svc_id}' config must be an object")
            def_url = cfg.get("defaultUrl")
            if not isinstance(def_url, str) or not def_url:
                raise ValueError(f"Service '{svc_id}' requires non-empty 'defaultUrl'")
            p_def = urlparse(def_url)
            if p_def.scheme not in ("http", "https") or not p_def.netloc:
                raise ValueError(f"Service '{svc_id}' defaultUrl must be http or https")
            origins = cfg.get("origins")
            if not isinstance(origins, list) or not origins:
                raise ValueError(f"Service '{svc_id}' requires a non-empty 'origins' array")
            norm_origins: list[str] = []
            for o in origins:
                if not isinstance(o, str) or not o:
                    raise ValueError(f"Service '{svc_id}' origins entries must be non-empty strings")
                po = urlparse(o)
                if po.scheme not in ("http", "https") or not po.netloc:
                    raise ValueError(f"Service '{svc_id}' origin {o!r} must be http or https")
                norm_origins.append(f"{po.scheme.lower()}://{po.netloc.lower()}")
            def_origin = f"{p_def.scheme.lower()}://{p_def.netloc.lower()}"
            if def_origin not in norm_origins:
                raise ValueError(f"Service '{svc_id}' defaultUrl origin {def_origin!r} must be listed in 'origins'")
            perms = cfg.get("permissions")
            if perms is not None:
                if not isinstance(perms, list) or any(p not in ("open", "navigate", "notifications", "eval", "flows", "network") for p in perms):
                    raise ValueError(f"Service '{svc_id}' permissions must be a subset of open, navigate, notifications, eval, flows, network")
            container = cfg.get("container")
            if container is not None:
                if not isinstance(container, str) or (container and not APP_ID_RE.match(container)):
                    raise ValueError(f"Service '{svc_id}' container must be an empty string or valid container id")

    return manifest_file, {**data, "id": app_id}


def find_ladybird_binary(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
        raise FileNotFoundError(f"Ladybird binary not found: {explicit}")

    repo_root = Path(__file__).resolve().parent.parent
    candidates = [
        repo_root / "Build" / "release" / "bin" / "Ladybird.exe",
        repo_root / "Build" / "release" / "bin" / "Ladybird",
        repo_root / "Build" / "debug" / "bin" / "Ladybird",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    for exe_name in ("Ladybird", "Ladybird.exe", "ladybird"):
        found = shutil.which(exe_name)
        if found:
            return Path(found)
    raise FileNotFoundError("Could not locate Ladybird executable; pass --ladybird <path>")


def cmd_init(args: argparse.Namespace) -> int:
    target = Path(args.directory).resolve()
    target.mkdir(parents=True, exist_ok=True)
    app_name = args.name or target.name or "My Ladybird App"
    app_id = args.id or sanitize_app_id(app_name)

    manifest = {
        "id": app_id,
        "name": app_name,
        "version": "0.1.0",
        "entry": "index.html",
        "window": {"width": 1024, "height": 680, "minWidth": 520, "minHeight": 360, "resizable": True},
        "mcp": {"enabled": True, "allowWebMCP": True},
        "permissions": {"fsRead": ["data"], "fsWrite": ["data"], "systemInfo": True},
    }
    (target / "ladybird-app.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (target / "index.html").write_text(
        f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>{app_name}</title>
    <script src="/__ladybird_bridge.js"></script>
  </head>
  <body style="font-family: sans-serif; background:#09090b; color:#f4f4f5; padding:32px;">
    <h1>{app_name}</h1>
    <pre id="status">Loading bridge...</pre>
    <script>
      (async () => {{
        const info = await window.ladybirdApp.system.info();
        document.getElementById('status').textContent = JSON.stringify(info, null, 2);
      }})();
    </script>
  </body>
</html>
""",
        encoding="utf-8",
    )
    print(f"Initialized Ladybird app '{app_name}' ({app_id}) in {target}")
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    manifest_file, data = validate_manifest_path(Path(args.target))
    print(f"VALID {manifest_file} (id={data['id']}, name={data['name']})")
    return 0


def resolve_app_identity(target_str: str) -> tuple[str, str, str]:
    parsed_url = urlparse(target_str)
    if parsed_url.scheme in ("http", "https") and parsed_url.netloc:
        canonical_origin = canonical_remote_origin(parsed_url)
        host = urlparse(canonical_origin).hostname or "remote-app"
        try:
            app_id = sanitize_app_id(host)
        except ValueError:
            app_id = "remote-app"
        storage_id = source_bound_storage_id(app_id, "remote", canonical_origin)
        return app_id, host, storage_id

    target = Path(target_str).resolve(strict=True)
    if target.is_dir():
        manifest_file = target / "ladybird-app.json"
        if manifest_file.is_file():
            _, data = validate_manifest_path(manifest_file)
            app_id = data["id"]
            canonical_root = manifest_file.resolve(strict=True).parent
            return app_id, data["name"], source_bound_storage_id(app_id, "local", str(canonical_root))
        try:
            app_id = sanitize_app_id(target.name)
        except ValueError:
            app_id = "local-app"
        return app_id, target.name, source_bound_storage_id(app_id, "local", str(target))

    _, data = validate_manifest_path(target)
    app_id = data["id"]
    canonical_root = target.parent.resolve(strict=True)
    return app_id, data["name"], source_bound_storage_id(app_id, "local", str(canonical_root))


def resolve_app_id(target_str: str) -> tuple[str, str]:
    app_id, app_name, _ = resolve_app_identity(target_str)
    return app_id, app_name


def ladybird_instance_data_dir() -> Path:
    if sys.platform == "win32":
        root = Path(os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData" / "Local"))
        return root / "Ladybird"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "Ladybird"
    xdg_data = os.environ.get("XDG_DATA_HOME")
    base = Path(xdg_data) if xdg_data else (Path.home() / ".local" / "share")
    return base / "Ladybird"


def app_runtime_dir() -> Path:
    return ladybird_instance_data_dir() / "AppRuntime"


def runtime_descriptor_path(storage_id: str, account_id: str) -> Path:
    return app_runtime_dir() / f"{storage_id}--{account_id}.json"


def is_pid_alive(pid: int | None) -> bool:
    if not pid or pid <= 0:
        return False
    if sys.platform == "win32":
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            if not ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return False
            return code.value == STILL_ACTIVE
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    try:
        os.kill(int(pid), 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def probe_mcp_ping(mcp_url: str | None, token: str | None, timeout: float = 2.0) -> bool:
    if not mcp_url or not token:
        return False
    req = urllib.request.Request(
        mcp_url,
        data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"}).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status != 200:
                return False
            raw = resp.read().decode("utf-8", errors="replace")
    except Exception:
        return False
    for line in raw.splitlines():
        if line.startswith("data: "):
            raw = line[6:]
            break
    try:
        parsed = json.loads(raw)
        return "result" in parsed
    except Exception:
        return False


def inspect_runtime_record(path: Path) -> dict | None:
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if not isinstance(data, dict):
        return None
    pid = data.get("pid")
    ui_pid = data.get("uiPid")
    pid_alive = is_pid_alive(pid) or is_pid_alive(ui_pid)
    mcp_enabled = bool(data.get("mcpEnabled"))
    mcp_live = probe_mcp_ping(data.get("mcpUrl"), data.get("mcpToken")) if mcp_enabled else True
    running = bool(pid_alive and mcp_live)
    return {
        **data,
        "descriptorPath": str(path),
        "pidAlive": pid_alive,
        "mcpLive": mcp_live,
        "running": running,
        "stale": not running,
    }


def _kill_pid(pid: int, force: bool = False) -> None:
    if not is_pid_alive(pid):
        return
    if sys.platform == "win32":
        cmd = ["taskkill", "/PID", str(pid), "/T"]
        if force:
            cmd.append("/F")
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        sig = signal.SIGKILL if force else signal.SIGTERM
        try:
            os.kill(int(pid), sig)
        except ProcessLookupError:
            pass


def terminate_runtime_instance(record: dict, timeout: float = 8.0) -> bool:
    pids = [p for p in (record.get("uiPid"), record.get("pid")) if isinstance(p, int) and p > 0]
    for force in (False, True):
        for p in pids:
            _kill_pid(p, force=force)
        deadline = time.time() + (timeout * 0.65 if not force else timeout * 0.35)
        while time.time() < deadline:
            if not any(is_pid_alive(p) for p in pids):
                desc = record.get("descriptorPath")
                if desc:
                    try:
                        Path(desc).unlink(missing_ok=True)
                    except Exception:
                        pass
                return True
            time.sleep(0.2)
    desc = record.get("descriptorPath")
    if desc and not any(is_pid_alive(p) for p in pids):
        try:
            Path(desc).unlink(missing_ok=True)
        except Exception:
            pass
    return not any(is_pid_alive(p) for p in pids)


def build_launch_command(
    target_str: str,
    binary: Path,
    account: str | None = None,
    profile: str | None = None,
    mcp_port: int | None = None,
    headless: bool = False,
    webmcp_only: bool = False,
    temporary_profile: bool = False,
) -> list[str]:
    cmd = [str(binary)]
    parsed_url = urlparse(target_str)
    if parsed_url.scheme in ("http", "https") and parsed_url.netloc:
        cmd.extend(["--app", target_str])
    else:
        target = Path(target_str).resolve()
        if target.is_dir():
            manifest_file = target / "ladybird-app.json"
            if manifest_file.is_file():
                validate_manifest_path(manifest_file)
                cmd.extend(["--app-manifest", str(manifest_file)])
            else:
                cmd.extend(["--app-dir", str(target)])
        else:
            validate_manifest_path(target)
            cmd.extend(["--app-manifest", str(target)])

    if account:
        if not APP_ID_RE.match(account):
            raise ValueError("--account must match ^[a-z0-9][a-z0-9_-]{0,63}$")
        cmd.extend(["--account", account])
    if profile:
        cmd.extend(["--profile", profile])
    if mcp_port is not None:
        cmd.append(f"--mcp={mcp_port}")
    if webmcp_only:
        cmd.append("--mcp-webmcp-only")
    if headless:
        cmd.append("--headless=manual")
    if temporary_profile:
        cmd.append("--temporary-profile")
    return cmd


def cmd_run(args: argparse.Namespace) -> int:
    binary = find_ladybird_binary(args.ladybird)
    cmd = build_launch_command(
        args.target,
        binary,
        account=args.account,
        profile=getattr(args, "profile", None),
        mcp_port=args.mcp,
        headless=args.headless,
        webmcp_only=getattr(args, "webmcp_only", False),
        temporary_profile=args.temporary_profile,
    )
    print("Running:", " ".join(cmd))
    return subprocess.call(cmd)


def cmd_status(args: argparse.Namespace) -> int:
    app_id, app_name, storage_id = resolve_app_identity(args.target)
    records: list[dict] = []
    if args.account:
        if not APP_ID_RE.match(args.account):
            raise ValueError("--account must match ^[a-z0-9][a-z0-9_-]{0,63}$")
        path = runtime_descriptor_path(storage_id, args.account)
        rec = inspect_runtime_record(path)
        if rec:
            records.append(rec)
    else:
        rdir = app_runtime_dir()
        if rdir.is_dir():
            for path in sorted(rdir.glob(f"{storage_id}--*.json")):
                rec = inspect_runtime_record(path)
                if rec:
                    records.append(rec)

    if args.json:
        if args.account:
            payload = records[0] if records else {
                "appId": app_id,
                "accountId": args.account,
                "running": False,
                "descriptorPath": str(runtime_descriptor_path(storage_id, args.account)),
            }
            print(json.dumps(payload, indent=2))
        else:
            print(json.dumps({"appId": app_id, "name": app_name, "instances": records}, indent=2))
        return 0

    print(f"Runtime status for '{app_name}' ({app_id}):")
    if not records:
        acct_note = f" (account '{args.account}')" if args.account else ""
        print(f"  stopped{acct_note}")
        return 0
    for rec in records:
        state = "running" if rec["running"] else "stale"
        print(
            f"  - account={rec.get('accountId')} state={state} mode={rec.get('mode')} "
            f"pid={rec.get('pid')} mcp={rec.get('mcpUrl')} access={rec.get('mcpAccess')}"
        )
    return 0


def cmd_stop(args: argparse.Namespace) -> int:
    app_id, _, storage_id = resolve_app_identity(args.target)
    account = args.account or "default"
    if not APP_ID_RE.match(account):
        raise ValueError("--account must match ^[a-z0-9][a-z0-9_-]{0,63}$")
    path = runtime_descriptor_path(storage_id, account)
    rec = inspect_runtime_record(path)
    if not rec:
        payload = {"appId": app_id, "accountId": account, "stopped": True, "wasRunning": False}
        print(json.dumps(payload, indent=2) if args.json else f"{app_id} ({account}) is not running.")
        return 0

    was_running = bool(rec["pidAlive"])
    stopped = terminate_runtime_instance(rec)
    payload = {"appId": app_id, "accountId": account, "stopped": stopped, "wasRunning": was_running, "pid": rec.get("pid")}
    print(json.dumps(payload, indent=2) if args.json else f"Stopped {app_id} ({account}): {stopped}")
    return 0 if stopped else 1


def _spawn_detached(cmd: list[str]) -> subprocess.Popen:
    kwargs: dict = {
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
    }
    if sys.platform == "win32":
        DETACHED_PROCESS = 0x00000008
        CREATE_NEW_PROCESS_GROUP = 0x00000200
        kwargs["creationflags"] = CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS
    else:
        kwargs["start_new_session"] = True
    return subprocess.Popen(cmd, **kwargs)


def cmd_connect(args: argparse.Namespace) -> int:
    if args.headless and args.gui:
        raise ValueError("Pass either --headless or --gui, not both")

    app_id, _, storage_id = resolve_app_identity(args.target)
    account = args.account or "default"
    if not APP_ID_RE.match(account):
        raise ValueError("--account must match ^[a-z0-9][a-z0-9_-]{0,63}$")

    desired_mode = "headless" if args.headless else ("gui" if args.gui else None)
    path = runtime_descriptor_path(storage_id, account)
    existing = inspect_runtime_record(path)

    if existing and existing["running"]:
        current_mode = existing.get("mode")
        if args.webmcp_only and existing.get("mcpAccess") != "webmcp-only":
            if not args.switch_mode:
                raise RuntimeError(
                    f"{app_id} ({account}) is already running in '{existing.get('mcpAccess')}' access mode; "
                    "pass --switch-mode to restart it in 'webmcp-only' mode."
                )
            if not terminate_runtime_instance(existing):
                raise RuntimeError(f"Failed stopping existing {app_id} ({account}) instance")
        elif desired_mode is None or desired_mode == current_mode:
            payload = {**existing, "reused": True}
            print(json.dumps(payload, indent=2) if args.json else f"Reusing {app_id} ({account}) [{current_mode}] MCP={existing.get('mcpUrl')}")
            return 0
        elif current_mode == "headless" and desired_mode == "gui":
            if not terminate_runtime_instance(existing):
                raise RuntimeError(f"Failed stopping headless {app_id} ({account}) before promoting to GUI")
        elif current_mode == "gui" and desired_mode == "headless":
            if not args.switch_mode:
                raise RuntimeError(
                    f"{app_id} ({account}) is currently open in GUI mode (pid={existing.get('pid')}); "
                    "omit --headless to reuse the live window or pass --switch-mode to replace it."
                )
            if not terminate_runtime_instance(existing):
                raise RuntimeError(f"Failed stopping GUI {app_id} ({account}) instance")
    elif existing:
        if existing.get("pidAlive"):
            terminate_runtime_instance(existing)
        else:
            path.unlink(missing_ok=True)

    launch_headless = True if desired_mode is None else (desired_mode == "headless")
    binary = find_ladybird_binary(args.ladybird)
    cmd = build_launch_command(
        args.target,
        binary,
        account=account,
        profile=getattr(args, "profile", None),
        mcp_port=args.mcp,
        headless=launch_headless,
        webmcp_only=bool(args.webmcp_only),
        temporary_profile=False,
    )

    path.parent.mkdir(parents=True, exist_ok=True)
    path.unlink(missing_ok=True)
    proc = _spawn_detached(cmd)

    deadline = time.time() + float(args.timeout)
    live_record: dict | None = None
    while time.time() < deadline:
        rec = inspect_runtime_record(path)
        if rec and rec["running"]:
            live_record = rec
            break
        if proc.poll() is not None and not is_pid_alive(proc.pid):
            # In GUI mode, Ladybird launcher might fork Hub or exit on profile handoff; check one more time before failing
            rec = inspect_runtime_record(path)
            if rec and rec["running"]:
                live_record = rec
                break
            raise RuntimeError(f"Launched process exited early with code {proc.returncode}")
        time.sleep(0.25)

    if not live_record:
        raise TimeoutError(f"Timed out after {args.timeout}s waiting for {app_id} ({account}) runtime descriptor")

    payload = {**live_record, "reused": False}
    print(json.dumps(payload, indent=2) if args.json else f"Connected {app_id} ({account}) [{payload.get('mode')}] MCP={payload.get('mcpUrl')}")
    return 0


def cmd_accounts(args: argparse.Namespace) -> int:
    app_id, app_name, storage_id = resolve_app_identity(args.target)
    registry_path = ladybird_instance_data_dir() / "AppAccounts" / f"{storage_id}.json"
    print(f"Accounts for '{app_name}' ({app_id})")
    print(f"Registry: {registry_path}")
    if not registry_path.is_file():
        print("  (no accounts registered yet; default profile: app-" + storage_id + ")")
        return 0
    data = json.loads(registry_path.read_text(encoding="utf-8"))
    accounts = data.get("accounts", [])
    if not accounts:
        print("  (no accounts recorded)")
        return 0
    for acc in accounts:
        print(f"  - {acc.get('id')}: label={acc.get('label')!r} profile={acc.get('profileName')}")
    return 0


def _ignore_runtime_entries(_directory: str, names: list[str]) -> set[str]:
    cli_tools = {"js", "js.exe", "wasm", "wasm.exe", "xml", "xml.exe", "dns", "dns.exe"}
    ignored: set[str] = set()
    for name in names:
        low = name.lower()
        if low in ("__pycache__", ".ds_store") or low.endswith((".pdb", ".ilk", ".exp", ".lib", ".obj")):
            ignored.add(name)
        elif low.startswith("vc_redist") and low.endswith(".exe"):
            ignored.add(name)
        elif low in cli_tools:
            ignored.add(name)
        elif (low.startswith(("test-", "test_")) or name.startswith("Test")) and not low.endswith((".dll", ".so", ".dylib")):
            ignored.add(name)
    return ignored


def cmd_package(args: argparse.Namespace) -> int:
    manifest_file, data = validate_manifest_path(Path(args.target))
    app_root = manifest_file.parent.resolve(strict=True)
    app_id = data["id"]

    runtime_src = Path(args.runtime).resolve(strict=True)
    out_dir = Path(args.out).resolve()
    bundle_dir = out_dir / app_id
    runtime_dest = bundle_dir / "runtime"
    app_dest = bundle_dir / "app"

    if bundle_dir.exists():
        shutil.rmtree(bundle_dir)
    bundle_dir.mkdir(parents=True, exist_ok=True)

    if runtime_src.is_file() and runtime_src.suffix.lower() == ".zip":
        with zipfile.ZipFile(runtime_src, "r") as zf:
            zf.extractall(runtime_dest)
    elif runtime_src.is_dir():
        bin_src = (runtime_src / "bin") if (runtime_src / "bin").is_dir() else runtime_src
        share_src = None
        for candidate in (bin_src.parent / "share" / "Lagom", runtime_src / "share" / "Lagom"):
            if candidate.is_dir():
                share_src = candidate
                break
        shutil.copytree(bin_src, runtime_dest / "bin", ignore=_ignore_runtime_entries)
        if share_src is not None:
            shutil.copytree(share_src, runtime_dest / "share" / "Lagom")
    else:
        raise ValueError("--runtime must be an extracted runtime directory or .zip archive")

    shutil.copytree(app_root, app_dest, ignore=shutil.ignore_patterns("__pycache__", ".DS_Store", "tests"))

    exe_rel = "bin\\Ladybird.exe" if (runtime_dest / "bin" / "Ladybird.exe").exists() else "Ladybird.exe"
    if (runtime_dest / "bin" / "Ladybird").exists():
        sh_rel = "bin/Ladybird"
    elif (runtime_dest / "Ladybird").exists():
        sh_rel = "Ladybird"
    else:
        sh_rel = "bin/Ladybird"

    (bundle_dir / f"{app_id}.cmd").write_text(
        f'@echo off\r\n"%~dp0runtime\\{exe_rel}" --app-manifest "%~dp0app\\ladybird-app.json" %*\r\n',
        encoding="utf-8",
    )
    sh_launcher = bundle_dir / f"{app_id}.sh"
    sh_launcher.write_text(
        f'#!/usr/bin/env sh\nDIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\nexec "$DIR/runtime/{sh_rel}" --app-manifest "$DIR/app/ladybird-app.json" "$@"\n',
        encoding="utf-8",
    )
    if os.name != "nt":
        sh_launcher.chmod(0o755)

    bundle_mb = sum(p.stat().st_size for p in bundle_dir.rglob("*") if p.is_file()) / (1024 * 1024)
    print(f"Packaged '{data['name']}' into {bundle_dir} ({bundle_mb:.1f} MB)")

    if getattr(args, "zip", False):
        version = str(data.get("version") or "0.1.0")
        zip_path = out_dir / f"{app_id}-{version}.zip"
        sha_path = out_dir / f"{app_id}-{version}.zip.sha256"
        with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for item in sorted(bundle_dir.rglob("*")):
                if item.is_file():
                    zf.write(item, item.relative_to(out_dir).as_posix())
        h = hashlib.sha256()
        with zip_path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        digest = h.hexdigest()
        sha_path.write_text(f"{digest}  {zip_path.name}\n", encoding="utf-8")
        zip_mb = zip_path.stat().st_size / (1024 * 1024)
        print(f"Archive: {zip_path} ({zip_mb:.1f} MB)")
        print(f"SHA-256: {digest} ({sha_path})")

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Ladybird App Mode CLI")
    sub = parser.add_subparsers(dest="command", required=True)

    p_init = sub.add_parser("init", help="Scaffold a new Ladybird App Mode application")
    p_init.add_argument("directory")
    p_init.add_argument("--name")
    p_init.add_argument("--id")
    p_init.set_defaults(func=cmd_init)

    p_val = sub.add_parser("validate", help="Validate ladybird-app.json and confined assets")
    p_val.add_argument("target")
    p_val.set_defaults(func=cmd_validate)

    p_run = sub.add_parser("run", help="Launch an app in Ladybird App Mode")
    p_run.add_argument("target")
    p_run.add_argument("--ladybird")
    p_run.add_argument("--account", help="Isolated account profile id (e.g. default, personal, work)")
    p_run.add_argument("--profile", help="Reuse an existing browser profile directory name (e.g. personal)")
    p_run.add_argument("--mcp", type=int)
    p_run.add_argument("--webmcp-only", action="store_true", help="Restrict MCP server to WebMCP tools only (blocks DOM/screenshots/logs/eval_js)")
    p_run.add_argument("--headless", action="store_true")
    p_run.add_argument("--temporary-profile", action="store_true")
    p_run.set_defaults(func=cmd_run)

    p_accounts = sub.add_parser("accounts", help="List registered account profiles for an app")
    p_accounts.add_argument("target")
    p_accounts.set_defaults(func=cmd_accounts)

    p_connect = sub.add_parser("connect", help="Reuse an already running app instance or launch it on demand")
    p_connect.add_argument("target")
    p_connect.add_argument("--ladybird")
    p_connect.add_argument("--account", default="default", help="Account profile id (default: default)")
    p_connect.add_argument("--profile", help="Reuse an existing browser profile directory name (e.g. personal)")
    p_connect.add_argument("--mcp", type=int)
    p_connect.add_argument("--headless", action="store_true", help="Require headless mode (or launch headless when stopped)")
    p_connect.add_argument("--gui", action="store_true", help="Require GUI window (promotes headless instance to GUI)")
    p_connect.add_argument("--webmcp-only", action="store_true", help="Enforce WebMCP-only privacy mode when launching")
    p_connect.add_argument("--switch-mode", action="store_true", help="Allow restarting an incompatible running instance")
    p_connect.add_argument("--timeout", type=float, default=25.0)
    p_connect.add_argument("--json", action="store_true")
    p_connect.set_defaults(func=cmd_connect)

    p_status = sub.add_parser("status", help="Show live runtime status and MCP endpoint for an app")
    p_status.add_argument("target")
    p_status.add_argument("--account")
    p_status.add_argument("--json", action="store_true")
    p_status.set_defaults(func=cmd_status)

    p_stop = sub.add_parser("stop", help="Stop a running app instance and clean up its runtime descriptor")
    p_stop.add_argument("target")
    p_stop.add_argument("--account", default="default")
    p_stop.add_argument("--json", action="store_true")
    p_stop.set_defaults(func=cmd_stop)

    p_pkg = sub.add_parser("package", help="Bundle app + runtime + launcher into an output directory")
    p_pkg.add_argument("target")
    p_pkg.add_argument("--runtime", required=True)
    p_pkg.add_argument("--out", required=True)
    p_pkg.add_argument("--zip", action="store_true", help="Also emit <app-id>-<version>.zip and .sha256 checksum")
    p_pkg.set_defaults(func=cmd_package)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
