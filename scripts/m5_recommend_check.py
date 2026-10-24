#!/usr/bin/env python3
r"""MORA M5 recommendation / random-play acceptance (real network, frozen binary).

What this proves
----------------
M5 adds music_recommend and music_play_random to the MCP stdio adapter. This harness
reuses the M4 driver (scripts/m4_bridge_check.py: hidden test console, live TUI, MCP
stdio client, per-user pipe, same-instant memory sampling) and checks the new tools
against the real service:

  1. fingerprint   SHA256 of build/mora.exe before/after; exit 2 on mismatch/replace
  2. preflight     no other MORA player may own the session pipe
  3. tui           live TUI (no --demo) renders the Amu home frame and owns the pipe
  4. mcp           initialize(2025-11-25) -> initialized -> tools/list (8 tools)
  5. status        music_get_status: playerSessionAttached=true
  6. recommend     music_recommend(intent, source=search, limit=5): real candidates
                   with real ids and reasons; playback does NOT start (now_playing
                   playing=false, TUI snapshot has no PLAYING)
  7. deterministic the same seed twice yields the same candidate ids in the same
                   order; a different seed is recorded (allowed to differ)
  8. play_random   music_play_random(candidateSetId): audio really starts and
                   now_playing.track.id equals the returned candidate id; the TUI
                   snapshot shows that track PLAYING
  9. no-repeat     a second play_random on the same set never picks the current
                   track and really switches audio
 10. by-intent     play_random(intent=...) works from a fresh search and avoids the
                   currently playing track
 11. missing-set   play_random(candidateSetId="rec-does-not-exist") -> not_found and
                   playback state unchanged
 12. no-candidates the no_candidates path plus "never skip" behavior: a literal
                   nonsensical intent is recorded honestly (live search may still
                   return fuzzy hits), and a deterministic one-candidate-set second
                   pick proves no_candidates without skipping the playing track
 13. memory        TUI + MCP sampled at the same instant while audio is playing;
                   sum working set and sum private commit must each stay < 50 MiB
 14. playlists     music_recommend(source=playlists) recorded honestly: a guest gets
                   needs_login; a signed-in account must return real candidates and
                   play_random must work
 15. eof           close stdin: MCP exits 0 and stdout was pure JSON-RPC (no ANSI)
 16. cleanup       /quit, report written, pipe released
 17. fingerprint   binary unchanged during the run (never rebuilt)
 18. classify      every failure classified as environment/product/harness

Exit codes (same as the M4 script)
----------------------------------
  0  every step passed (or only environment failures with --allow-environment-failures)
  1  product defect or harness failure
  2  binary fingerprint mismatch / binary replaced during the run (nothing rebuilt)
  3  environment / permission / network / console failure only

Writes only build/m5-recommend/** (gitignored artifacts) and the --out JSON (default
docs/verification/m5-recommend.json). Never builds, never invokes build.ps1, never
modifies the frozen binary, the user's playlists or favorites. No account writes.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).resolve()
sys.path.insert(0, str(ROOT / "scripts"))

import m4_bridge_check as m4  # noqa: E402  - shared TUI/MCP/memory driver

DEFAULT_EXE = ROOT / "build" / "mora.exe"
DEFAULT_OUT = ROOT / "docs" / "verification" / "m5-recommend.json"
DEFAULT_ARTIFACTS = ROOT / "build" / "m5-recommend"

MB = 1048576.0
BUDGET_MB = 50.0
TUI_SECONDS_DEFAULT = 180

TOOL_NAMES = {
    "music_search",
    "music_open_collection",
    "music_play",
    "music_control",
    "music_now_playing",
    "music_get_status",
    "music_recommend",
    "music_play_random",
}

STEP_ORDER = [
    ("1-binary-fingerprint", "记录被测二进制指纹"),
    ("2-preflight", "预检：没有其他 MORA 播放器占用会话管道"),
    ("3-tui-start", "启动真实 TUI 并等待首页快照（阿木）"),
    ("4-mcp-handshake", "MCP initialize / initialized / tools/list（8 个工具）"),
    ("5-mcp-status", "music_get_status: playerSessionAttached=true"),
    ("6-recommend-no-play", "music_recommend 真实候选且不启动播放"),
    ("7-recommend-deterministic", "同一 seed 两次 recommend 候选 id 顺序一致"),
    ("8-play-random-set", "music_play_random(candidateSetId) 真实播放候选"),
    ("9-play-random-no-repeat", "同一候选集再抽一次：排除当前曲目并真实切歌"),
    ("10-play-random-intent", "music_play_random(intent) 按意图工作且不重复当前曲目"),
    ("11-play-random-missing-set", "candidateSetId 不存在 -> not_found（播放器在线）"),
    ("12-no-candidates", "空候选/唯一候选在播 -> no_candidates 且不跳曲"),
    ("13-memory", "播放中同刻采样 TUI+MCP 内存，合计 < 50 MB x2"),
    ("14-recommend-playlists", "music_recommend(source=playlists) 如实记录"),
    ("15-mcp-eof", "关闭 stdin：MCP 退出 0 且 stdout 只有 JSON-RPC"),
    ("16-tui-cleanup", "TUI /quit 退出、报告写出、管道释放"),
    ("17-fingerprint-stable", "运行期间二进制未被替换"),
]
CLASSIFICATION_STEP = ("18-classification", "失败分类与未验证项")


# --------------------------------------------------------------------- helpers
def track_id_of(state) -> int:
    if not isinstance(state, dict):
        return 0
    track = state.get("track")
    if isinstance(track, dict):
        value = track.get("id")
        if isinstance(value, int):
            return value
    return 0


def core_name(name: str) -> str:
    text = name or ""
    return re.split(r"[\(（]", text)[0].strip() or text[:6]


def poll_playing_track(mcp, expected_id: int, timeout: float, label: str = "", interval: float = 0.2):
    return m4.poll_now_playing(
        mcp,
        lambda state: state.get("playing") is True and track_id_of(state) == expected_id,
        timeout,
        interval,
        label or ("playing track %s" % expected_id),
    )


def wait_snapshot_track(tui, name: str, timeout: float = 10.0):
    core = core_name(name)
    return m4.poll_snapshot(
        tui,
        lambda frame: bool(core) and core in frame and "PLAYING" in frame,
        timeout,
        0.1,
    )


def stable_frame(tui, attempts: int = 20, delay: float = 0.05, min_lines: int = 30) -> str:
    """Read a complete plain snapshot.

    The TUI rewrites its --snapshot file every tick with a truncate+write, so a naive
    read can catch an empty or partially written frame. Retry until the frame looks
    complete instead of turning a harness race into a product verdict.
    """
    frame = ""
    for _ in range(attempts):
        frame = tui.frame()
        if len(frame.splitlines()) >= min_lines:
            return frame
        time.sleep(delay)
    return frame


def snapshot_excerpt(frame: str, needles) -> list:
    wanted = [item for item in needles if item]
    out = []
    for index, line in enumerate((frame or "").splitlines()):
        if any(item in line for item in wanted) or "PLAYING" in line or "PAUSED" in line:
            out.append({"line": index, "text": line.strip()[:200]})
        if len(out) >= 8:
            break
    return out


def tool_map(tools) -> dict:
    return {item.get("name"): item for item in tools if isinstance(item, dict)}


def real_candidate_list(value) -> list:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, dict) and isinstance(item.get("id"), int) and item["id"] > 0]


# ------------------------------------------------------------------- execute
def execute(args, console, harness_extra) -> dict:
    started_at = time.monotonic()
    exe = Path(args.exe).resolve()
    artifacts = Path(args.artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    steps = []
    state = {
        "findings": [],
        "memory": None,
        "tuiReport": None,
        "mcpStdout": None,
        "cleanup": None,
        "environment": {
            "mcpTransport": "stdio JSON-RPC, one UTF-8 JSON object per line",
            "tuiMode": "live (no --demo) in the hidden test console",
            "accountMode": "unknown until music_recommend(source=playlists)",
            "pipeNamePattern": "\\\\.\\pipe\\mora-player-<current user SID>",
        },
        "exitOverride": None,
        "abort": None,
        "binaryBefore": None,
        "snapshotInfo": None,
        "recommendation": {},
        "sharedStates": {},
        "failures": [],
        "unverified": [],
    }
    resources = {"tui": None, "mcp": None}
    state["environment"]["pipesBefore"] = m4.list_mora_pipes()
    binary_before = m4.fingerprint(exe)
    state["binaryBefore"] = binary_before

    try:
        try:
            # ---------------------------------------------------------- step 1
            expected = args.expected_sha256.upper() if args.expected_sha256 else None
            if not binary_before.get("exists"):
                steps.append(
                    m4.make_step(
                        "1-binary-fingerprint",
                        "记录被测二进制指纹",
                        {"path": str(exe)},
                        {"exists": False},
                        [m4.assertion("binary-exists", True, False, str(exe))],
                        failure_status="environment-failure",
                        classification={"kind": "environment", "reason": "build/mora.exe 不存在，不重建"},
                    )
                )
                state["exitOverride"] = ("environment-failed", 3)
                raise m4.Abort("binary missing", "environment")
            matches = (binary_before["sha256"] == expected) if expected else True
            steps.append(
                m4.make_step(
                    "1-binary-fingerprint",
                    "记录被测二进制指纹",
                    {
                        "path": str(exe),
                        "method": "Get-FileHash -Algorithm SHA256 equivalent (hashlib over the file bytes)",
                        "expectedSha256": expected,
                    },
                    {
                        "sizeBytes": binary_before["sizeBytes"],
                        "sha256": binary_before["sha256"],
                        "lastWriteTime": binary_before["lastWriteTime"],
                        "matchesExpected": matches,
                        "rebuiltByHarness": False,
                    },
                    [
                        m4.assertion("binary-exists", True, True, binary_before["relativePath"]),
                        m4.assertion("sha256-matches-expected", expected or "(record only, no --expected-sha256)", matches, binary_before["sha256"]),
                    ],
                    failure_status="harness-failure",
                    classification={"kind": "fingerprint", "reason": "SHA256 与 --expected-sha256 不一致，未执行被测行为"},
                )
            )
            if not matches:
                state["exitOverride"] = ("binary-mismatch", 2)
                raise m4.Abort("binary fingerprint mismatch", "fingerprint")

            # ---------------------------------------------------------- step 2
            pipes_before = state["environment"]["pipesBefore"]
            steps.append(
                m4.make_step(
                    "2-preflight",
                    "预检：没有其他 MORA 播放器占用会话管道",
                    {"listedPipeRoot": "\\\\.\\pipe\\", "observedMoraPipes": pipes_before},
                    {"observedMoraPipes": pipes_before},
                    [m4.assertion("no-foreign-mora-player", "[]", len(pipes_before) == 0, pipes_before)],
                    failure_status="environment-failure",
                    classification={"kind": "environment", "reason": "已有 MORA 播放器占用会话管道 %s" % (pipes_before,)},
                )
            )
            if pipes_before:
                state["exitOverride"] = ("environment-failed", 3)
                raise m4.Abort("another MORA player owns the session pipe", "environment")

            # ---------------------------------------------------------- step 3
            tui = m4.TuiProcess(exe, console, artifacts, args.tui_seconds)
            resources["tui"] = tui
            try:
                tui.start()
            except OSError as exc:
                steps.append(
                    m4.make_step(
                        "3-tui-start",
                        "启动真实 TUI 并等待首页快照（阿木）",
                        {"argv": tui.argv},
                        {"error": str(exc)},
                        [m4.assertion("tui-process-started", True, False, str(exc))],
                        failure_status="environment-failure",
                        classification={"kind": "environment", "reason": "无法用隐藏测试控制台启动 TUI: %s" % exc},
                    )
                )
                state["exitOverride"] = ("environment-failed", 3)
                raise m4.Abort("cannot start TUI", "environment")
            home_frame, home_elapsed = tui.wait(lambda frame: "阿木" in frame, args.tui_timeout)
            alive = tui.proc.poll() is None
            snapshot_exists = tui.snapshot.exists()
            snapshot_mtime = tui.snapshot.stat().st_mtime if snapshot_exists else None
            snapshot_fresh = bool(snapshot_mtime and (time.time() - snapshot_mtime) < 10)
            pipes_during = m4.list_mora_pipes()
            state["environment"]["pipesDuring"] = pipes_during
            real_mode = ("DEMO" not in (home_frame or "")) and ("离线演示" not in (home_frame or ""))
            pipe_observed = any(name.lower().startswith("mora-player-") for name in pipes_during)
            state["snapshotInfo"] = {
                "path": m4.relative(tui.snapshot),
                "exists": snapshot_exists,
                "fresh": snapshot_fresh,
                "firstHomeFrameSeconds": home_elapsed,
                "frameLength": len(home_frame or ""),
            }
            tui_assertions = [
                m4.assertion("home-frame-with-amu", "阿木 出现在 --snapshot 首页", home_frame is not None, home_elapsed),
                m4.assertion("real-mode", "没有 DEMO / 离线演示 标记", real_mode, real_mode),
                m4.assertion("snapshot-fresh", "快照文件在 10s 内被重写", snapshot_fresh, snapshot_mtime),
                m4.assertion("tui-alive", "进程仍在运行", alive, tui.proc.returncode if not alive else "running"),
                m4.assertion("session-pipe-observed", "出现 mora-player-* 命名管道", pipe_observed, pipes_during),
            ]
            if home_frame is not None:
                steps.append(
                    m4.make_step(
                        "3-tui-start",
                        "启动真实 TUI 并等待首页快照（阿木）",
                        {"argv": tui.argv, "console": {"mode": console.mode, "columns": console.columns, "rows": console.rows}},
                        {
                            "firstHomeFrameSeconds": home_elapsed,
                            "pipesObserved": pipes_during,
                            "snapshot": state["snapshotInfo"],
                            "homeExcerpt": [line for line in (home_frame or "").splitlines() if "阿木" in line][:3],
                        },
                        tui_assertions,
                        failure_status="product-defect",
                        classification={"kind": "product", "reason": "TUI 启动后快照/管道/进程状态不符合预期"},
                    )
                )
            else:
                stderr_tail = tui.stderr_tail()
                rc = tui.proc.returncode if not alive else None
                env_hit, env_marker = m4.classify_environment(stderr_tail)
                if not alive and rc == 2:
                    failure_status = "environment-failure"
                    classification = {"kind": "environment", "reason": "TUI 因另一个播放器占用会话管道而退出 (exit 2)"}
                elif not alive and env_hit:
                    failure_status = "environment-failure"
                    classification = {"kind": "environment", "reason": "TUI 启动失败并给出环境原因: %s" % env_marker}
                elif not alive:
                    failure_status = "product-defect"
                    classification = {"kind": "product", "reason": "TUI 在写出首页快照前退出（exit=%s）" % rc}
                else:
                    failure_status = "product-defect"
                    classification = {"kind": "product", "reason": "TUI 存活但 %ss 内没有写出含阿木的首页快照" % args.tui_timeout}
                steps.append(
                    m4.make_step(
                        "3-tui-start",
                        "启动真实 TUI 并等待首页快照（阿木）",
                        {"argv": tui.argv},
                        {"alive": alive, "returncode": rc, "stderrTail": stderr_tail, "snapshot": state["snapshotInfo"]},
                        tui_assertions,
                        failure_status=failure_status,
                        classification=classification,
                    )
                )
                raise m4.Abort("TUI did not render the home frame", classification["kind"])

            # ---------------------------------------------------------- step 4
            mcp = m4.McpClient(exe, artifacts, "main")
            resources["mcp"] = mcp
            mcp.start()
            init_req = {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "mora-m5-recommend-check", "version": "1"},
            }
            init = mcp.request("initialize", init_req, timeout=20.0)
            init_result = init["message"].get("result") or {}
            mcp.notify("notifications/initialized")
            tools_resp = mcp.request("tools/list", {}, timeout=20.0)
            tools = (tools_resp["message"].get("result") or {}).get("tools", [])
            tool_names = sorted(item.get("name") for item in tools if isinstance(item, dict))
            tools_by_name = tool_map(tools)
            recommend_annotations = (tools_by_name.get("music_recommend") or {}).get("annotations") or {}
            play_random_annotations = (tools_by_name.get("music_play_random") or {}).get("annotations") or {}
            handshake_audit = m4.audit_stdout(mcp)
            handshake_assertions = [
                m4.assertion("negotiated-version", "2025-11-25", init_result.get("protocolVersion") == "2025-11-25", init_result.get("protocolVersion")),
                m4.assertion("server-info", "mora-music", (init_result.get("serverInfo") or {}).get("name") == "mora-music", init_result.get("serverInfo")),
                m4.assertion("tools-list-8", sorted(TOOL_NAMES), tool_names, tool_names),
                m4.assertion("recommend-readonly", "readOnlyHint=true", recommend_annotations.get("readOnlyHint") is True, recommend_annotations),
                m4.assertion("play-random-not-readonly", "readOnlyHint=false", play_random_annotations.get("readOnlyHint") is False, play_random_annotations),
                m4.assertion("stdout-pure-json-rpc", "no ANSI / no non-JSON text", not handshake_audit["problems"], handshake_audit["problems"]),
            ]
            steps.append(
                m4.make_step(
                    "4-mcp-handshake",
                    "MCP initialize / initialized / tools/list（8 个工具）",
                    {"initialize": init_req, "tools/list": {}},
                    {
                        "protocolVersion": init_result.get("protocolVersion"),
                        "serverInfo": init_result.get("serverInfo"),
                        "tools": tool_names,
                        "music_recommend.annotations": recommend_annotations,
                        "music_play_random.annotations": play_random_annotations,
                        "stdoutAudit": handshake_audit,
                    },
                    handshake_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "M5 工具没有出现在 tools/list，或注解不符合契约"},
                )
            )
            if not all(item["passed"] for item in handshake_assertions):
                raise m4.Abort("MCP handshake/tool catalog failed", "product")

            # ---------------------------------------------------------- step 5
            status_resp = mcp.call_tool("music_get_status", {}, timeout=20.0)
            status_structured = status_resp.get("structured") or {}
            attached = status_structured.get("playerSessionAttached")
            session_view = status_structured.get("session")
            status_assertions = [
                m4.assertion("isError", False, status_resp.get("isError") is False, status_resp.get("isError")),
                m4.assertion("playerSessionAttached", True, attached is True, attached),
                m4.assertion("session-non-empty", "dict", isinstance(session_view, dict) and len(session_view) > 0, session_view),
                m4.assertion("stage", "m4-session-bridge", status_structured.get("stage") == "m4-session-bridge", status_structured.get("stage")),
                m4.assertion("implemented-lists-m5", "deterministic_recommendation + random_playback", {"deterministic_recommendation", "random_playback"}.issubset(set(status_structured.get("implemented") or [])), status_structured.get("implemented")),
            ]
            steps.append(
                m4.make_step(
                    "5-mcp-status",
                    "music_get_status: playerSessionAttached=true",
                    {"tool": "music_get_status", "arguments": {}},
                    {
                        "stage": status_structured.get("stage"),
                        "playerSessionAttached": attached,
                        "session": session_view,
                        "implemented": status_structured.get("implemented"),
                        "pending": status_structured.get("pending"),
                        "elapsedSeconds": status_resp.get("elapsedSeconds"),
                    },
                    status_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "MCP 没有连上 TUI 的权威播放会话"},
                )
            )
            if not all(item["passed"] for item in status_assertions):
                raise m4.Abort("MCP did not attach to the TUI session", "product")

            # ---------------------------------------------------------- step 6
            recommend_args = {"intent": args.intent, "source": "search", "limit": 5, "seed": args.seed}
            rec1 = mcp.call_tool("music_recommend", recommend_args, timeout=args.recommend_timeout)
            rec1_sc = rec1.get("structured") or {}
            cands1 = real_candidate_list(rec1_sc.get("candidates"))
            ids1 = [item["id"] for item in cands1]
            set1 = rec1_sc.get("candidateSetId")
            now_after_rec = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            now_after_sc = now_after_rec.get("structured") if isinstance(now_after_rec.get("structured"), dict) else {}
            frame_after_rec = stable_frame(tui)
            snapshot_without_playing = ("PLAYING" not in frame_after_rec) and ("PAUSED" not in frame_after_rec)
            home_after_rec = "阿木" in frame_after_rec
            real_candidate_fields = all(
                isinstance(item.get("id"), int)
                and item["id"] > 0
                and bool(item.get("name"))
                and bool(item.get("reason"))
                for item in cands1
            )
            reason_mentions_search = all("搜索" in str(item.get("reason") or "") for item in cands1)
            unique_candidate_ids = len(set(ids1)) == len(ids1)
            state["recommendation"].update(
                {
                    "intent": args.intent,
                    "seed": args.seed,
                    "limit": 5,
                    "candidateSetId": set1,
                    "candidateIds": ids1,
                    "firstCandidate": cands1[0] if cands1 else None,
                }
            )
            recommend_assertions = [
                m4.assertion("isError", False, rec1.get("isError") is False, rec1.get("isError")),
                m4.assertion("candidateSetId", "rec-*", isinstance(set1, str) and set1.startswith("rec-"), set1),
                m4.assertion("source", "search", rec1_sc.get("source") == "search", rec1_sc.get("source")),
                m4.assertion("query", args.intent, rec1_sc.get("query") == args.intent, rec1_sc.get("query")),
                m4.assertion("seed-echoed", args.seed, rec1_sc.get("seed") == args.seed, rec1_sc.get("seed")),
                m4.assertion("playbackStarted", False, rec1_sc.get("playbackStarted") is False, rec1_sc.get("playbackStarted")),
                m4.assertion("count-5", 5, rec1_sc.get("count") == 5 and len(cands1) == 5, rec1_sc.get("count")),
                m4.assertion("candidates-real", "real id/name/reason and unique ids", real_candidate_fields and unique_candidate_ids, {"count": len(cands1), "unique": unique_candidate_ids}),
                m4.assertion("reason-mentions-search", "reason keeps the real search provenance", reason_mentions_search, [item.get("reason") for item in cands1][:2]),
                m4.assertion("now-playing-still-false", "playing=false", now_after_sc.get("playing") is False, now_after_sc.get("playing")),
                m4.assertion("no-track-selected", "track=null", track_id_of(now_after_sc) == 0, now_after_sc.get("track")),
                m4.assertion("snapshot-has-no-PLAYING", "no PLAYING/PAUSED marker", snapshot_without_playing, snapshot_excerpt(frame_after_rec, [])[:3]),
                m4.assertion("tui-still-home", "阿木 home frame", home_after_rec, home_after_rec),
            ]
            if all(item["passed"] for item in recommend_assertions):
                recommend_status, recommend_classification = "product-defect", None
            elif tui.proc.poll() is not None:
                recommend_status = "harness-failure"
                recommend_classification = {
                    "kind": "harness",
                    "reason": "TUI 在推荐验证期间退出（--seconds 太短或外部结束），不能判产品缺陷",
                }
            elif len((frame_after_rec or "").splitlines()) < 30:
                recommend_status = "harness-failure"
                recommend_classification = {
                    "kind": "harness",
                    "reason": "TUI 快照读取为空/不完整（并发写入竞态，已重试），不能据此判产品缺陷",
                }
            elif rec1.get("isError"):
                env_hit, env_marker = m4.classify_environment(m4.McpClient.tool_error_text(mcp, rec1))
                if env_hit:
                    recommend_status = "environment-failure"
                    recommend_classification = {"kind": "environment", "reason": "推荐被环境阻断: %s" % env_marker}
                else:
                    recommend_status = "product-defect"
                    recommend_classification = {"kind": "product", "reason": "music_recommend 返回了不可解释的错误"}
            else:
                recommend_status = "product-defect"
                recommend_classification = {"kind": "product", "reason": "推荐没有返回真实候选，或推荐期间启动了播放"}
            steps.append(
                m4.make_step(
                    "6-recommend-no-play",
                    "music_recommend 真实候选且不启动播放",
                    {"tool": "music_recommend", "arguments": recommend_args},
                    {
                        "isError": rec1.get("isError"),
                        "elapsedSeconds": rec1.get("elapsedSeconds"),
                        "candidateSetId": set1,
                        "source": rec1_sc.get("source"),
                        "query": rec1_sc.get("query"),
                        "seed": rec1_sc.get("seed"),
                        "count": rec1_sc.get("count"),
                        "playbackStarted": rec1_sc.get("playbackStarted"),
                        "candidates": cands1,
                        "nowPlayingAfter": now_after_sc,
                        "tuiSnapshot": {"path": m4.relative(tui.snapshot), "hasPlaying": "PLAYING" in frame_after_rec, "hasAmu": home_after_rec},
                        "errorText": m4.McpClient.tool_error_text(mcp, rec1)[:400] if rec1.get("isError") else "",
                    },
                    recommend_assertions,
                    failure_status=recommend_status,
                    classification=recommend_classification or {"kind": "product", "reason": "推荐契约不符合"},
                    note="推荐只读：music_recommend 不得启动播放；此刻 now_playing.playing=false 且 TUI 快照没有 PLAYING。",
                )
            )
            if not all(item["passed"] for item in recommend_assertions):
                raise m4.Abort(
                    "music_recommend did not return real candidates without playing",
                    "harness" if recommend_status == "harness-failure" else "product",
                )

            # ---------------------------------------------------------- step 7
            rec2 = mcp.call_tool("music_recommend", recommend_args, timeout=args.recommend_timeout)
            rec2_sc = rec2.get("structured") or {}
            cands2 = real_candidate_list(rec2_sc.get("candidates"))
            ids2 = [item["id"] for item in cands2]
            rec3_args = dict(recommend_args)
            rec3_args["seed"] = args.seed + 1
            rec3 = mcp.call_tool("music_recommend", rec3_args, timeout=args.recommend_timeout)
            rec3_sc = rec3.get("structured") or {}
            cands3 = real_candidate_list(rec3_sc.get("candidates"))
            ids3 = [item["id"] for item in cands3]
            now_mid = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            now_mid_sc = now_mid.get("structured") if isinstance(now_mid.get("structured"), dict) else {}
            frame_mid = stable_frame(tui)
            deterministic = ids1 == ids2 and len(ids1) > 0
            different_seed_order_differs = ids1 != ids3
            state["recommendation"].update(
                {
                    "sameSeedIds": ids2,
                    "differentSeed": args.seed + 1,
                    "differentSeedIds": ids3,
                    "sameSeedDeterministic": deterministic,
                    "differentSeedOrderDiffers": different_seed_order_differs,
                }
            )
            determinism_assertions = [
                m4.assertion("isError-same-seed", False, rec2.get("isError") is False, rec2.get("isError")),
                m4.assertion("same-seed-same-order", ids1, deterministic, {"first": ids1, "second": ids2}),
                m4.assertion("same-seed-same-count", len(ids1), len(ids1) == len(ids2) and len(ids2) == 5, {"first": len(ids1), "second": len(ids2)}),
                m4.assertion("no-playback-during-recommend", "playing=false", now_mid_sc.get("playing") is False, now_mid_sc.get("playing")),
                m4.assertion("snapshot-no-PLAYING", "no PLAYING", ("PLAYING" not in frame_mid) and ("PAUSED" not in frame_mid), snapshot_excerpt(frame_mid, [])[:3]),
            ]
            if all(item["passed"] for item in determinism_assertions):
                determinism_status, determinism_classification = "product-defect", None
            elif tui.proc.poll() is not None:
                determinism_status = "harness-failure"
                determinism_classification = {
                    "kind": "harness",
                    "reason": "TUI 在确定性对比期间退出（--seconds 太短或外部结束），候选池退化为本地回退，不能判产品缺陷",
                }
            elif len((frame_mid or "").splitlines()) < 30:
                determinism_status = "harness-failure"
                determinism_classification = {
                    "kind": "harness",
                    "reason": "TUI 快照读取为空/不完整（并发写入竞态，已重试），不能据此判产品缺陷",
                }
            else:
                determinism_status = "product-defect"
                determinism_classification = {"kind": "product", "reason": "同一 seed 的候选顺序不稳定，或推荐过程启动了播放"}
            steps.append(
                m4.make_step(
                    "7-recommend-deterministic",
                    "同一 seed 两次 recommend 候选 id 顺序一致",
                    {"tool": "music_recommend", "sameSeed": recommend_args, "differentSeed": rec3_args},
                    {
                        "firstIds": ids1,
                        "secondIds": ids2,
                        "sameSeedDeterministic": deterministic,
                        "differentSeedIds": ids3,
                        "differentSeedOrderDiffers": different_seed_order_differs,
                        "differentSeedNote": "不同 seed 允许不同；只记录，不作硬断言。",
                        "firstCandidateSetId": state["recommendation"].get("candidateSetId"),
                        "secondCandidateSetId": rec2_sc.get("candidateSetId"),
                        "thirdCandidateSetId": rec3_sc.get("candidateSetId"),
                        "nowPlaying": {"playing": now_mid_sc.get("playing"), "trackId": track_id_of(now_mid_sc)},
                    },
                    determinism_assertions,
                    failure_status=determinism_status,
                    classification=determinism_classification or {"kind": "product", "reason": "确定性契约不符合"},
                    note="确定性来自 recommend.cpp 的 splitmix64(seed ^ id) 排序；相同候选池 + 相同 seed 必须同序。",
                )
            )
            if not all(item["passed"] for item in determinism_assertions):
                raise m4.Abort(
                    "recommend determinism failed",
                    "harness" if determinism_status == "harness-failure" else "product",
                )

            # ---------------------------------------------------------- step 8
            play_seed = args.play_seed
            pr1 = mcp.call_tool(
                "music_play_random",
                {"candidateSetId": set1, "seed": play_seed},
                timeout=args.play_call_timeout,
            )
            pr1_sc = pr1.get("structured") or {}
            cand_play1 = pr1_sc.get("candidate") if isinstance(pr1_sc.get("candidate"), dict) else {}
            cand_play1_id = cand_play1.get("id") if isinstance(cand_play1.get("id"), int) else 0
            poll1 = poll_playing_track(mcp, cand_play1_id, args.play_timeout, "play_random candidate playing")
            np1 = poll1.get("matchedState") or {}
            snap1 = wait_snapshot_track(tui, cand_play1.get("name") or "", 10.0)
            frame1 = tui.frame()
            state["sharedStates"]["after-first-play-random"] = {
                "snapshotPath": m4.relative(tui.snapshot),
                "matched": snap1["matched"],
                "elapsedSeconds": snap1["elapsedSeconds"],
                "excerpt": snapshot_excerpt(frame1, [core_name(cand_play1.get("name") or "")]),
            }
            state["recommendation"]["firstPlayRandom"] = {"candidate": cand_play1, "playRunning": poll1["matched"]}
            play_assertions = [
                m4.assertion("isError", False, pr1.get("isError") is False, pr1.get("isError")),
                m4.assertion("status", "playing or accepted", pr1_sc.get("status") in ("playing", "accepted"), pr1_sc.get("status")),
                m4.assertion("candidate-from-set", "candidate.id in the recommended set", cand_play1_id in ids1, {"candidateId": cand_play1_id, "setIds": ids1}),
                m4.assertion("candidateSetId-echoed", set1, pr1_sc.get("candidateSetId") == set1, pr1_sc.get("candidateSetId")),
                m4.assertion("playing-true", "playing=true", poll1["matched"], {"pollCount": poll1["pollCount"], "matchedAt": poll1["matchedAtSeconds"], "last": poll1["last"], "error": poll1["error"]}),
                m4.assertion("track-id-matches-candidate", cand_play1_id, track_id_of(np1) == cand_play1_id, track_id_of(np1)),
                m4.assertion("queueSize", 1, np1.get("queueSize") == 1, np1.get("queueSize")),
                m4.assertion("tui-snapshot-playing", "候选曲名 + PLAYING", snap1["matched"], state["sharedStates"]["after-first-play-random"]["excerpt"]),
            ]
            steps.append(
                m4.make_step(
                    "8-play-random-set",
                    "music_play_random(candidateSetId) 真实播放候选",
                    {"tool": "music_play_random", "arguments": {"candidateSetId": set1, "seed": play_seed}},
                    {
                        "isError": pr1.get("isError"),
                        "elapsedSeconds": pr1.get("elapsedSeconds"),
                        "status": pr1_sc.get("status"),
                        "playing": pr1_sc.get("playing"),
                        "candidate": cand_play1,
                        "candidateSetId": pr1_sc.get("candidateSetId"),
                        "playbackPoll": {
                            "matched": poll1["matched"],
                            "matchedAtSeconds": poll1["matchedAtSeconds"],
                            "pollCount": poll1["pollCount"],
                            "error": poll1["error"],
                            "last": poll1["last"],
                        },
                        "nowPlaying": np1,
                        "tuiSnapshot": state["sharedStates"]["after-first-play-random"],
                        "errorText": m4.McpClient.tool_error_text(mcp, pr1)[:400] if pr1.get("isError") else "",
                    },
                    play_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "play_random 没有真正播放返回的候选"},
                    note="play_random 内部最多等约 4s；这里再用 now_playing 复核 playing 与 track.id。",
                )
            )
            if not all(item["passed"] for item in play_assertions):
                raise m4.Abort("play_random(candidateSetId) did not start audio", "product")


            # ---------------------------------------------------------- step 9
            current_after_first = track_id_of(np1)
            pr2 = mcp.call_tool(
                "music_play_random",
                {"candidateSetId": set1, "seed": play_seed + 1},
                timeout=args.play_call_timeout,
            )
            pr2_sc = pr2.get("structured") or {}
            cand_play2 = pr2_sc.get("candidate") if isinstance(pr2_sc.get("candidate"), dict) else {}
            cand_play2_id = cand_play2.get("id") if isinstance(cand_play2.get("id"), int) else 0
            poll2 = poll_playing_track(mcp, cand_play2_id, args.play_timeout, "second play_random candidate playing")
            np2 = poll2.get("matchedState") or {}
            snap2 = wait_snapshot_track(tui, cand_play2.get("name") or "", 10.0)
            frame2 = tui.frame()
            state["sharedStates"]["after-second-play-random"] = {
                "snapshotPath": m4.relative(tui.snapshot),
                "matched": snap2["matched"],
                "elapsedSeconds": snap2["elapsedSeconds"],
                "excerpt": snapshot_excerpt(frame2, [core_name(cand_play2.get("name") or "")]),
            }
            state["recommendation"]["secondPlayRandom"] = {"candidate": cand_play2, "previousTrackId": current_after_first}
            no_repeat_assertions = [
                m4.assertion("isError", False, pr2.get("isError") is False, pr2.get("isError")),
                m4.assertion("candidate-not-current", "candidate.id != previous playing id", cand_play2_id != current_after_first, {"candidateId": cand_play2_id, "previousId": current_after_first}),
                m4.assertion("candidate-from-set", "candidate.id in the recommended set", cand_play2_id in ids1, {"candidateId": cand_play2_id, "setIds": ids1}),
                m4.assertion("playing-true", "playing=true", poll2["matched"], {"pollCount": poll2["pollCount"], "matchedAt": poll2["matchedAtSeconds"], "last": poll2["last"], "error": poll2["error"]}),
                m4.assertion("track-id-matches-candidate", cand_play2_id, track_id_of(np2) == cand_play2_id, track_id_of(np2)),
                m4.assertion("track-really-changed", "track.id != previous", track_id_of(np2) not in (0, current_after_first), {"now": track_id_of(np2), "previous": current_after_first}),
                m4.assertion("tui-snapshot-playing", "新候选曲名 + PLAYING", snap2["matched"], state["sharedStates"]["after-second-play-random"]["excerpt"]),
            ]
            steps.append(
                m4.make_step(
                    "9-play-random-no-repeat",
                    "同一候选集再抽一次：排除当前曲目并真实切歌",
                    {"tool": "music_play_random", "arguments": {"candidateSetId": set1, "seed": play_seed + 1}, "previousTrackId": current_after_first},
                    {
                        "isError": pr2.get("isError"),
                        "elapsedSeconds": pr2.get("elapsedSeconds"),
                        "status": pr2_sc.get("status"),
                        "candidate": cand_play2,
                        "previousTrackId": current_after_first,
                        "playbackPoll": {
                            "matched": poll2["matched"],
                            "matchedAtSeconds": poll2["matchedAtSeconds"],
                            "pollCount": poll2["pollCount"],
                            "error": poll2["error"],
                            "last": poll2["last"],
                        },
                        "nowPlaying": np2,
                        "tuiSnapshot": state["sharedStates"]["after-second-play-random"],
                    },
                    no_repeat_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "第二次 play_random 没有排除当前曲目，或没有真实切歌"},
                    note="selectCandidates(..., avoid=当前曲目) 保证不重复；这里再用 now_playing + 快照复核。",
                )
            )
            if not all(item["passed"] for item in no_repeat_assertions):
                raise m4.Abort("second play_random repeated or did not switch", "product")

            # ---------------------------------------------------------- step 10
            current_after_second = track_id_of(np2)
            pr3 = mcp.call_tool(
                "music_play_random",
                {"intent": args.intent, "seed": play_seed + 2},
                timeout=args.play_call_timeout,
            )
            pr3_sc = pr3.get("structured") or {}
            cand_play3 = pr3_sc.get("candidate") if isinstance(pr3_sc.get("candidate"), dict) else {}
            cand_play3_id = cand_play3.get("id") if isinstance(cand_play3.get("id"), int) else 0
            poll3 = poll_playing_track(mcp, cand_play3_id, args.play_timeout, "intent play_random candidate playing")
            np3 = poll3.get("matchedState") or {}
            snap3 = wait_snapshot_track(tui, cand_play3.get("name") or "", 10.0)
            frame3 = tui.frame()
            state["sharedStates"]["after-intent-play-random"] = {
                "snapshotPath": m4.relative(tui.snapshot),
                "matched": snap3["matched"],
                "elapsedSeconds": snap3["elapsedSeconds"],
                "excerpt": snapshot_excerpt(frame3, [core_name(cand_play3.get("name") or "")]),
            }
            state["recommendation"]["intentPlayRandom"] = {"candidate": cand_play3, "previousTrackId": current_after_second}
            intent_assertions = [
                m4.assertion("isError", False, pr3.get("isError") is False, pr3.get("isError")),
                m4.assertion("candidate-not-current", "candidate.id != previous playing id", cand_play3_id != current_after_second, {"candidateId": cand_play3_id, "previousId": current_after_second}),
                m4.assertion("candidate-real", "id/name/reason", cand_play3_id > 0 and bool(cand_play3.get("name")) and bool(cand_play3.get("reason")), cand_play3),
                m4.assertion("playing-true", "playing=true", poll3["matched"], {"pollCount": poll3["pollCount"], "matchedAt": poll3["matchedAtSeconds"], "last": poll3["last"], "error": poll3["error"]}),
                m4.assertion("track-id-matches-candidate", cand_play3_id, track_id_of(np3) == cand_play3_id, track_id_of(np3)),
                m4.assertion("tui-snapshot-playing", "候选曲名 + PLAYING", snap3["matched"], state["sharedStates"]["after-intent-play-random"]["excerpt"]),
            ]
            steps.append(
                m4.make_step(
                    "10-play-random-intent",
                    "music_play_random(intent) 按意图工作且不重复当前曲目",
                    {"tool": "music_play_random", "arguments": {"intent": args.intent, "seed": play_seed + 2}, "previousTrackId": current_after_second},
                    {
                        "isError": pr3.get("isError"),
                        "elapsedSeconds": pr3.get("elapsedSeconds"),
                        "status": pr3_sc.get("status"),
                        "candidate": cand_play3,
                        "candidateSetIdEcho": pr3_sc.get("candidateSetId"),
                        "previousTrackId": current_after_second,
                        "playbackPoll": {
                            "matched": poll3["matched"],
                            "matchedAtSeconds": poll3["matchedAtSeconds"],
                            "pollCount": poll3["pollCount"],
                            "error": poll3["error"],
                            "last": poll3["last"],
                        },
                        "nowPlaying": np3,
                        "tuiSnapshot": state["sharedStates"]["after-intent-play-random"],
                    },
                    intent_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "按 intent 的随机播放没有工作或重复了当前曲目"},
                    note="intent 路径在工具内部做一次新的真实搜索，再排除当前曲目选择。",
                )
            )
            if not all(item["passed"] for item in intent_assertions):
                raise m4.Abort("play_random(intent) did not work", "product")

            # ---------------------------------------------------------- step 11
            before_missing = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            before_missing_sc = before_missing.get("structured") if isinstance(before_missing.get("structured"), dict) else {}
            missing_id = track_id_of(before_missing_sc)
            pr4 = mcp.call_tool(
                "music_play_random",
                {"candidateSetId": "rec-does-not-exist"},
                timeout=args.play_call_timeout,
            )
            pr4_sc = pr4.get("structured") or {}
            pr4_code = pr4_sc.get("error") if isinstance(pr4_sc, dict) else None
            after_missing = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            after_missing_sc = after_missing.get("structured") if isinstance(after_missing.get("structured"), dict) else {}
            missing_assertions = [
                m4.assertion("isError", True, pr4.get("isError") is True, pr4.get("isError")),
                m4.assertion("error-code", "not_found", pr4_code == "not_found", pr4_code),
                m4.assertion("no-timeout", "answered quickly", float(pr4.get("elapsedSeconds") or 99) < 10.0, pr4.get("elapsedSeconds")),
                m4.assertion("playback-unchanged", "same track id", track_id_of(after_missing_sc) == missing_id, {"before": missing_id, "after": track_id_of(after_missing_sc)}),
                m4.assertion("still-playing", "playing=true", after_missing_sc.get("playing") is True, after_missing_sc.get("playing")),
            ]
            steps.append(
                m4.make_step(
                    "11-play-random-missing-set",
                    "candidateSetId 不存在 -> not_found（播放器在线）",
                    {"tool": "music_play_random", "arguments": {"candidateSetId": "rec-does-not-exist"}},
                    {
                        "isError": pr4.get("isError"),
                        "errorCode": pr4_code,
                        "elapsedSeconds": pr4.get("elapsedSeconds"),
                        "content": pr4.get("contentText")[:200],
                        "before": {"trackId": missing_id, "playing": before_missing_sc.get("playing"), "positionSeconds": before_missing_sc.get("positionSeconds")},
                        "after": {"trackId": track_id_of(after_missing_sc), "playing": after_missing_sc.get("playing"), "positionSeconds": after_missing_sc.get("positionSeconds")},
                    },
                    missing_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "未知 candidateSetId 没有快速返回 not_found，或改变了播放状态"},
                )
            )
            if not all(item["passed"] for item in missing_assertions):
                raise m4.Abort("unknown candidateSetId did not fail with not_found", "product")


            # ---------------------------------------------------------- step 12
            # 12a: the literal nonsensical intent from the task, observed honestly.
            empty_args = {"intent": args.empty_intent, "source": "search", "limit": 5, "seed": 1}
            empty_rec = mcp.call_tool("music_recommend", empty_args, timeout=args.recommend_timeout)
            empty_rec_sc = empty_rec.get("structured") or {}
            empty_rec_cands = real_candidate_list(empty_rec_sc.get("candidates"))
            empty_rec_ids = [item["id"] for item in empty_rec_cands]
            empty_rec_error = empty_rec.get("isError")
            empty_rec_code = empty_rec_sc.get("error") if isinstance(empty_rec_sc, dict) else None
            empty_rec_is_nocand = bool(empty_rec_error and empty_rec_code == "no_candidates")
            empty_rec_real_candidates = bool(
                not empty_rec_error
                and len(empty_rec_cands) >= 1
                and empty_rec_sc.get("playbackStarted") is False
            )
            empty_rec_valid = empty_rec_is_nocand or empty_rec_real_candidates
            if not empty_rec_is_nocand:
                state["findings"].append(
                    {
                        "id": "empty-search-expectation",
                        "severity": "medium",
                        "kind": "spec-data-mismatch",
                        "detail": (
                            "任务预期 intent=%r 会得到空候选并返回 no_candidates，但真实网易云搜索是模糊检索："
                            "该字符串仍返回 %d 条真实候选（id %s）。recommend 因此不会走到 empty-pool 的 no_candidates 分支；"
                            "no_candidates 只在“唯一候选正在播放”等路径出现（本次用单候选集验证）。"
                            % (args.empty_intent, len(empty_rec_cands), empty_rec_ids[:5])
                        ),
                        "observed": {
                            "intent": args.empty_intent,
                            "isError": empty_rec_error,
                            "error": empty_rec_code,
                            "candidateCount": len(empty_rec_cands),
                            "candidateIds": empty_rec_ids[:5],
                            "playbackStarted": empty_rec_sc.get("playbackStarted"),
                        },
                    }
                )

            # 12b: play_random on the same literal intent; it must answer (no hang) and
            # must not skip the track that is already playing when its pick fails.
            before_literal = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            before_literal_sc = before_literal.get("structured") if isinstance(before_literal.get("structured"), dict) else {}
            literal_id = track_id_of(before_literal_sc)
            literal_position = before_literal_sc.get("positionSeconds") or 0
            pr_empty = mcp.call_tool(
                "music_play_random",
                {"intent": args.empty_intent},
                timeout=args.play_call_timeout,
            )
            pr_empty_sc = pr_empty.get("structured") or {}
            pr_empty_code = pr_empty_sc.get("error") if isinstance(pr_empty_sc, dict) else None
            time.sleep(1.3)
            after_literal = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            after_literal_sc = after_literal.get("structured") if isinstance(after_literal.get("structured"), dict) else {}
            literal_played = pr_empty.get("isError") is False
            literal_no_skip = bool(
                track_id_of(after_literal_sc) == literal_id
                and after_literal_sc.get("playing") is True
                and (after_literal_sc.get("positionSeconds") or 0) > literal_position + 0.4
            )
            literal_ok = literal_played or (
                pr_empty.get("isError") is True
                and pr_empty_code in ("no_candidates", "playback_unavailable", "not_found")
            )

            # 12c: deterministic no_candidates. A one-candidate set is played once, then a
            # second pick must return no_candidates ("the only candidate is already playing")
            # and the playing track must not be skipped. At most 3 seeds are tried because a
            # fuzzy pool can contain an unplayable track.
            single_attempts = []
            single_ok = False
            single_set = None
            single_cand = None
            single_again_sc = {}
            single_again_code = None
            single_again_is_nocand = False
            single_no_skip = False
            single_snapshot_matched = False
            single_before_sc = {}
            single_after_sc = {}
            for attempt in range(3):
                single_seed = args.seed + 100 + attempt
                rec_single = mcp.call_tool(
                    "music_recommend",
                    {"intent": args.intent, "source": "search", "limit": 1, "seed": single_seed},
                    timeout=args.recommend_timeout,
                )
                rec_single_sc = rec_single.get("structured") or {}
                single_list = real_candidate_list(rec_single_sc.get("candidates"))
                attempt_info = {
                    "attempt": attempt + 1,
                    "seed": single_seed,
                    "candidateSetId": rec_single_sc.get("candidateSetId"),
                    "recommendIsError": rec_single.get("isError"),
                }
                single_attempts.append(attempt_info)
                if rec_single.get("isError") or not single_list:
                    attempt_info["recommendError"] = rec_single_sc.get("error") or "no candidate"
                    continue
                candidate = single_list[0]
                attempt_info["candidateId"] = candidate["id"]
                attempt_info["candidateName"] = candidate.get("name")
                first = mcp.call_tool(
                    "music_play_random",
                    {"candidateSetId": rec_single_sc["candidateSetId"], "seed": args.play_seed + 10 + attempt},
                    timeout=args.play_call_timeout,
                )
                first_sc = first.get("structured") or {}
                first_code = first_sc.get("error") if isinstance(first_sc, dict) else None
                attempt_info["firstIsError"] = first.get("isError")
                attempt_info["firstError"] = first_code
                if first.get("isError"):
                    if first_code != "no_candidates":
                        continue
                    # The single candidate is already playing: no switch needed.
                else:
                    confirm = poll_playing_track(mcp, candidate["id"], 15.0, "single-candidate started")
                    attempt_info["playingConfirmed"] = confirm["matched"]
                    if not confirm["matched"]:
                        continue
                single_ok = True
                single_set = rec_single_sc["candidateSetId"]
                single_cand = candidate
                break
            if single_ok:
                before_again = mcp.call_tool("music_now_playing", {}, timeout=20.0)
                single_before_sc = before_again.get("structured") if isinstance(before_again.get("structured"), dict) else {}
                again = mcp.call_tool(
                    "music_play_random",
                    {"candidateSetId": single_set, "seed": args.play_seed + 30},
                    timeout=args.play_call_timeout,
                )
                single_again_sc = again.get("structured") or {}
                single_again_code = single_again_sc.get("error") if isinstance(single_again_sc, dict) else None
                single_again_is_nocand = bool(again.get("isError") and single_again_code == "no_candidates")
                time.sleep(1.5)
                after_again = mcp.call_tool("music_now_playing", {}, timeout=20.0)
                single_after_sc = after_again.get("structured") if isinstance(after_again.get("structured"), dict) else {}
                single_no_skip = bool(
                    track_id_of(single_after_sc) == track_id_of(single_before_sc)
                    and track_id_of(single_after_sc) == single_cand["id"]
                    and single_after_sc.get("playing") is True
                    and (single_after_sc.get("positionSeconds") or 0)
                    > (single_before_sc.get("positionSeconds") or 0) + 0.4
                    and single_after_sc.get("queueSize") == single_before_sc.get("queueSize")
                    and single_after_sc.get("queueIndex") == single_before_sc.get("queueIndex")
                )
                single_snapshot = wait_snapshot_track(tui, single_cand.get("name") or "", 5.0)
                single_snapshot_matched = single_snapshot["matched"]
                state["sharedStates"]["no-candidates-still-playing"] = {
                    "snapshotPath": m4.relative(tui.snapshot),
                    "matched": single_snapshot_matched,
                    "excerpt": snapshot_excerpt(stable_frame(tui), [core_name(single_cand.get("name") or "")]),
                }
            state["recommendation"].update(
                {
                    "emptyIntent": args.empty_intent,
                    "emptyIntentObserved": {
                        "isError": empty_rec_error,
                        "error": empty_rec_code,
                        "candidateCount": len(empty_rec_cands),
                        "candidateIds": empty_rec_ids[:5],
                        "expectationNoCandidatesMet": empty_rec_is_nocand,
                    },
                    "literalPlayRandom": {
                        "isError": pr_empty.get("isError"),
                        "error": pr_empty_code,
                        "elapsedSeconds": pr_empty.get("elapsedSeconds"),
                        "candidate": pr_empty_sc.get("candidate"),
                        "noSkip": literal_played or literal_no_skip,
                    },
                    "singleCandidateAttempts": single_attempts,
                    "singleCandidateSet": single_set,
                    "singleCandidate": single_cand,
                    "noCandidatesError": single_again_code,
                }
            )
            no_candidates_assertions = [
                m4.assertion("literal-empty-intent-documented", "no_candidates OR real fuzzy candidates", empty_rec_valid, state["recommendation"]["emptyIntentObserved"]),
                m4.assertion("literal-empty-play-random", "machine-readable error or real playback, never a hang", literal_ok, {"isError": pr_empty.get("isError"), "error": pr_empty_code, "elapsedSeconds": pr_empty.get("elapsedSeconds")}),
                m4.assertion("literal-empty-no-skip", "previous track still playing and progressing when the pick fails", literal_played or literal_no_skip, {"before": {"id": literal_id, "positionSeconds": literal_position}, "after": {"id": track_id_of(after_literal_sc), "positionSeconds": after_literal_sc.get("positionSeconds"), "playing": after_literal_sc.get("playing")}}),
                m4.assertion("single-set-no-candidates", "isError no_candidates on the second pick", single_again_is_nocand, {"singleSet": single_set, "error": single_again_code}),
                m4.assertion("single-set-message", "non-empty explanation", bool(single_again_sc.get("message")), single_again_sc.get("message")),
                m4.assertion("no-skip-after-no-candidates", "same track, playing, position advanced", single_no_skip, {"before": single_before_sc, "after": {"trackId": track_id_of(single_after_sc), "playing": single_after_sc.get("playing"), "positionSeconds": single_after_sc.get("positionSeconds"), "queueSize": single_after_sc.get("queueSize")}}),
                m4.assertion("tui-snapshot-still-playing", "单候选曲名 + PLAYING", single_snapshot_matched, state["sharedStates"].get("no-candidates-still-playing", {}).get("excerpt")),
            ]
            if not single_ok:
                no_candidates_status = "environment-failure"
                no_candidates_classification = {
                    "kind": "environment",
                    "reason": "三次尝试都无法构造可播放的单候选集（候选可能无完整播放权限），no_candidates 分支未能在真实音频上触发",
                }
            elif all(item["passed"] for item in no_candidates_assertions):
                no_candidates_status, no_candidates_classification = "product-defect", None
            else:
                no_candidates_status = "product-defect"
                no_candidates_classification = {"kind": "product", "reason": "no_candidates / 不跳曲行为不符合契约"}
            steps.append(
                m4.make_step(
                    "12-no-candidates",
                    "空候选/唯一候选在播 -> no_candidates 且不跳曲",
                    {
                        "literalIntent": args.empty_intent,
                        "singleCandidateSeeds": [args.seed + 100, args.seed + 101, args.seed + 102],
                        "note": "任务预期的空 intent 在真实模糊检索下可能仍有候选；因此同时验证“唯一候选在播”的 no_candidates 路径。",
                    },
                    {
                        "literalRecommend": state["recommendation"]["emptyIntentObserved"],
                        "literalPlayRandom": state["recommendation"]["literalPlayRandom"],
                        "singleCandidateAttempts": single_attempts,
                        "singleCandidateSet": single_set,
                        "singleCandidate": single_cand,
                        "secondPick": {
                            "isError": single_again_is_nocand or None,
                            "errorCode": single_again_code,
                            "message": single_again_sc.get("message"),
                        },
                        "beforeSecondPick": single_before_sc,
                        "afterSecondPick": single_after_sc,
                        "positionAdvancedSeconds": round(
                            (single_after_sc.get("positionSeconds") or 0) - (single_before_sc.get("positionSeconds") or 0), 3
                        ),
                        "tuiSnapshot": state["sharedStates"].get("no-candidates-still-playing"),
                        "expectationNoCandidatesMet": empty_rec_is_nocand,
                    },
                    no_candidates_assertions,
                    failure_status=no_candidates_status,
                    classification=no_candidates_classification,
                    note="no_candidates 是“停在可解释错误”，绝不能自动换曲重试；这里同时验证当前曲目进度继续前进。",
                )
            )
            if not all(item["passed"] for item in no_candidates_assertions):
                raise m4.Abort("no_candidates behavior failed", "environment" if not single_ok else "product")


            # ---------------------------------------------------------- step 13
            tui_pid = tui.proc.pid
            mcp_pid = mcp.proc.pid
            samples = []
            sampling_error = None
            sampling_started = time.monotonic()
            while True:
                tick = time.monotonic()
                record = m4.sample_pair(tui_pid, mcp_pid)
                record["atSeconds"] = round(tick - sampling_started, 3)
                samples.append(record)
                for role in ("tui", "mcp"):
                    if "error" in record[role]:
                        sampling_error = "%s memory sample failed: %s (%s)" % (
                            role,
                            record[role].get("error"),
                            record[role].get("errorSource"),
                        )
                if sampling_error:
                    break
                if time.monotonic() - sampling_started >= args.memory_seconds:
                    break
                time.sleep(max(0.0, args.memory_interval - (time.monotonic() - tick)))
            valid = [item for item in samples if "sumWorkingSetMB" in item]
            tui_peak_ws = max((item["tui"]["workingSetMB"] for item in valid), default=None)
            tui_peak_private = max((item["tui"]["privateMB"] for item in valid), default=None)
            mcp_peak_ws = max((item["mcp"]["workingSetMB"] for item in valid), default=None)
            mcp_peak_private = max((item["mcp"]["privateMB"] for item in valid), default=None)
            sum_peak_ws = max((item["sumWorkingSetMB"] for item in valid), default=None)
            sum_peak_private = max((item["sumPrivateMB"] for item in valid), default=None)
            headroom = None
            if sum_peak_ws is not None and sum_peak_private is not None:
                headroom = round(BUDGET_MB - max(sum_peak_ws, sum_peak_private), 3)
            memory = {
                "unit": "MiB (bytes / 1048576, same as GetProcessMemoryInfo and MORA --report)",
                "scope": "TUI (live build/mora.exe) + MCP adapter (build/mora.exe --mcp) sampled at the same instant while audio is playing; the external host/terminal is not counted",
                "method": "GetProcessMemoryInfo(PROCESS_MEMORY_COUNTERS_EX): WorkingSetSize and PrivateUsage per process, summed per sample",
                "budgetMB": BUDGET_MB,
                "durationSeconds": round(time.monotonic() - sampling_started, 3),
                "intervalSeconds": args.memory_interval,
                "sampleCount": len(samples),
                "validSampleCount": len(valid),
                "processes": {
                    "tui": {"pid": tui_pid, "peakWorkingSetMB": tui_peak_ws, "peakPrivateMB": tui_peak_private},
                    "mcp": {"pid": mcp_pid, "peakWorkingSetMB": mcp_peak_ws, "peakPrivateMB": mcp_peak_private},
                },
                "sum": {
                    "peakWorkingSetMB": sum_peak_ws,
                    "peakPrivateMB": sum_peak_private,
                    "headroomMB": headroom,
                    "workingSetPassed": bool(sum_peak_ws is not None and sum_peak_ws < BUDGET_MB),
                    "privatePassed": bool(sum_peak_private is not None and sum_peak_private < BUDGET_MB),
                },
                "samples": samples,
                "note": "合计=两个 MORA 进程同一时刻求和；外部宿主/终端不计入。M5 的候选集只存在 MCP 进程内，内存增量很小。",
            }
            state["memory"] = memory
            if headroom is not None and 0 < headroom < 5.0:
                state["findings"].append(
                    {
                        "id": "memory-headroom-narrow",
                        "severity": "medium",
                        "kind": "product-risk",
                        "detail": "TUI+MCP 同刻合计工作集距 50 MB 只剩 %.2f MB（本次 %.2f MB）。" % (headroom, max(sum_peak_ws, sum_peak_private)),
                        "observed": {"sumPeakWorkingSetMB": sum_peak_ws, "sumPeakPrivateMB": sum_peak_private, "headroomMB": headroom},
                    }
                )
            memory_assertions = [
                m4.assertion("samples-collected", ">=5 samples", len(valid) >= 5, len(valid)),
                m4.assertion("sum-working-set", "< %.0f MB" % BUDGET_MB, bool(sum_peak_ws is not None and sum_peak_ws < BUDGET_MB), sum_peak_ws),
                m4.assertion("sum-private-commit", "< %.0f MB" % BUDGET_MB, bool(sum_peak_private is not None and sum_peak_private < BUDGET_MB), sum_peak_private),
            ]
            if sampling_error:
                memory_status, memory_classification = "harness-failure", {"kind": "harness", "reason": sampling_error}
            elif all(item["passed"] for item in memory_assertions):
                memory_status, memory_classification = "product-defect", None
            else:
                memory_status, memory_classification = "product-defect", {"kind": "product", "reason": "TUI+MCP 合计内存超出 50 MB 预算"}
            steps.append(
                m4.make_step(
                    "13-memory",
                    "播放中同刻采样 TUI+MCP 内存，合计 < 50 MB x2",
                    {"tuiPid": tui_pid, "mcpPid": mcp_pid, "seconds": args.memory_seconds, "interval": args.memory_interval},
                    {
                        "table": {
                            "process": ["tui", "mcp", "sum"],
                            "peakWorkingSetMB": [tui_peak_ws, mcp_peak_ws, sum_peak_ws],
                            "peakPrivateMB": [tui_peak_private, mcp_peak_private, sum_peak_private],
                        },
                        "budgetMB": BUDGET_MB,
                        "headroomMB": headroom,
                        "sampleCount": len(valid),
                        "口径": memory["scope"],
                        "note": memory["note"],
                        "samplingError": sampling_error,
                    },
                    memory_assertions,
                    failure_status=memory_status,
                    classification=memory_classification or {"kind": "product", "reason": "内存断言失败"},
                )
            )
            if not all(item["passed"] for item in memory_assertions):
                raise m4.Abort("memory budget check failed", (memory_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 14
            pl_before = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            pl_before_sc = pl_before.get("structured") if isinstance(pl_before.get("structured"), dict) else {}
            pl_before_id = track_id_of(pl_before_sc)
            pl_before_position = pl_before_sc.get("positionSeconds") or 0
            rec_pl = mcp.call_tool(
                "music_recommend",
                {"source": "playlists", "limit": 5, "seed": args.seed + 7},
                timeout=args.recommend_timeout,
            )
            rec_pl_sc = rec_pl.get("structured") or {}
            pl_cands = real_candidate_list(rec_pl_sc.get("candidates"))
            pl_is_error = rec_pl.get("isError")
            pl_code = rec_pl_sc.get("error") if isinstance(rec_pl_sc, dict) else None
            account_mode = "unknown"
            pl_obs = {}
            pl_assertions = []
            pl_status = "product-defect"
            pl_classification = {"kind": "product", "reason": "source=playlists 既没有给出 needs_login/not_found 的可解释结果，也没有真实候选"}
            pl_played = None
            if pl_is_error:
                if pl_code == "needs_login":
                    account_mode = "guest"
                elif pl_code == "not_found":
                    account_mode = "signed-in-without-owned-playlists"
                after_pl = mcp.call_tool("music_now_playing", {}, timeout=20.0)
                after_pl_sc = after_pl.get("structured") if isinstance(after_pl.get("structured"), dict) else {}
                unchanged = track_id_of(after_pl_sc) == pl_before_id and after_pl_sc.get("playing") is True
                frame_pl = stable_frame(tui)
                pl_obs = {
                    "isError": pl_is_error,
                    "error": pl_code,
                    "elapsedSeconds": rec_pl.get("elapsedSeconds"),
                    "content": rec_pl.get("contentText")[:300],
                    "accountMode": account_mode,
                    "playbackUnchanged": {
                        "before": {"trackId": pl_before_id, "positionSeconds": pl_before_position},
                        "after": {"trackId": track_id_of(after_pl_sc), "positionSeconds": after_pl_sc.get("positionSeconds"), "playing": after_pl_sc.get("playing")},
                    },
                    "tuiSnapshot": snapshot_excerpt(frame_pl, [core_name((pl_before_sc.get("track") or {}).get("name") or "")]),
                    "note": "游客应 needs_login；已登录但无自建歌单时源码返回 not_found。两者都如实记录，均不算产品缺陷。",
                }
                pl_assertions = [
                    m4.assertion("isError", True, pl_is_error is True, pl_is_error),
                    m4.assertion("explainable-error", "needs_login or not_found", pl_code in ("needs_login", "not_found"), pl_code),
                    m4.assertion("recommend-did-not-play", "same track still playing", unchanged, {"before": pl_before_id, "after": track_id_of(after_pl_sc), "playing": after_pl_sc.get("playing")}),
                    m4.assertion("tui-snapshot-still-playing", "当前曲目 PLAYING", "PLAYING" in frame_pl, pl_obs["tuiSnapshot"]),
                ]
                if pl_code in ("needs_login", "not_found"):
                    pl_status, pl_classification = "product-defect", None
                    if len((frame_pl or "").splitlines()) < 30:
                        pl_status = "harness-failure"
                        pl_classification = {
                            "kind": "harness",
                            "reason": "TUI 快照读取为空/不完整（并发写入竞态，已重试），不能据此判产品缺陷",
                        }
                else:
                    env_hit, env_marker = m4.classify_environment(m4.McpClient.tool_error_text(mcp, rec_pl))
                    if env_hit:
                        pl_status = "environment-failure"
                        pl_classification = {"kind": "environment", "reason": "歌单推荐被环境阻断: %s" % env_marker}
            else:
                account_mode = "signed-in"
                pl_set = rec_pl_sc.get("candidateSetId")
                pr_pl = mcp.call_tool(
                    "music_play_random",
                    {"candidateSetId": pl_set, "seed": args.play_seed + 40},
                    timeout=args.play_call_timeout,
                )
                pr_pl_sc = pr_pl.get("structured") or {}
                pl_cand = pr_pl_sc.get("candidate") if isinstance(pr_pl_sc.get("candidate"), dict) else {}
                pl_cand_id = pl_cand.get("id") if isinstance(pl_cand.get("id"), int) else 0
                pl_poll = poll_playing_track(mcp, pl_cand_id, args.play_timeout, "playlist candidate playing")
                pl_np = pl_poll.get("matchedState") or {}
                pl_snap = wait_snapshot_track(tui, pl_cand.get("name") or "", 10.0)
                pl_played = {"candidate": pl_cand, "matched": pl_poll["matched"], "matchedAtSeconds": pl_poll["matchedAtSeconds"], "nowPlaying": pl_np}
                pl_obs = {
                    "isError": False,
                    "source": rec_pl_sc.get("source"),
                    "candidateSetId": pl_set,
                    "candidateCount": len(pl_cands),
                    "candidates": pl_cands[:5],
                    "playbackStarted": rec_pl_sc.get("playbackStarted"),
                    "accountMode": account_mode,
                    "playRandom": pl_played,
                    "tuiSnapshot": {"matched": pl_snap["matched"], "excerpt": snapshot_excerpt(stable_frame(tui), [core_name(pl_cand.get("name") or "")])},
                }
                pl_assertions = [
                    m4.assertion("candidates-real", ">=1 real candidate with reason", len(pl_cands) >= 1 and all(bool(item.get("reason")) for item in pl_cands), [item.get("reason") for item in pl_cands][:3]),
                    m4.assertion("playbackStarted", False, rec_pl_sc.get("playbackStarted") is False, rec_pl_sc.get("playbackStarted")),
                    m4.assertion("candidateSetId", "rec-*", isinstance(pl_set, str) and pl_set.startswith("rec-"), pl_set),
                    m4.assertion("play-random-isError", False, pr_pl.get("isError") is False, pr_pl.get("isError")),
                    m4.assertion("play-random-playing", "playing=true", pl_poll["matched"], {"pollCount": pl_poll["pollCount"], "matchedAt": pl_poll["matchedAtSeconds"], "error": pl_poll["error"]}),
                    m4.assertion("play-random-track", pl_cand_id, track_id_of(pl_np) == pl_cand_id, track_id_of(pl_np)),
                    m4.assertion("tui-snapshot-playing", "候选曲名 + PLAYING", pl_snap["matched"], pl_obs["tuiSnapshot"]),
                ]
                if not all(item["passed"] for item in pl_assertions):
                    pl_status = "product-defect"
                    pl_classification = {"kind": "product", "reason": "已登录账号的歌单推荐或随机播放失败"}
            state["environment"]["accountMode"] = account_mode
            steps.append(
                m4.make_step(
                    "14-recommend-playlists",
                    "music_recommend(source=playlists) 如实记录",
                    {"tool": "music_recommend", "arguments": {"source": "playlists", "limit": 5, "seed": args.seed + 7}, "accountMode": account_mode},
                    pl_obs,
                    pl_assertions,
                    failure_status=pl_status,
                    classification=pl_classification or {"kind": "product", "reason": "歌单推荐行为不符合契约"},
                    note="游客 needs_login / 登录但无自建歌单 not_found / 已登录则真实候选 + play_random；三种路径都不写账号。",
                )
            )
            if not all(item["passed"] for item in pl_assertions):
                raise m4.Abort(
                    "playlist recommendation branch failed",
                    "harness" if pl_status == "harness-failure" else (pl_classification or {}).get("kind", "product"),
                )


            # ---------------------------------------------------------- step 15
            eof_rc = mcp.close(timeout=args.eof_timeout)
            stdout_audit = m4.audit_stdout(mcp)
            state["mcpStdout"] = stdout_audit
            mcp_stderr = mcp.stderr_text()
            expected_line_count = len([item for item in mcp.requests if item.get("id") is not None])
            eof_assertions = [
                m4.assertion("exit-code", 0, eof_rc == 0, eof_rc),
                m4.assertion("stdout-line-count", expected_line_count, stdout_audit["lineCount"] == expected_line_count, {"lines": stdout_audit["lineCount"], "expected": expected_line_count}),
                m4.assertion("stdout-only-json-rpc", "no ANSI, no free text", len(stdout_audit["problems"]) == 0, stdout_audit["problems"]),
                m4.assertion("response-ids", "one response per request, in order", stdout_audit["idsMatchInOrder"], {"expected": stdout_audit["expectedRequestIds"], "observed": stdout_audit["observedResponseIds"]}),
                m4.assertion("stderr-no-ansi", "no terminal escapes on stderr", "\x1b" not in mcp_stderr, mcp_stderr[-400:]),
            ]
            steps.append(
                m4.make_step(
                    "15-mcp-eof",
                    "关闭 stdin：MCP 退出 0 且 stdout 只有 JSON-RPC",
                    {"action": "close stdin (EOF)", "timeoutSeconds": args.eof_timeout},
                    {
                        "returncode": eof_rc,
                        "stdout": stdout_audit,
                        "stderrTail": mcp_stderr,
                        "note": "stdout 只允许一行一个 JSON-RPC 2.0 对象；带 ANSI 或杂文本即破坏宿主解析。",
                    },
                    eof_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "MCP 在 EOF 后没有干净退出，或 stdout 不纯"},
                )
            )
            if not all(item["passed"] for item in eof_assertions):
                raise m4.Abort("MCP did not exit cleanly on EOF", "product")

            # ---------------------------------------------------------- step 16
            quit_rc = tui.quit(args.quit_timeout)
            release_started = time.monotonic()
            released = False
            while time.monotonic() - release_started < 5.0:
                if not m4.list_mora_pipes():
                    released = True
                    break
                time.sleep(0.2)
            tui.stop_handles()
            report = m4.read_json_file(tui.report)
            state["tuiReport"] = {
                "path": m4.relative(tui.report),
                "written": bool(report),
                "raw": report,
                "fieldsOfInterest": {
                    "mode": (report or {}).get("mode"),
                    "peakWorkingSetMB": (report or {}).get("peakWorkingSetMB"),
                    "peakPrivateMB": (report or {}).get("peakPrivateMB"),
                    "budgetMB": (report or {}).get("budgetMB"),
                    "passed": (report or {}).get("passed"),
                    "audioPlayed": (report or {}).get("audioPlayed"),
                    "maxPositionSeconds": (report or {}).get("maxPositionSeconds"),
                    "audioError": (report or {}).get("audioError"),
                    "actualQuality": (report or {}).get("actualQuality"),
                    "sampleRate": (report or {}).get("sampleRate"),
                    "sampleBits": (report or {}).get("sampleBits"),
                    "seconds": (report or {}).get("seconds"),
                },
            }
            peaks_ok = bool(
                report
                and isinstance(report.get("peakWorkingSetMB"), (int, float))
                and isinstance(report.get("peakPrivateMB"), (int, float))
            )
            audio_played = bool((report or {}).get("audioPlayed"))
            max_position = (report or {}).get("maxPositionSeconds") or 0
            audio_error = str((report or {}).get("audioError") or "")
            cleanup_assertions = [
                m4.assertion("tui-exit-code", 0, quit_rc == 0, quit_rc),
                m4.assertion("report-written", m4.relative(tui.report), bool(report), state["tuiReport"]["path"]),
                m4.assertion("report-mode", "live", (report or {}).get("mode") == "live", (report or {}).get("mode")),
                m4.assertion("report-peaks", "peakWorkingSetMB + peakPrivateMB", peaks_ok, {"peakWorkingSetMB": (report or {}).get("peakWorkingSetMB"), "peakPrivateMB": (report or {}).get("peakPrivateMB")}),
                m4.assertion("audio-evidence", "audioPlayed=true, maxPositionSeconds>0.5", audio_played and max_position > 0.5, {"audioPlayed": audio_played, "maxPositionSeconds": max_position, "audioError": audio_error}),
                m4.assertion("pipe-released", "no mora-player-* pipe", released, m4.list_mora_pipes()),
            ]
            cleanup_status = "product-defect"
            cleanup_classification = {"kind": "product", "reason": "TUI 没有干净退出、报告缺失或音频证据不足"}
            if quit_rc != 0 or not report:
                cleanup_status = "harness-failure"
                cleanup_classification = {"kind": "harness", "reason": "/quit 注入或报告写出失败（隐藏控制台限制）"}
            elif not audio_played and audio_error:
                cleanup_status = "environment-failure"
                cleanup_classification = {"kind": "environment", "reason": "TUI 报告音频设备错误: %s" % audio_error[:160]}
            steps.append(
                m4.make_step(
                    "16-tui-cleanup",
                    "TUI /quit 退出、报告写出、管道释放",
                    {"command": "/quit via WriteConsoleInputW", "report": m4.relative(tui.report)},
                    {
                        "returncode": quit_rc,
                        "pipesAfter": m4.list_mora_pipes(),
                        "report": state["tuiReport"],
                        "stderrTail": tui.stderr_tail(1200),
                    },
                    cleanup_assertions,
                    failure_status=cleanup_status,
                    classification=cleanup_classification,
                )
            )
            if not all(item["passed"] for item in cleanup_assertions):
                raise m4.Abort("TUI cleanup/report failed", (cleanup_classification or {}).get("kind", "product"))
        finally:
            state["cleanup"] = m4.cleanup_processes(resources)
    except m4.Abort as exc:
        state["abort"] = {"reason": exc.reason, "classification": exc.classification}

    # ---------------------------------------------------------- step 17
    binary_after = m4.fingerprint(exe)
    state["binaryAfter"] = binary_after
    fingerprint_changed = bool(
        binary_before.get("exists")
        and binary_after.get("exists")
        and (binary_after.get("sha256") != binary_before.get("sha256"))
    )
    if fingerprint_changed:
        state["exitOverride"] = ("binary-replaced", 2)
    steps.append(
        m4.make_step(
            "17-fingerprint-stable",
            "运行期间二进制未被替换",
            {"path": str(exe), "sha256Before": binary_before.get("sha256"), "sha256After": binary_after.get("sha256")},
            {"before": binary_before, "after": binary_after, "replacedDuringRun": fingerprint_changed, "rebuiltByHarness": False},
            [
                m4.assertion("binary-still-exists", True, bool(binary_after.get("exists")), binary_after.get("path")),
                m4.assertion("sha256-unchanged", binary_before.get("sha256"), not fingerprint_changed, binary_after.get("sha256")),
            ],
            failure_status="harness-failure",
            classification={"kind": "fingerprint", "reason": "被测二进制在运行期间被替换；未重建，按指纹不符退出 2"},
        )
    )

    # Fill steps that never ran after an abort.
    missing_reason = state["abort"]["reason"] if state.get("abort") else "前置步骤未通过，未运行"
    by_id = {item["id"]: item for item in steps}
    ordered = []
    for step_id, title in STEP_ORDER:
        ordered.append(by_id.get(step_id) or m4.not_run_step(step_id, title, missing_reason))
    steps = ordered

    # ---------------------------------------------------------- step 18
    failed_steps = [item for item in steps if item["status"] in ("environment-failure", "product-defect", "harness-failure")]
    failures = [
        {"step": item["id"], "status": item["status"], "classification": item.get("classification")}
        for item in failed_steps
    ]
    environment_failures = [item["id"] for item in failed_steps if item["status"] == "environment-failure"]
    product_defects = [item["id"] for item in failed_steps if item["status"] == "product-defect"]
    harness_failures = [item["id"] for item in failed_steps if item["status"] == "harness-failure"]
    not_run = [item["id"] for item in steps if item["status"] == "not-run"]
    unverified = [
        "空候选：任务给定的空 intent 在真实网易云模糊检索下仍返回候选，empty-pool 的 no_candidates 未能在真实网络触发；本次改用“唯一候选正在播放 / 只有 1 个候选”路径验证 no_candidates 与不跳曲。若必须验证空搜索池，需要离线假服务或一个“只接受精确匹配”的开关。",
        "candidateSets 生命周期：集合只存在 MCP 进程内（LRU 8 个），本次未验证进程重启后同一 candidateSetId 失效（预期 unknown -> not_found），也未测 8 个以上集合的淘汰。",
        "已登录路径：本次账号为游客，source=playlists 只验证了 needs_login；“已登录 + 有自建歌单 -> 真实候选 + play_random”未验证。",
        "失败码覆盖：no_candidates（单候选在播）与 not_found（未知集合）已覆盖；playback_unavailable 只在空-intent 的模糊候选上观测到一次，未逐一验证所有触发条件。",
        "“不会自动换曲重试”：以当前曲目未被跳过、进度继续前进间接验证；未验证连续多首随机、队列内随机与重复不可用曲目的行为。",
        "不同 seed 的分布只记录一组顺序差异，未做统计/去偏；recommend 只在单个 MCP 进程内验证确定性，未跨进程比较。",
        "music_recommend source=auto 的自动选择未单独验证（本次显式 source=search/playlists）。",
        "推荐 reason 的语义质量与多样性未评估，只断言它保留了真实搜索/歌单来源文本。",
        "内存合计只覆盖 TUI+MCP 两个进程；未做 1 小时 soak、VIP/Hi-Res 音质下的合计内存，外部宿主/终端不计入。",
        "真实 MCP 宿主（DST 等）的生命周期/重连、管道 DACL 跨用户边界沿用 M4 的未验证项；本次未重复验证单实例拒绝。",
        "本次没有执行任何账号写操作（红心/收藏/加入歌单），M5 也不包含这些工具。",
    ]
    memory_sum = (state.get("memory") or {}).get("sum") or {}
    classification_observations = {
        "failedSteps": failures,
        "environmentFailures": environment_failures,
        "productDefects": product_defects,
        "harnessFailures": harness_failures,
        "notRun": not_run,
        "abort": state.get("abort"),
        "findings": state["findings"],
        "unverified": unverified,
        "memorySummary": memory_sum,
        "accountMode": state["environment"].get("accountMode"),
        "memoryMeasurementPolicy": "合计=两个 MORA 进程（TUI + MCP 适配器）在同一时刻的 WorkingSetSize 与 PrivateUsage 分别求和；外部宿主/终端不计入。",
        "policy": "不播放无权限曲目、不自动换曲重试、不写账号（不红心/不收藏）、不解灰/换源/随机 IP；推荐只读，播放只提交真实候选 id。",
    }
    classification_assertions = [
        m4.assertion("every-failure-classified", "environment/product/harness", all(item.get("classification") for item in failed_steps), failures),
        m4.assertion("unverified-recorded", "non-empty", len(unverified) > 0, len(unverified)),
        m4.assertion("findings-recorded", "list", isinstance(state["findings"], list), len(state["findings"])),
    ]
    steps.append(
        m4.make_step(
            CLASSIFICATION_STEP[0],
            CLASSIFICATION_STEP[1],
            {"kind": "derived", "dependsOn": [item["id"] for item in steps]},
            classification_observations,
            classification_assertions,
            failure_status="harness-failure",
            classification={"kind": "harness", "reason": "失败步骤缺少分类或未验证项为空"},
        )
    )
    state["failures"] = failures
    state["unverified"] = unverified
    return build_payload(args, exe, steps, state, started_at, harness_extra, console)


# -------------------------------------------------------------------- payload
def verdict_summary(verdict: str) -> str:
    return {
        "passed": "通过：推荐只读且候选真实；同 seed 确定性；随机播放真实开始、排除当前曲目、未知集合 not_found、无候选不跳曲；歌单来源如实返回；双进程合计内存 < 50 MB。",
        "product-defect": "产品缺陷：推荐/随机播放出现了没有达到预期、也没有可解释环境原因的行为。",
        "harness-failed": "测试装置失败：隐藏控制台、进程驱动或内存采样失败，不能当作产品结论。",
        "environment-failed": "环境失败：被网络/权限/音频设备/游客状态/并发播放器阻断；工具如实返回错误，未发现产品缺陷。",
        "binary-mismatch": "被测二进制 SHA256 与期望值不一致，未采信测试结果；未重建。",
        "binary-replaced": "运行期间 build/mora.exe 被替换（脚本没有重建），指纹不符，结果不可采信。",
        "incomplete": "未完成：部分步骤因前置失败未运行。",
    }.get(verdict, verdict)


def compute_verdict(steps, state):
    override = state.get("exitOverride")
    if override:
        verdict, code = override
        return verdict, code, verdict_summary(verdict)
    product = [item["id"] for item in steps if item["status"] == "product-defect"]
    harness = [item["id"] for item in steps if item["status"] == "harness-failure"]
    environment = [item["id"] for item in steps if item["status"] == "environment-failure"]
    pending = [item["id"] for item in steps if item["status"] == "not-run"]
    if product:
        return "product-defect", 1, verdict_summary("product-defect")
    if harness:
        return "harness-failed", 1, verdict_summary("harness-failed")
    if environment:
        return "environment-failed", 3, verdict_summary("environment-failed")
    if pending:
        return "incomplete", 1, verdict_summary("incomplete")
    return "passed", 0, verdict_summary("passed")


def build_payload(args, exe: Path, steps, state, started_at, harness_extra, console) -> dict:
    binary_before = state.get("binaryBefore") or m4.fingerprint(exe)
    binary_after = state.get("binaryAfter")
    expected = args.expected_sha256.upper() if args.expected_sha256 else None
    matches = (binary_before.get("sha256") == expected) if expected else True
    replaced = bool(
        binary_before.get("exists")
        and isinstance(binary_after, dict)
        and binary_after.get("exists")
        and binary_after.get("sha256") != binary_before.get("sha256")
    )
    verdict, code, summary = compute_verdict(steps, state)
    memory = state.get("memory") or {}
    memory_sum = memory.get("sum") or {}
    artifacts_root = Path(args.artifacts).resolve()
    artifact_paths = []
    for name in (m4.SNAPSHOT_NAME, m4.REPORT_NAME, "tui-stderr.txt", "mcp-main-stderr.txt", "detached-worker.log"):
        candidate = artifacts_root / name
        if candidate.exists():
            artifact_paths.append(m4.relative(candidate))
    console_info = (
        {
            "mode": console.mode,
            "columns": console.columns,
            "rows": console.rows,
            "size": console.size,
            "acquireSteps": console.acquire_steps,
        }
        if console is not None
        else {"mode": "unavailable", **harness_extra}
    )
    environment = dict(state.get("environment") or {})
    environment["pipesAfter"] = m4.list_mora_pipes()
    environment["network"] = "real NetEase endpoints through the frozen binary; no proxy, no random IP headers"
    environment["audioDevice"] = "real system audio endpoint of the current Windows user session"
    recommendation = state.get("recommendation") or {}
    processes = {}
    if memory.get("processes"):
        processes = {
            "tuiPid": memory["processes"]["tui"].get("pid"),
            "mcpPid": memory["processes"]["mcp"].get("pid"),
        }
    payload = {
        "schema": "mora-m5-recommend/1",
        "generatedAt": m4.now_iso(),
        "harness": {
            "script": "scripts/m5_recommend_check.py",
            "driverReuse": "scripts/m4_bridge_check.py (hidden console, live TUI, MCP stdio client, same-instant memory sampler)",
            "command": " ".join(sys.argv),
            "pythonVersion": sys.version.split()[0],
            "platform": m4.platform.platform(),
            "host": m4.platform.node(),
            "console": console_info,
            "elapsedSeconds": round(time.monotonic() - started_at, 3),
            "networkRequired": True,
            "binaryFrozen": True,
            "rebuiltByHarness": False,
            "repeatable": True,
            "artifacts": m4.relative(artifacts_root),
            "note": "被测二进制为 build/mora.exe；脚本只读该文件，不构建、不替换。临时快照/报告/日志写入 build/m5-recommend/（.gitignore 已忽略 /build/）。",
            **harness_extra,
        },
        "binary": {
            **(binary_before if binary_before.get("exists") else {"path": str(exe), "exists": False}),
            "expectedSha256": expected,
            "matchesExpected": matches,
            "fingerprintBefore": binary_before,
            "fingerprintAfter": binary_after,
            "replacedDuringRun": replaced,
            "rebuiltByHarness": False,
        },
        "environment": environment,
        "processes": processes,
        "steps": steps,
        "recommendation": {
            "intent": recommendation.get("intent"),
            "seed": recommendation.get("seed"),
            "candidateSetId": recommendation.get("candidateSetId"),
            "candidateIds": recommendation.get("candidateIds"),
            "candidates": next(
                (item["observations"].get("candidates") for item in steps if item["id"] == "6-recommend-no-play"),
                None,
            ),
            "determinism": {
                "sameSeedIds": recommendation.get("sameSeedIds"),
                "differentSeed": recommendation.get("differentSeed"),
                "differentSeedIds": recommendation.get("differentSeedIds"),
                "sameSeedDeterministic": recommendation.get("sameSeedDeterministic"),
                "differentSeedOrderDiffers": recommendation.get("differentSeedOrderDiffers"),
            },
            "playRandom": {
                "first": recommendation.get("firstPlayRandom"),
                "second": recommendation.get("secondPlayRandom"),
                "intent": recommendation.get("intentPlayRandom"),
            },
            "emptyIntent": recommendation.get("emptyIntent"),
            "emptyIntentObserved": recommendation.get("emptyIntentObserved"),
            "literalPlayRandom": recommendation.get("literalPlayRandom"),
            "singleCandidateAttempts": recommendation.get("singleCandidateAttempts"),
            "singleCandidateSet": recommendation.get("singleCandidateSet"),
            "singleCandidate": recommendation.get("singleCandidate"),
            "noCandidatesError": recommendation.get("noCandidatesError"),
            "accountMode": environment.get("accountMode"),
        },
        "sharedStates": state.get("sharedStates"),
        "memory": memory,
        "tuiReport": state.get("tuiReport"),
        "mcpStdout": state.get("mcpStdout"),
        "cleanup": state.get("cleanup"),
        "failures": state.get("failures", []),
        "findings": state.get("findings", []),
        "artifacts": artifact_paths,
        "conclusion": {
            "verdict": verdict,
            "suggestedExitCode": code,
            "summary": summary,
            "productDefects": [item["id"] for item in steps if item["status"] == "product-defect"],
            "environmentFailures": [item["id"] for item in steps if item["status"] == "environment-failure"],
            "harnessFailures": [item["id"] for item in steps if item["status"] == "harness-failure"],
            "notRun": [item["id"] for item in steps if item["status"] == "not-run"],
            "failedSteps": state.get("failures", []),
            "unverified": state.get("unverified", []),
            "findings": state.get("findings", []),
            "abort": state.get("abort"),
            "accountMode": environment.get("accountMode"),
            "memory": {
                "budgetMB": BUDGET_MB,
                "sumPeakWorkingSetMB": memory_sum.get("peakWorkingSetMB"),
                "sumPeakPrivateMB": memory_sum.get("peakPrivateMB"),
                "headroomMB": memory_sum.get("headroomMB"),
                "workingSetPassed": memory_sum.get("workingSetPassed"),
                "privatePassed": memory_sum.get("privatePassed"),
            },
        },
    }
    return payload


# ----------------------------------------------------------------------- main
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="m5_recommend_check.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "M5 recommendation / random-play acceptance for the frozen build/mora.exe.\n"
            "Reuses the M4 hidden-console TUI + MCP stdio driver and the same-instant\n"
            "TUI+MCP memory sampler."
        ),
        epilog=(
            "examples:\n"
            "  python scripts/m5_recommend_check.py\n"
            "  python scripts/m5_recommend_check.py --expected-sha256 <frozen sha256>\n"
            "  python scripts/m5_recommend_check.py --allow-environment-failures\n"
        ),
    )
    parser.add_argument("--exe", default=str(DEFAULT_EXE), help="frozen binary (default: build/mora.exe)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="verification JSON (default: docs/verification/m5-recommend.json)")
    parser.add_argument("--artifacts", default=str(DEFAULT_ARTIFACTS), help="snapshot/report artifacts dir (default: build/m5-recommend)")
    parser.add_argument("--expected-sha256", default="", help="expected frozen SHA256 (optional; recorded only when omitted)")
    parser.add_argument("--columns", type=int, default=116, help="test console columns (default: 116)")
    parser.add_argument("--rows", type=int, default=36, help="test console rows (default: 36)")
    parser.add_argument("--tui-seconds", type=int, default=TUI_SECONDS_DEFAULT, help="TUI --seconds budget (default: 180)")
    parser.add_argument("--tui-timeout", type=float, default=25.0, help="seconds to wait for the home frame (default: 25)")
    parser.add_argument("--intent", default="夜的钢琴曲 石进", help="intent used by recommend/play_random")
    parser.add_argument("--seed", type=int, default=424242, help="deterministic recommendation seed (default: 424242)")
    parser.add_argument("--play-seed", type=int, default=20261024, help="seed used for random playback picks")
    parser.add_argument("--empty-intent", default="zzzz-不存在的歌名-zzzz", help="intent expected to be empty by the task (observed honestly)")
    parser.add_argument("--recommend-timeout", type=float, default=60.0, help="seconds for one recommend (default: 60)")
    parser.add_argument("--play-call-timeout", type=float, default=40.0, help="seconds for one play_random call (default: 40)")
    parser.add_argument("--play-timeout", type=float, default=15.0, help="seconds to poll playing=true (default: 15)")
    parser.add_argument("--memory-seconds", type=float, default=6.0, help="same-instant memory sampling window (default: 6)")
    parser.add_argument("--memory-interval", type=float, default=0.2, help="memory sampling interval (default: 0.2)")
    parser.add_argument("--eof-timeout", type=float, default=20.0, help="seconds to wait for MCP exit after stdin EOF (default: 20)")
    parser.add_argument("--quit-timeout", type=float, default=25.0, help="seconds to wait for TUI /quit (default: 25)")
    parser.add_argument("--allow-environment-failures", action="store_true", help="exit 0 when only environment failures occurred")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--result", default="", help=argparse.SUPPRESS)
    return parser


def minimal_payload(exe: Path, args, verdict: str, summary: str, code: int, extra=None) -> dict:
    binary = m4.fingerprint(exe) if exe.exists() else {"path": str(exe), "exists": False}
    return {
        "schema": "mora-m5-recommend/1",
        "generatedAt": m4.now_iso(),
        "harness": {
            "script": "scripts/m5_recommend_check.py",
            "command": " ".join(sys.argv),
            "pythonVersion": sys.version.split()[0],
            "platform": m4.platform.platform(),
            "host": m4.platform.node(),
            "binaryFrozen": True,
            "rebuiltByHarness": False,
            "note": "未运行被测行为；脚本不构建、不替换 build/mora.exe。",
        },
        "binary": {**binary, "expectedSha256": (args.expected_sha256.upper() if args.expected_sha256 else None)},
        "environment": {"pipesBefore": m4.list_mora_pipes()},
        "steps": [],
        "failures": [],
        "findings": [],
        "conclusion": {"verdict": verdict, "suggestedExitCode": code, "summary": summary, "unverified": [], **(extra or {})},
    }


def main(argv=None) -> int:
    m4.configure_stdout()
    args = build_parser().parse_args(argv)
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    artifacts = Path(args.artifacts).resolve()
    if not exe.exists():
        payload = minimal_payload(
            exe,
            args,
            "environment-failed",
            "build/mora.exe 不存在，无法进行端到端验收；脚本不重建。",
            3,
            {"environmentFailures": ["binary"]},
        )
        payload["_out"] = m4.relative(out)
        m4.write_payload(payload, out, 3)
        m4.print_summary(payload)
        return 3

    if args.expected_sha256:
        actual = m4.sha256_file(exe)
        if actual != args.expected_sha256.upper():
            payload = minimal_payload(
                exe,
                args,
                "binary-mismatch",
                "被测二进制 SHA256 与 --expected-sha256 不一致，未执行测试；未重建。",
                2,
                {"actualSha256": actual, "expectedSha256": args.expected_sha256.upper()},
            )
            payload["binary"]["matchesExpected"] = False
            payload["_out"] = m4.relative(out)
            m4.write_payload(payload, out, 2)
            m4.print_summary(payload)
            print("error: SHA256 mismatch\n  actual  : %s\n  expected: %s" % (actual, args.expected_sha256.upper()), file=sys.stderr)
            return 2

    if args.worker:
        result_path = Path(args.result).resolve()
        console = None
        console_error = ""
        try:
            console = m4.TestConsole(args.columns, args.rows)
        except m4.ConsoleUnavailable as exc:
            console_error = str(exc)
        if console is None:
            payload = minimal_payload(
                exe,
                args,
                "environment-failed",
                "DETACHED_PROCESS 后备进程仍无法创建隐藏测试控制台: %s" % console_error,
                3,
                {"environmentFailures": ["console"]},
            )
        else:
            try:
                payload = execute(args, console, {"consoleAttempt": "detached-worker", "consoleError": console_error})
            finally:
                console.close()
        payload["_out"] = m4.relative(out)
        code = m4.exit_code_for(payload, args.allow_environment_failures)
        m4.write_payload(payload, out, code)
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        m4.print_summary(payload)
        return code

    console = None
    console_error = ""
    try:
        console = m4.TestConsole(args.columns, args.rows)
    except m4.ConsoleUnavailable as exc:
        console_error = str(exc)

    if console is None:
        print("console unavailable (%s); retrying as DETACHED_PROCESS worker ..." % console_error, flush=True)
        artifacts.mkdir(parents=True, exist_ok=True)
        result_path = artifacts / "detached-result.json"
        try:
            result_path.unlink()
        except FileNotFoundError:
            pass
        worker_cmd = [
            sys.executable,
            str(SCRIPT),
            "--worker",
            "--result",
            str(result_path),
            "--exe",
            str(exe),
            "--out",
            str(out),
            "--artifacts",
            str(artifacts),
            "--expected-sha256",
            args.expected_sha256,
            "--columns",
            str(args.columns),
            "--rows",
            str(args.rows),
            "--tui-seconds",
            str(args.tui_seconds),
            "--tui-timeout",
            str(args.tui_timeout),
            "--intent",
            args.intent,
            "--seed",
            str(args.seed),
            "--play-seed",
            str(args.play_seed),
            "--empty-intent",
            args.empty_intent,
            "--recommend-timeout",
            str(args.recommend_timeout),
            "--play-call-timeout",
            str(args.play_call_timeout),
            "--play-timeout",
            str(args.play_timeout),
            "--memory-seconds",
            str(args.memory_seconds),
            "--memory-interval",
            str(args.memory_interval),
            "--eof-timeout",
            str(args.eof_timeout),
            "--quit-timeout",
            str(args.quit_timeout),
        ]
        if args.allow_environment_failures:
            worker_cmd.append("--allow-environment-failures")
        log_path = artifacts / "detached-worker.log"
        with open(log_path, "wb") as log:
            completed = subprocess.run(
                worker_cmd,
                cwd=str(ROOT),
                stdout=log,
                stderr=log,
                creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NO_WINDOW,
                timeout=1200,
            )
        payload = m4.read_json_file(result_path)
        if not payload:
            payload = minimal_payload(
                exe,
                args,
                "harness-failed",
                "无法创建测试控制台，DETACHED_PROCESS 后备也没有产出结果。",
                1,
                {"harnessFailures": ["console"]},
            )
            payload["harness"]["console"] = {"mode": "detached-worker", "error": console_error}
            code = 1
        else:
            payload.setdefault("harness", {})["workerReturncode"] = completed.returncode
            code = m4.exit_code_for(payload, args.allow_environment_failures)
        payload["_out"] = m4.relative(out)
        m4.write_payload(payload, out, code)
        m4.print_summary(payload)
        return code

    try:
        payload = execute(args, console, {"consoleAttempt": "direct"})
    finally:
        console.close()
    payload["_out"] = m4.relative(out)
    code = m4.exit_code_for(payload, args.allow_environment_failures)
    m4.write_payload(payload, out, code)
    m4.print_summary(payload)
    return code


if __name__ == "__main__":
    sys.exit(main())
