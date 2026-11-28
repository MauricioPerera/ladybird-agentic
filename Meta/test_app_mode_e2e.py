#!/usr/bin/env python3
"""Consolidated E2E regression suite for Ladybird App Mode:

1. Manifest CLI validation (AgenticNotesApp, BashWorkbenchApp, AudioStudioApp, SalesCRMApp & FlowStudioApp)
2. BashWorkbenchApp auto-MCP startup + just-bash (jq, awk, /workspace sync, kv) + restart persistence
3. Privacy boundary enforcement for mcp.access = 'webmcp-only'
4. Runtime discovery (AppRuntime) and on-demand lifecycle commands (connect / status / stop)
5. AudioStudioApp custom BSD-2-Clause WebMCP-only workstation, DSP, Range header probe & persistence
6. SalesCRMApp custom BSD-2-Clause full-spec CRM, recurring MRR, 30s stage undo, repeating tasks & 3-tier PII isolation
7. FlowStudioApp unit/static suite, dynamic services, inbound webhooks, FlowPacks & restart persistence
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import urlparse

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI = REPO_ROOT / "Meta" / "ladybird-app.py"
NOTES_APP = REPO_ROOT / "Examples" / "AgenticNotesApp"
BASH_APP = REPO_ROOT / "Examples" / "BashWorkbenchApp"
AUDIO_APP = REPO_ROOT / "Examples" / "AudioStudioApp"
CRM_APP = REPO_ROOT / "Examples" / "SalesCRMApp"
FLOW_APP = REPO_ROOT / "Examples" / "FlowStudioApp"
MCP_BANNER_RE = re.compile(r"MCP server listening on http://127\.0\.0\.1:(\d+)/mcp \(token: ([0-9a-fA-F]+)\)")
RAW_IBAN = "ES91-SECRET-IBAN-2100-0418-4502-0005-1332"


def find_ladybird_binary() -> Path:
    for candidate in (
        REPO_ROOT / "Build" / "release" / "bin" / "Ladybird.exe",
        REPO_ROOT / "Build" / "release" / "bin" / "Ladybird",
        REPO_ROOT / "Build" / "debug" / "bin" / "Ladybird",
    ):
        if candidate.exists():
            return candidate
    raise FileNotFoundError("Could not find built Ladybird binary under Build/{release,debug}/bin/")


def instance_data_dir() -> Path:
    if sys.platform == "win32":
        root = Path(os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData" / "Local"))
        return root / "Ladybird"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "Ladybird"
    xdg_data = os.environ.get("XDG_DATA_HOME")
    base = Path(xdg_data) if xdg_data else (Path.home() / ".local" / "share")
    return base / "Ladybird"


def clean_test_account(app_root: Path, account_id: str) -> None:
    manifest = json.loads((app_root / "ladybird-app.json").read_text(encoding="utf-8"))
    app_id = manifest["id"]
    source = b"ladybird-app-storage:v1:local:" + os.fsencode(app_root.resolve(strict=True))
    storage_id = f"{app_id[:31]}-{hashlib.sha256(source).hexdigest()[:32]}"
    bases = [instance_data_dir()]
    if sys.platform == "win32" and os.environ.get("APPDATA"):
        bases.append(Path(os.environ["APPDATA"]) / "Ladybird")
    for base in bases:
        profile_dir = base / "Profiles" / f"app-{storage_id}--{account_id}"
        if profile_dir.exists():
            shutil.rmtree(profile_dir, ignore_errors=True)
    data_dir = instance_data_dir()
    runtime_file = data_dir / "AppRuntime" / f"{storage_id}--{account_id}.json"
    runtime_file.unlink(missing_ok=True)
    registry_file = data_dir / "AppAccounts" / f"{storage_id}.json"
    if registry_file.is_file():
        try:
            doc = json.loads(registry_file.read_text(encoding="utf-8"))
            doc["accounts"] = [a for a in doc.get("accounts", []) if a.get("id") != account_id]
            registry_file.write_text(json.dumps(doc), encoding="utf-8")
        except Exception:
            pass


def mcp_rpc(url: str, token: str, method: str, params: dict | None = None):
    body = {"jsonrpc": "2.0", "id": 1, "method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
    for line in raw.splitlines():
        if line.startswith("data: "):
            return json.loads(line[6:])
    return json.loads(raw)


def mcp_call(url: str, token: str, tool_name: str, arguments: dict | None = None):
    parsed = mcp_rpc(url, token, "tools/call", {"name": tool_name, "arguments": arguments or {}})
    result = parsed.get("result", {})
    content = result.get("content", [])
    text = "\n".join(c.get("text", "") for c in content if c.get("type") == "text")
    data = json.loads(text) if text.strip().startswith(("{", "[")) else text
    return bool(result.get("isError")) or ("error" in parsed), data


def launch_and_discover_mcp(binary: Path, args: list[str]) -> tuple[subprocess.Popen, str, str, int]:
    proc = subprocess.Popen(
        [str(binary), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    captured: list[str] = []
    deadline = time.time() + 25.0
    while time.time() < deadline:
        if proc.poll() is not None:
            rest = proc.stdout.read() if proc.stdout else ""
            captured.append(rest)
            raise RuntimeError(f"Ladybird exited early ({proc.returncode}):\n" + "\n".join(captured))
        line = proc.stdout.readline() if proc.stdout else ""
        if line:
            captured.append(line.rstrip("\r\n"))
            match = MCP_BANNER_RE.search(line)
            if match:
                port = int(match.group(1))
                token = match.group(2)
                return proc, f"http://127.0.0.1:{port}/mcp", token, port
        else:
            time.sleep(0.1)
    stop_proc(proc)
    raise TimeoutError("Timed out waiting for MCP banner:\n" + "\n".join(captured))


def wait_for_webmcp_ready(mcp_url: str, token: str, expected_tool: str, timeout_s: float = 25.0) -> tuple[int, str]:
    mcp_rpc(mcp_url, token, "initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "app-mode-e2e", "version": "1.0"}})
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        err, tabs_res = mcp_call(mcp_url, token, "list_tabs", {})
        if not err:
            for tab in tabs_res.get("tabs", []):
                tab_url = str(tab.get("url", ""))
                if tab_url.startswith("http://127.0.0.1:"):
                    tab_id = tab["id"]
                    err2, tools_res = mcp_call(mcp_url, token, "list_page_tools", {"tab_id": tab_id})
                    if not err2 and any(t.get("name") == expected_tool for t in tools_res.get("tools", [])):
                        return tab_id, tab_url
        time.sleep(0.35)
    raise TimeoutError(f"Timed out waiting for WebMCP tool '{expected_tool}'")


def stop_proc(proc: subprocess.Popen | None) -> None:
    if proc is None:
        return
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        try:
            proc.kill()
            proc.wait(timeout=3)
        except Exception:
            pass


def run_cli(*args: str, check: bool = True) -> dict:
    cmd = [sys.executable, str(CLI), *args]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=35)
    if check and proc.returncode != 0:
        raise AssertionError(f"CLI failed ({proc.returncode}): {' '.join(cmd)}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    return json.loads(proc.stdout) if proc.stdout.strip().startswith(("{", "[")) else {"raw": proc.stdout, "returncode": proc.returncode}


def test_validate_examples() -> None:
    print("[1/7] Validating example app manifests...")
    for app_dir in (NOTES_APP, BASH_APP, AUDIO_APP, CRM_APP, FLOW_APP):
        res = subprocess.run([sys.executable, str(CLI), "validate", str(app_dir)], capture_output=True, text=True, check=True)
        assert res.stdout.startswith("VALID "), res.stdout


def test_bash_workbench_automcp_and_persistence(binary: Path) -> None:
    print("[2/7] Testing BashWorkbench auto-MCP, just-bash, disk sync, KV & restart persistence...")
    acct = "e2e-suite-bash"
    clean_test_account(BASH_APP, acct)
    manifest = str(BASH_APP / "ladybird-app.json")
    proc1 = proc2 = None
    try:
        proc1, mcp_url1, tok1, mcp_port1 = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual"]
        )
        tab1, url1 = wait_for_webmcp_ready(mcp_url1, tok1, "run_bash")
        origin1 = f"{urlparse(url1).scheme}://{urlparse(url1).netloc}"

        err, info = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "get_workbench_info", "arguments": {}})
        assert not err and info["result"]["system"]["mcpPort"] == mcp_port1, info

        err, jq_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "run_bash",
            "arguments": {"command": "cat /workspace/tasks.json | jq -c '[.[] | select(.done == false) | .id]'"},
        })
        assert not err and "[2,3]" in jq_res["result"]["stdout"], jq_res

        err, awk_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "run_bash",
            "arguments": {"command": "awk -F, 'NR>1 {sum+=$3} END {print sum}' /workspace/sales.csv"},
        })
        assert not err and awk_res["result"]["stdout"].strip() == "4280", awk_res

        err, write_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "run_bash",
            "arguments": {"command": 'echo "persisted-line" > /workspace/e2e.txt && kv set suite:k \'{"v":42}\' >/dev/null && kv get suite:k | jq -r .value.v'},
        })
        assert not err and write_res["result"]["stdout"].strip() == "42", write_res

        stop_proc(proc1)
        proc1 = None
        time.sleep(1.2)

        proc2, mcp_url2, tok2, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual"]
        )
        tab2, url2 = wait_for_webmcp_ready(mcp_url2, tok2, "run_bash")
        origin2 = f"{urlparse(url2).scheme}://{urlparse(url2).netloc}"
        assert origin2 == origin1, f"Stable origin changed: {origin1} -> {origin2}"

        err, check_res = mcp_call(mcp_url2, tok2, "call_page_tool", {
            "tab_id": tab2,
            "name": "run_bash",
            "arguments": {"command": "cat /workspace/e2e.txt && kv get suite:k | jq -r .value.v"},
        })
        assert not err and check_res["result"]["stdout"].splitlines() == ["persisted-line", "42"], check_res
    finally:
        stop_proc(proc1)
        stop_proc(proc2)
        clean_test_account(BASH_APP, acct)


def test_webmcp_only_privacy_boundary(binary: Path) -> None:
    print("[3/7] Testing mcp.access = 'webmcp-only' privacy isolation...")
    proc = None
    with tempfile.TemporaryDirectory(prefix="lb_e2e_privacy_") as tmpdir:
        tmp = Path(tmpdir)
        manifest_path = tmp / "ladybird-app.json"
        manifest_path.write_text(
            json.dumps({
                "id": "privacy-e2e",
                "name": "Privacy E2E",
                "version": "0.1.0",
                "entry": "index.html",
                "mcp": {"enabled": True, "allowWebMCP": True, "access": "webmcp-only"},
                "permissions": {"systemInfo": True},
            }),
            encoding="utf-8",
        )
        (tmp / "index.html").write_text(
            f"""<!doctype html><html><head><meta charset="utf-8"/><script src="/__ladybird_bridge.js"></script></head>
<body><div id="secret">{RAW_IBAN}</div><script>
console.log('SECRET:', '{RAW_IBAN}');
function reg(){{ if(!document.modelContext) return;
document.modelContext.registerTool({{
  name:'get_monthly_totals',
  description:'Return aggregate totals only.',
  inputSchema:{{type:'object',properties:{{}},additionalProperties:false}},
  execute: async () => ({{ total: 1115.75, count: 3, mcpAccess: (await window.ladybirdApp.system.info()).mcpAccess }})
}});}}
window.addEventListener('ladybird:webmcp-ready', reg); reg();
</script></body></html>""",
            encoding="utf-8",
        )
        try:
            proc, mcp_url, tok, _ = launch_and_discover_mcp(
                binary, ["--app-manifest", str(manifest_path), "--account", "e2e-suite-privacy", "--headless=manual", "--temporary-profile"]
            )
            tab_id, _ = wait_for_webmcp_ready(mcp_url, tok, "get_monthly_totals")

            tools_list = mcp_rpc(mcp_url, tok, "tools/list", {})
            assert sorted(t["name"] for t in tools_list["result"]["tools"]) == [
                "call_page_tool", "get_active_tab", "list_page_tools", "list_tabs"
            ]
            assert mcp_rpc(mcp_url, tok, "resources/list", {})["result"]["resources"] == []
            assert mcp_rpc(mcp_url, tok, "resources/read", {"uri": "ladybird://notifications"})["error"]["code"] == -32003
            assert mcp_rpc(mcp_url, tok, "skills/list", {})["error"]["code"] == -32003

            for blocked_tool, args in (
                ("get_page_snapshot", {"tab_id": tab_id}),
                ("get_dom_tree", {"tab_id": tab_id}),
                ("get_page_markdown", {"tab_id": tab_id}),
                ("take_screenshot", {"tab_id": tab_id}),
                ("eval_js", {"tab_id": tab_id, "script": "document.body.innerText"}),
                ("get_console_logs", {"tab_id": tab_id}),
                ("open_tab", {"url": "http://127.0.0.1/"}),
            ):
                is_err, payload = mcp_call(mcp_url, tok, blocked_tool, args)
                assert is_err and RAW_IBAN not in str(payload), f"Expected block for {blocked_tool}"

            is_err, agg = mcp_call(mcp_url, tok, "call_page_tool", {"tab_id": tab_id, "name": "get_monthly_totals", "arguments": {}})
            assert not is_err and agg["result"]["total"] == 1115.75 and agg["result"]["mcpAccess"] == "webmcp-only"
        finally:
            stop_proc(proc)


def test_runtime_connect_status_stop() -> None:
    print("[4/7] Testing AppRuntime discovery and connect/status/stop lifecycle...")
    acct = "e2e-suite-life"
    clean_test_account(BASH_APP, acct)
    try:
        c1 = run_cli("connect", str(BASH_APP), "--account", acct, "--headless", "--webmcp-only", "--json")
        assert c1["running"] is True and c1["reused"] is False and c1["mode"] == "headless" and c1["mcpAccess"] == "webmcp-only"

        st = run_cli("status", str(BASH_APP), "--account", acct, "--json")
        assert st["running"] is True and st["pid"] == c1["pid"]

        c2 = run_cli("connect", str(BASH_APP), "--account", acct, "--json")
        assert c2["reused"] is True and c2["pid"] == c1["pid"] and c2["mcpPort"] == c1["mcpPort"]

        tab_id, _ = wait_for_webmcp_ready(c1["mcpUrl"], c1["mcpToken"], "get_workbench_info")
        err, info = mcp_call(c1["mcpUrl"], c1["mcpToken"], "call_page_tool", {"tab_id": tab_id, "name": "get_workbench_info", "arguments": {}})
        assert not err and info["result"]["system"]["mcpPort"] == c1["mcpPort"]

        sp = run_cli("stop", str(BASH_APP), "--account", acct, "--json")
        assert sp["stopped"] is True and sp["wasRunning"] is True
        assert run_cli("status", str(BASH_APP), "--account", acct, "--json")["running"] is False
    finally:
        run_cli("stop", str(BASH_APP), "--account", acct, "--json", check=False)
        clean_test_account(BASH_APP, acct)


def test_audio_studio_webmcp_only_and_persistence(binary: Path) -> None:
    print("[5/7] Testing AudioStudioApp (webmcp-only, Range probe, DSP, undo, markers & persistence)...")
    acct = "e2e-suite-audio"
    clean_test_account(AUDIO_APP, acct)
    manifest = str(AUDIO_APP / "ladybird-app.json")
    proc1 = proc2 = None
    try:
        proc1, mcp_url1, tok1, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual", "--mcp-webmcp-only"]
        )
        tab1, url1 = wait_for_webmcp_ready(mcp_url1, tok1, "synthesize_audio")
        origin1 = f"{urlparse(url1).scheme}://{urlparse(url1).netloc}"

        tools_list = mcp_rpc(mcp_url1, tok1, "tools/list", {})
        assert sorted(t["name"] for t in tools_list["result"]["tools"]) == [
            "call_page_tool", "get_active_tab", "list_page_tools", "list_tabs"
        ], tools_list

        for blocked in ("get_page_snapshot", "eval_js"):
            is_err, _ = mcp_call(mcp_url1, tok1, blocked, {"tab_id": tab1} if blocked == "get_page_snapshot" else {"tab_id": tab1, "script": "1+1"})
            assert is_err, f"Expected {blocked} to be blocked in webmcp-only mode"

        err_pt, page_tools = mcp_call(mcp_url1, tok1, "list_page_tools", {"tab_id": tab1})
        assert not err_pt and len(page_tools.get("tools", [])) == 14, page_tools

        err_syn, syn = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "synthesize_audio",
            "arguments": {"filename": "qa-tone.wav", "waveform": "chord", "freq": 440, "durationSec": 1.0, "gainDb": -6, "channels": 1},
        })
        assert not err_syn and syn["result"]["saved"]["bytesWritten"] == 88244, syn
        assert syn["result"]["document"]["name"] == "qa-tone.wav", syn

        err_pr, probe = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "probe_wav_header",
            "arguments": {"path": "audio/qa-tone.wav"},
        })
        assert not err_pr and probe["result"]["sampleRate"] == 44100 and probe["result"]["channels"] == 1 and probe["result"]["bitDepth"] == 16, probe
        assert abs(probe["result"]["durationSec"] - 1.0) < 0.001, probe

        err_an1, an1 = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "analyze_audio", "arguments": {}})
        assert not err_an1 and -6.5 <= an1["result"]["analysis"]["peakDb"] <= -5.5, an1

        err_norm, _ = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "apply_dsp",
            "arguments": {"effect": "normalize", "params": {"targetDb": -1.0}},
        })
        assert not err_norm
        err_an2, an2 = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "analyze_audio", "arguments": {}})
        assert not err_an2 and -1.3 <= an2["result"]["analysis"]["peakDb"] <= -0.7, an2

        err_lp, _ = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "apply_dsp",
            "arguments": {"effect": "lowpass", "params": {"freq": 600}},
        })
        assert not err_lp

        err_tr, trimmed = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "edit_region",
            "arguments": {"action": "trim", "startSec": 0.1, "endSec": 0.6},
        })
        assert not err_tr and abs(trimmed["result"]["document"]["durationSec"] - 0.5) <= 0.01, trimmed

        err_un, undone = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "undo_redo",
            "arguments": {"action": "undo"},
        })
        assert not err_un and undone["result"]["applied"] is True and abs(undone["result"]["document"]["durationSec"] - 1.0) <= 0.01, undone

        err_mk, mk_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "manage_markers",
            "arguments": {"action": "add", "timeSec": 0.25, "label": "Intro"},
        })
        assert not err_mk and any(m["label"] == "Intro" for m in mk_res["result"]["markers"]), mk_res

        err_exp, exp_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "export_wav",
            "arguments": {"filename": "master.wav", "folder": "exports"},
        })
        assert not err_exp and exp_res["result"]["path"] == "exports/master.wav", exp_res

        err_ls, ls_res = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "list_audio_files", "arguments": {}})
        assert not err_ls and any(it["name"] == "master.wav" for it in ls_res["result"]["exports"]), ls_res

        stop_proc(proc1)
        proc1 = None
        time.sleep(1.2)

        proc2, mcp_url2, tok2, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual", "--mcp-webmcp-only"]
        )
        tab2, url2 = wait_for_webmcp_ready(mcp_url2, tok2, "open_audio_file")
        origin2 = f"{urlparse(url2).scheme}://{urlparse(url2).netloc}"
        assert origin2 == origin1, f"Stable origin changed: {origin1} -> {origin2}"

        err_ls2, ls2 = mcp_call(mcp_url2, tok2, "call_page_tool", {"tab_id": tab2, "name": "list_audio_files", "arguments": {}})
        assert not err_ls2 and any(it["name"] == "master.wav" for it in ls2["result"]["exports"]), ls2

        err_op, op2 = mcp_call(mcp_url2, tok2, "call_page_tool", {
            "tab_id": tab2,
            "name": "open_audio_file",
            "arguments": {"path": "audio/qa-tone.wav"},
        })
        assert not err_op and any(m["label"] == "Intro" for m in op2["result"]["document"]["markers"]), op2
    finally:
        stop_proc(proc1)
        stop_proc(proc2)
        clean_test_account(AUDIO_APP, acct)


def test_sales_crm_full_spec_and_privacy(binary: Path) -> None:
    print("[6/7] Testing SalesCRMApp (full-mode CRM CRUD + recurring MRR + 3-tier webmcp-only PII isolation + persistence)...")
    acct = "e2e-suite-crm"
    clean_test_account(CRM_APP, acct)
    manifest = str(CRM_APP / "ladybird-app.json")
    canary_name = "Elena Vance-Canary"
    canary_email = "ceo-secret@acme-vip.example"
    canary_phone = "+1-555-0199-8877"
    canary_note = "CONFIDENTIAL-TERM-SHEET-9941"
    canaries = (canary_name, canary_email, canary_phone, canary_note)
    proc1 = proc2 = None
    try:
        # Phase 1: Full mode setup with PII canary records, recurring MRR, 30s stage-flip undo, and repeating follow-ups
        proc1, mcp_url1, tok1, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual"]
        )
        tab1, url1 = wait_for_webmcp_ready(mcp_url1, tok1, "upsert_record")
        origin1 = f"{urlparse(url1).scheme}://{urlparse(url1).netloc}"

        err_pt1, pt1 = mcp_call(mcp_url1, tok1, "list_page_tools", {"tab_id": tab1})
        assert not err_pt1 and len(pt1.get("tools", [])) == 17, f"Expected 17 Tier A+B+C tools in full mode: {pt1}"

        err_co, co_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "upsert_record",
            "arguments": {"kind": "company", "data": {"name": "Acme VIP Corp", "industry": "Finance", "notes": canary_note}},
        })
        assert not err_co and co_res["ok"]
        co_id = co_res["result"]["record"]["id"]

        err_ct, ct_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "upsert_record",
            "arguments": {
                "kind": "contact",
                "data": {"name": canary_name, "email": canary_email, "phone": canary_phone, "companyId": co_id, "source": "Referral", "status": "active", "notes": canary_note},
            },
        })
        assert not err_ct and ct_res["ok"]
        ct_id = ct_res["result"]["record"]["id"]

        # Create monthly recurring deal ($2000/mo x 12 = $24,000 contract, $2000 MRR)
        err_dl, dl_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "upsert_record",
            "arguments": {
                "kind": "deal",
                "data": {"name": "Acme VIP Renewal", "value": 2000, "billing": "monthly", "term": 12, "stage": "new", "contactId": ct_id, "companyId": co_id, "notes": canary_note},
            },
        })
        assert not err_dl and dl_res["ok"]
        deal_id = dl_res["result"]["record"]["id"]

        # Test stage transition + 30s stage-flip undo (new -> proposal -> new pops history back to 1)
        err_m1, m1 = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "move_deal_stage",
            "arguments": {"dealId": deal_id, "toStage": "proposal"},
        })
        assert not err_m1 and m1["result"]["stageHistoryLength"] == 2, m1
        err_m2, m2 = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "move_deal_stage",
            "arguments": {"dealId": deal_id, "toStage": "new"},
        })
        assert not err_m2 and m2["result"]["stageHistoryLength"] == 1, m2
        # Move forward cleanly to negotiation (0.75 * 24000 = 18000)
        err_m3, m3 = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "move_deal_stage",
            "arguments": {"dealId": deal_id, "toStage": "negotiation"},
        })
        assert not err_m3 and m3["result"]["weightedPipeline"] == 18000, m3

        # Repeating task spawns child on completion
        err_tk, tk_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "upsert_record",
            "arguments": {"kind": "task", "data": {"title": canary_note, "due": "2026-11-27", "dealId": deal_id, "contactId": ct_id, "repeat": {"every": 7, "unit": "day"}}},
        })
        assert not err_tk and tk_res["ok"]
        task_id = tk_res["result"]["record"]["id"]
        err_comp, comp_res = mcp_call(mcp_url1, tok1, "call_page_tool", {
            "tab_id": tab1,
            "name": "complete_followup",
            "arguments": {"taskId": task_id, "done": True},
        })
        assert not err_comp and comp_res["result"]["spawnedTaskId"], comp_res

        # Search in full mode finds record via committee
        err_srch, srch = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "search_crm", "arguments": {"query": "Vance"}})
        assert not err_srch and len(srch["result"]["contacts"]) == 1 and len(srch["result"]["deals"]) == 1, srch

        # Console clean in full mode
        _, logs1 = mcp_call(mcp_url1, tok1, "get_console_logs", {"tab_id": tab1, "limit": 100})
        errs1 = [e for e in logs1.get("logs", []) if str(e.get("level", "")).lower() in ("error", "err")]
        assert errs1 == [], f"Console errors in full mode: {errs1}"

        stop_proc(proc1)
        proc1 = None
        time.sleep(1.2)

        # Phase 2: Restart same account in strict --mcp-webmcp-only mode & assert ZERO PII canary leakage
        proc2, mcp_url2, tok2, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual", "--mcp-webmcp-only"]
        )
        tab2, url2 = wait_for_webmcp_ready(mcp_url2, tok2, "get_pipeline_summary")
        origin2 = f"{urlparse(url2).scheme}://{urlparse(url2).netloc}"
        assert origin2 == origin1, f"Stable origin changed: {origin1} -> {origin2}"

        tl = mcp_rpc(mcp_url2, tok2, "tools/list", {})
        assert sorted(t["name"] for t in tl["result"]["tools"]) == ["call_page_tool", "get_active_tab", "list_page_tools", "list_tabs"], tl

        err_pt2, pt2 = mcp_call(mcp_url2, tok2, "list_page_tools", {"tab_id": tab2})
        pt2_names = {t["name"] for t in pt2.get("tools", [])}
        assert not err_pt2 and len(pt2_names) == 13, pt2_names
        for tier_c in ("search_crm", "get_record_detail", "upsert_record", "parse_contact_or_company_text"):
            assert tier_c not in pt2_names, f"Tier C tool leaked into webmcp-only: {tier_c}"

        for blocked in ("get_page_snapshot", "eval_js"):
            is_err, payload = mcp_call(mcp_url2, tok2, blocked, {"tab_id": tab2} if blocked == "get_page_snapshot" else {"tab_id": tab2, "script": "1+1"})
            assert is_err and not any(c in json.dumps(payload) for c in canaries)

        # Invoke all Tier A + Tier B tools and assert zero PII leakage & exact persisted aggregates
        tier_ab_calls = [
            ("get_crm_status", {}),
            ("get_pipeline_summary", {}),
            ("get_revenue_forecast", {"months": 6}),
            ("get_conversion_funnel", {"window": "all"}),
            ("get_pipeline_trends", {"months": 6}),
            ("get_attention_summary", {}),
            ("list_playbooks_and_views", {}),
            ("log_structured_touch", {"dealId": deal_id, "type": "call"}),
            ("create_backup", {}),
            ("export_ics_summary", {"filename": "e2e.ics"}),
        ]
        results_by_tool = {}
        for tname, targs in tier_ab_calls:
            is_err, out = mcp_call(mcp_url2, tok2, "call_page_tool", {"tab_id": tab2, "name": tname, "arguments": targs})
            assert not is_err and out.get("ok"), f"{tname} failed: {out}"
            serialized = json.dumps(out)
            for canary in canaries:
                assert canary not in serialized, f"PII canary {canary!r} leaked in {tname}: {serialized}"
            results_by_tool[tname] = out["result"]

        ps = results_by_tool["get_pipeline_summary"]
        assert ps["openPipeline"] == 24000 and ps["weightedPipeline"] == 18000 and ps["pipelineMRR"] == 2000, ps
        assert results_by_tool["get_crm_status"]["counts"]["openTasks"] == 1, results_by_tool["get_crm_status"]
        assert results_by_tool["create_backup"]["path"].startswith("backups/"), results_by_tool["create_backup"]
        assert results_by_tool["export_ics_summary"]["eventCount"] >= 1, results_by_tool["export_ics_summary"]
    finally:
        stop_proc(proc1)
        stop_proc(proc2)
        clean_test_account(CRM_APP, acct)


def eval_js_value(mcp_url: str, token: str, tab_id: int, script: str):
    is_err, res = mcp_call(mcp_url, token, "eval_js", {"tab_id": tab_id, "script": script})
    assert not is_err, f"eval_js failed: {res}"
    return res.get("value") if isinstance(res, dict) else res


def post_json_status(url: str, payload: dict, headers: dict[str, str] | None = None) -> tuple[int, str]:
    req_headers = {"Content-Type": "application/json", **(headers or {})}
    req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"), headers=req_headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace")


def test_flow_studio_full_e2e(binary: Path) -> None:
    print("[7/7] Testing FlowStudioApp (unit/static suite, async eval, SSE push, dynamic services, webhooks, FlowPacks & persistence)...")
    node_bin = shutil.which("node")
    if node_bin:
        node_res = subprocess.run(
            [node_bin, str(FLOW_APP / "tests" / "test_flowstudio.mjs")],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=True,
        )
        assert "failed=0" in node_res.stdout, node_res.stdout

    acct = "e2e-suite-flow"
    clean_test_account(FLOW_APP, acct)
    manifest = str(FLOW_APP / "ladybird-app.json")
    proc1 = proc2 = None
    try:
        proc1, mcp_url1, tok1, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual", "--mcp-allow-eval-js"]
        )
        tab1, url1 = wait_for_webmcp_ready(mcp_url1, tok1, "flowstudio_export_pack")
        origin1 = f"{urlparse(url1).scheme}://{urlparse(url1).netloc}"

        err_pt, pt1 = mcp_call(mcp_url1, tok1, "list_page_tools", {"tab_id": tab1})
        pt1_names = {t["name"] for t in pt1.get("tools", [])}
        assert not err_pt and len(pt1_names) == 9, pt1
        for expected in ("flowstudio_list_packs", "flowstudio_export_pack", "flowstudio_list_services", "flowstudio_list_triggers", "flowstudio_run_workflow"):
            assert expected in pt1_names, f"Missing WebMCP tool {expected}: {pt1_names}"

        # Dynamic service registration via native async eval_js (await directly supported)
        svc_res = eval_js_value(
            mcp_url1,
            tok1,
            tab1,
            'return await window.ladybirdApp.tabs.registerService({ id: "e2e-slack", label: "Slack E2E", defaultUrl: "https://app.slack.com/", origins: ["https://app.slack.com"], container: "e2e-work", permissions: ["open", "navigate", "notifications", "eval", "flows", "network"] });',
        )
        assert svc_res["service"]["id"] == "e2e-slack" and svc_res["service"]["configuredContainer"] == "e2e-work", svc_res

        err_svc, svcs = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "flowstudio_list_services", "arguments": {}})
        assert not err_svc and svcs.get("ok"), svcs
        slack_svc = next((s for s in svcs["result"].get("services", []) if s.get("id") == "e2e-slack"), None)
        assert slack_svc and slack_svc.get("configuredContainer") == "e2e-work" and slack_svc.get("dynamic") is True, svcs

        # Subscribe via EventSource-backed tabs.onEvent to verify real-time SSE delivery
        sub_ok = eval_js_value(
            mcp_url1,
            tok1,
            tab1,
            'window.__sseHits = []; window.__sseUnsub = window.ladybirdApp.tabs.onEvent("mail", (ev) => { window.__sseHits.push(ev); }); return true;',
        )
        assert sub_ok is True
        time.sleep(0.25)

        # Inbound local webhook trigger save, auth rejection (401), dispatch (202), cooldown (429) & SSE delivery (< 500ms)
        trig_res = eval_js_value(
            mcp_url1,
            tok1,
            tab1,
            'return await window.ladybirdApp.triggers.save({ id: "e2e-hook", name: "E2E Hook", type: "webhook", serviceId: "mail", flowId: "gmail-sync-watcher", clientHandled: true, cooldownMs: 5000 });',
        )
        secret = trig_res["trigger"]["webhookSecret"]
        assert isinstance(secret, str) and len(secret) == 32, trig_res

        hook_url = f"{origin1}/__ladybird_webhook/e2e-hook"
        st_bad, _ = post_json_status(hook_url, {"source": "e2e"}, {"X-Webhook-Token": "wrong-token"})
        assert st_bad == 401, f"Expected 401 for bad webhook token, got {st_bad}"

        st_ok, body_ok = post_json_status(hook_url, {"source": "e2e", "ok": True}, {"X-Webhook-Token": secret})
        assert st_ok == 202 and json.loads(body_ok).get("accepted") is True, (st_ok, body_ok)

        sse_hits = eval_js_value(
            mcp_url1,
            tok1,
            tab1,
            'await new Promise((r) => setTimeout(r, 350)); try { window.__sseUnsub && window.__sseUnsub(); } catch (e) {} return window.__sseHits;',
        )
        assert isinstance(sse_hits, list) and any(
            isinstance(ev, dict) and ev.get("type") == "webhook" and (ev.get("data") or {}).get("payload", {}).get("source") == "e2e"
            for ev in sse_hits
        ), f"Expected real-time SSE webhook delivery, got: {sse_hits}"

        st_cool, _ = post_json_status(hook_url, {"source": "e2e"}, {"X-Webhook-Token": secret})
        assert st_cool == 429, f"Expected 429 during webhook cooldown, got {st_cool}"

        evs_res = eval_js_value(mcp_url1, tok1, tab1, 'return await window.ladybirdApp.tabs.events({ serviceId: "mail", limit: 20 });')
        webhook_evs = [e for e in evs_res.get("events", []) if e.get("type") == "webhook" and (e.get("data") or {}).get("payload", {}).get("source") == "e2e"]
        assert len(webhook_evs) >= 1, evs_res

        # Portable FlowPack export & listing
        err_exp, exp = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "flowstudio_export_pack", "arguments": {}})
        assert not err_exp and exp.get("ok"), exp
        exp_res = exp["result"]
        assert exp_res["path"].startswith("packs/") and exp_res["integrity"].startswith("sha256-"), exp_res

        err_lp, lp1 = mcp_call(mcp_url1, tok1, "call_page_tool", {"tab_id": tab1, "name": "flowstudio_list_packs", "arguments": {}})
        assert not err_lp and any(p.get("packId") == exp_res["packId"] or p.get("path") == exp_res["path"] for p in lp1["result"].get("packs", [])), lp1

        err_logs1, logs1 = mcp_call(mcp_url1, tok1, "get_console_logs", {"tab_id": tab1, "limit": 100})
        assert not err_logs1
        errs1 = [e for e in logs1.get("logs", []) if str(e.get("level", "")).lower() in ("error", "err")]
        assert errs1 == [], f"Console errors in FlowStudioApp phase 1: {errs1}"

        stop_proc(proc1)
        proc1 = None
        time.sleep(1.2)

        # Restart same account and confirm persistence of origin, dynamic service, FlowPack & webhook secret
        proc2, mcp_url2, tok2, _ = launch_and_discover_mcp(
            binary, ["--app-manifest", manifest, "--account", acct, "--headless=manual"]
        )
        tab2, url2 = wait_for_webmcp_ready(mcp_url2, tok2, "flowstudio_list_packs")
        origin2 = f"{urlparse(url2).scheme}://{urlparse(url2).netloc}"
        assert origin2 == origin1, f"Stable origin changed: {origin1} -> {origin2}"

        err_svc2, svcs2 = mcp_call(mcp_url2, tok2, "call_page_tool", {"tab_id": tab2, "name": "flowstudio_list_services", "arguments": {}})
        assert not err_svc2 and any(s.get("id") == "e2e-slack" and s.get("configuredContainer") == "e2e-work" for s in svcs2["result"].get("services", [])), svcs2

        err_lp2, lp2 = mcp_call(mcp_url2, tok2, "call_page_tool", {"tab_id": tab2, "name": "flowstudio_list_packs", "arguments": {}})
        assert not err_lp2 and any(p.get("packId") == exp_res["packId"] or p.get("path") == exp_res["path"] for p in lp2["result"].get("packs", [])), lp2

        err_tr2, tr2 = mcp_call(mcp_url2, tok2, "call_page_tool", {"tab_id": tab2, "name": "flowstudio_list_triggers", "arguments": {}})
        assert not err_tr2 and any(t.get("id") == "e2e-hook" and t.get("webhookSecret") == secret for t in tr2["result"].get("triggers", [])), tr2

        err_logs2, logs2 = mcp_call(mcp_url2, tok2, "get_console_logs", {"tab_id": tab2, "limit": 100})
        assert not err_logs2
        errs2 = [e for e in logs2.get("logs", []) if str(e.get("level", "")).lower() in ("error", "err")]
        assert errs2 == [], f"Console errors in FlowStudioApp phase 2: {errs2}"
    finally:
        stop_proc(proc1)
        stop_proc(proc2)
        clean_test_account(FLOW_APP, acct)


def main() -> int:
    binary = find_ladybird_binary()
    test_validate_examples()
    test_bash_workbench_automcp_and_persistence(binary)
    test_webmcp_only_privacy_boundary(binary)
    test_runtime_connect_status_stop()
    test_audio_studio_webmcp_only_and_persistence(binary)
    test_sales_crm_full_spec_and_privacy(binary)
    test_flow_studio_full_e2e(binary)
    print("ALL_APP_MODE_E2E_SUITES_PASSED=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
