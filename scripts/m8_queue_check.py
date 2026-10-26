#!/usr/bin/env python3
r"""MORA M8 queue-control end-to-end check (real TUI + real MCP stdio, hidden console).

What this proves
----------------
M8 adds two MCP tools that let an external agent drive the *one* running player's queue:

  music_queue_list  read-only queue projection (total, 0-based current index, play mode, rows)
  music_queue_edit  one bounded write per call: set_mode / play_index / remove / move / clear /
                    add_next / append

Both must go through the same per-user named-pipe bridge as music_control, must fail honestly with
needs_player when no terminal player owns the session, must reject unknown arguments and
out-of-range indexes with invalid_arguments *before* touching the player, and must never leak
stream URLs, cookies or keys into a tool result.

This harness drives the *frozen* build/mora.exe and observes real behaviour:

   1. fingerprint   record build/mora.exe SHA256; never rebuild or replace it
   2. preflight     no other MORA player may own the session pipe (repeatability)
   3. alone         MCP without a player:
                      * tools/list advertises both queue tools with the frozen schema
                      * queue_edit argument errors -> invalid_arguments (validated before the player)
                      * valid queue writes -> needs_player
                      * music_queue_list -> needs_player
   4. tui           live TUI (no --demo) in a hidden console with --snapshot/--report
   5. attach        a second MCP process connects to the live session (playerSessionAttached=true)
   6. build-queue   music_play + append + add_next build a deterministic 4-row queue
   7. list-shape    total/index/mode/items, exactly one current row, 0-based positions
   8. modes         shuffle (+alias random), repeat_one (+alias single), repeat_all, sequential;
                    seed keeps shuffle deterministic and total/index unchanged
   9. play-index    jump to a row; music_now_playing agrees with the queue snapshot
  10. remove        one row deleted, total-1, index shifts by the documented rule
  11. move          row moved; the playing row follows its content, not its old slot
  12. shuffle-walk  consecutive next() in shuffle visits every other row exactly once
  13. repeat-one    next() in repeat_one replays the same row instead of leaving it
  14. bounded       limit/offset produce a 0-based bounded window inside the same total
  15. out-of-range  index >= total -> invalid_arguments and *no* partial queue write
  16. clear         total=0, items=[], music_now_playing.queueSize=0
  17. hygiene       no stream URL / cookie / token / key material in any queue tool result
  18. stdout        MCP stdout is pure JSON-RPC (no ANSI, no prose)
  19. cleanup       /quit, report written, pipe released, binary unchanged

Exit codes
----------
  0  every step passed (or only environment failures with --allow-environment-failures)
  1  product defect or harness failure
  2  binary fingerprint mismatch (nothing rebuilt)
  3  environment / permission / network / console failure only

Only build/m8-queue/** (gitignored artifacts) and the --out JSON (default
docs/verification/m8-queue.json) are written. The harness never builds.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import m4_bridge_check as m4  # noqa: E402  (TestConsole / TuiProcess / McpClient / fingerprint)

DEFAULT_EXE = ROOT / "build" / "mora.exe"
DEFAULT_OUT = ROOT / "docs" / "verification" / "m8-queue.json"
DEFAULT_ARTIFACTS = ROOT / "build" / "m8-queue"

QUEUE_TOOLS = ("music_queue_list", "music_queue_edit")
OP_VALUES = ("set_mode", "play_index", "remove", "move", "clear", "add_next", "append", "replace")
MODE_VALUES = ("sequential", "repeat_all", "repeat_one", "shuffle")
ROW_KEYS = {"position", "id", "name", "artist", "album", "durationSeconds", "current"}

# Argument errors must be decided before any player lookup, so they must never turn into
# needs_player. Each entry is (tool, arguments, expected error code).
INVALID_ARGUMENTS = [
    ("music_queue_list", {"limit": 0}, "invalid_arguments"),
    ("music_queue_list", {"limit": 101}, "invalid_arguments"),
    ("music_queue_list", {"limit": 1.5}, "invalid_arguments"),
    ("music_queue_list", {"offset": -1}, "invalid_arguments"),
    ("music_queue_list", {"offset": 10001}, "invalid_arguments"),
    ("music_queue_list", {"unexpected": 1}, "invalid_arguments"),
    ("music_queue_edit", {}, "invalid_arguments"),
    ("music_queue_edit", {"op": 7}, "invalid_arguments"),
    ("music_queue_edit", {"op": "explode"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "set_mode"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "set_mode", "mode": 5}, "invalid_arguments"),
    ("music_queue_edit", {"op": "set_mode", "mode": "explode"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "set_mode", "mode": "shuffle", "index": 1}, "invalid_arguments"),
    ("music_queue_edit", {"op": "play_index"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "play_index", "index": -1}, "invalid_arguments"),
    ("music_queue_edit", {"op": "play_index", "index": 10000}, "invalid_arguments"),
    ("music_queue_edit", {"op": "remove"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "move", "from": 0}, "invalid_arguments"),
    ("music_queue_edit", {"op": "move", "to": 1}, "invalid_arguments"),
    ("music_queue_edit", {"op": "move", "from": 0, "to": 1, "index": 2}, "invalid_arguments"),
    ("music_queue_edit", {"op": "clear", "index": 0}, "invalid_arguments"),
    ("music_queue_edit", {"op": "clear", "surprise": 1}, "invalid_arguments"),
    ("music_queue_edit", {"op": "append"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "append", "ids": []}, "invalid_arguments"),
    ("music_queue_edit", {"op": "append", "ids": [1, "x"]}, "invalid_arguments"),
    ("music_queue_edit", {"op": "append", "ids": [0]}, "invalid_arguments"),
    ("music_queue_edit", {"op": "append", "ids": list(range(1, 52))}, "invalid_arguments"),
    ("music_queue_edit", {"op": "replace"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "replace", "ids": []}, "invalid_arguments"),
    ("music_queue_edit", {"op": "replace", "ids": [1, "x"]}, "invalid_arguments"),
    ("music_queue_edit", {"op": "replace", "id": 1, "ids": [2]}, "invalid_arguments"),
    ("music_queue_edit", {"op": "replace", "ids": list(range(1, 1002))}, "invalid_arguments"),
    ("music_queue_edit", {"op": "replace", "ids": [1, 2], "mode": "shuffle"}, "invalid_arguments"),
    ("music_queue_edit", {"op": "add_next", "id": 0}, "invalid_arguments"),
    ("music_queue_edit", {"op": "add_next", "id": 1, "ids": [2]}, "invalid_arguments"),
]

# Valid writes/reads that simply cannot be served without the authoritative player.
NEEDS_PLAYER = [
    ("music_queue_list", {}),
    ("music_queue_list", {"limit": 10}),
    ("music_queue_edit", {"op": "set_mode", "mode": "shuffle"}),
    ("music_queue_edit", {"op": "clear"}),
    ("music_queue_edit", {"op": "play_index", "index": 0}),
    ("music_queue_edit", {"op": "append", "ids": [1, 2]}),
    ("music_queue_edit", {"op": "add_next", "id": 1}),
    ("music_queue_edit", {"op": "replace", "ids": [1, 2, 3]}),
]

HYGIENE_MARKERS = ("http://", "https://", "cookie", "token", "authorization", "streamUrl",
                   "playUrl", "session.bin", "password", "MUSIC_U")

# Steps that only make sense once a live queue exists; recorded as not-run when an earlier step
# aborts so the report never implies they were verified.
PENDING_STEPS = [
    ("7-list-shape", "music_queue_list 投影形状"),
    ("8-modes", "播放模式与别名"),
    ("9-play-index", "play_index 跳转"),
    ("10-remove", "remove 删除"),
    ("11-move", "move 移动"),
    ("12-shuffle-walk", "shuffle 遍历不重复"),
    ("13-repeat-one-next", "repeat_one 重播同一行"),
    ("14-bounded-window", "limit/offset 有界窗口"),
    ("15-out-of-range", "越界 invalid_arguments 且无部分写入"),
    ("16-clear", "clear 清空"),
    ("16b-replace", "replace 换整组且 append/add_next 不替换"),
    ("16c-album-series", "music_play(kind=album) 用整张专辑替换队列"),
    ("17-hygiene", "队列结果无敏感字段"),
    ("18-mcp-stdout", "stdout 纯 JSON-RPC"),
]


def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def profile_pipe_suffix(profile: Path) -> str:
    """Reproduce src/bridge.cpp::bridgePipeName()'s --data-dir suffix (FNV-1a over the wide path).

    The player and the adapter both derive their private pipe from MORA_DATA_DIR, so the harness
    can preflight exactly its own profile instead of treating the user's player as an obstruction.
    """
    text = str(profile)
    value = 1469598103934665603
    for char in text:
        value ^= ord(char)
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%08x" % (value & 0xFFFFFFFF)


class ProfileTui(m4.TuiProcess):
    """TuiProcess with --data-dir so the TUI owns a private session pipe."""

    def __init__(self, exe, console, artifacts, seconds, data_dir):
        super().__init__(exe, console, artifacts, seconds)
        self.argv = [self.argv[0], "--data-dir", str(data_dir)] + self.argv[1:]


def owned_profile_pipes(suffix: str):
    target = "-" + suffix
    return [name for name in m4.list_mora_pipes() if name.lower().endswith(target)]


def check(name: str, expected, passed: bool, observed) -> dict:
    return {"name": name, "expected": expected, "passed": bool(passed), "observed": observed}


def step(sid, title, input_info, observations, assertions, status="product-defect", note=""):
    ok = all(item["passed"] for item in assertions)
    entry = {
        "id": sid,
        "title": title,
        "status": "passed" if ok else status,
        "input": input_info,
        "observations": observations,
        "assertions": assertions,
        "passed": ok,
    }
    if not ok:
        entry["classification"] = {"kind": "product" if status == "product-defect" else status}
    if note:
        entry["note"] = note
    return entry


def not_run(sid, title, reason):
    return {
        "id": sid,
        "title": title,
        "status": "not-run",
        "input": None,
        "observations": {"reason": reason},
        "assertions": [],
        "passed": False,
    }


def tool_error(record: dict):
    structured = record.get("structured")
    if isinstance(structured, dict) and structured.get("error"):
        return structured.get("error")
    text = record.get("contentText") or ""
    for code in ("needs_player", "invalid_arguments", "busy", "queue_boundary", "unknown_method",
                 "disconnected", "not_found", "playback_unavailable", "no_candidates"):
        if text.startswith(code) or (" " + code) in text:
            return code
    return None


def structured(record: dict) -> dict:
    value = record.get("structured")
    return value if isinstance(value, dict) else {}


def rows_of(state: dict):
    items = state.get("items")
    return items if isinstance(items, list) else []


def ids_of(state: dict):
    return [row.get("id") for row in rows_of(state) if isinstance(row, dict)]


def queue_list(mcp, arguments=None, timeout=20.0, label=""):
    arguments = arguments if arguments is not None else {}
    record = mcp.call_tool("music_queue_list", arguments, timeout=timeout)
    return record


def queue_edit_once(mcp, arguments, timeout=25.0):
    """One raw queue_edit call: used when the harness must observe a busy reply verbatim."""
    return mcp.call_tool("music_queue_edit", arguments, timeout=timeout)


def queue_edit(mcp, arguments, timeout=25.0, attempts=12, delay=0.5):
    """queue_edit with a bounded harness-side retry on an explicit busy reply only.

    service.cpp never retries a write; the player answers busy while its serialized worker is
    still finishing the previous request (for example the network fetch right after music_play),
    so the harness re-issues the same intent a bounded number of times.
    """
    record = None
    for attempt in range(attempts):
        record = mcp.call_tool("music_queue_edit", arguments, timeout=timeout)
        if not record.get("isError") or tool_error(record) != "busy" or attempt == attempts - 1:
            return record
        time.sleep(delay)
    return record


def wait_for(predicate, timeout, interval=0.2):
    started = time.monotonic()
    last = None
    while time.monotonic() - started < timeout:
        last = predicate()
        if last:
            return last, round(time.monotonic() - started, 3)
        time.sleep(interval)
    return None, round(time.monotonic() - started, 3)


def shuffle_walk(mcp, seed, count):
    """Deterministic shuffle walk.

    Observed semantics: Queue::rebuildShuffle() forces the *current* row to the front of the
    shuffle order, and set_mode is a no-op when the mode is already current. So the walk only
    repeats for the same seed when the mode is switched to sequential and row 0 is selected
    *before* shuffle is (re)enabled.
    """
    queue_edit(mcp, {"op": "set_mode", "mode": "sequential"})
    queue_edit(mcp, {"op": "play_index", "index": 0})
    queue_edit(mcp, {"op": "set_mode", "mode": "shuffle", "seed": seed})
    walked = [structured(queue_list(mcp)).get("index")]
    for _ in range(count):
        control, _retries = retry_on_busy(lambda: mcp.call_tool(
            "music_control", {"action": "next"}, timeout=20.0))
        if control.get("isError"):
            break
        walked.append(structured(queue_list(mcp)).get("index"))
    return walked


def now_playing(mcp, timeout=20.0):
    return structured(mcp.call_tool("music_now_playing", {}, timeout=timeout))


def retry_on_busy(call, attempts=12, delay=0.5):
    """Harness-side bounded retry for an explicit busy reply only (never for other errors).

    The player answers busy while its serialized worker is finishing the previous request (for
    example fetching a playback URL right after music_play). service.cpp never retries writes;
    the harness may re-issue the same intent a bounded number of times on an explicit busy code.
    """
    record = None
    for index in range(attempts):
        record = call()
        if not record.get("isError") or tool_error(record) != "busy":
            return record, index
        time.sleep(delay)
    return record, attempts


def wait_player_settled(mcp, timeout=10.0, interval=0.25):
    """Wait until the player is no longer mid-request: playing, or an honest error is reported."""
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = now_playing(mcp)
        if last.get("playing") is True:
            return last
        if last.get("lastError") or last.get("audioError"):
            return last
        time.sleep(interval)
    return last


# ------------------------------------------------------------------- execute
def execute(args, console: m4.TestConsole) -> dict:
    started_at = time.monotonic()
    exe = Path(args.exe).resolve()
    artifacts = Path(args.artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    steps = []
    limitations = []
    environment_failures = []
    state = {}

    # ---------------------------------------------------------------- step 1
    binary_before = m4.fingerprint(exe)
    if not binary_before.get("exists"):
        steps.append(
            step(
                "1-binary-fingerprint",
                "记录被测二进制指纹",
                {"path": str(exe)},
                {"exists": False},
                [check("binary-exists", True, False, str(exe))],
                status="environment-failure",
                note="脚本不构建、不替换二进制。",
            )
        )
        return finish(steps, binary_before, None, state, started_at, environment_failures,
                      ["build/mora.exe 不存在，未执行任何被测行为"], args)

    expected = args.expected_sha256.upper() if args.expected_sha256 else None
    matches = (binary_before["sha256"] == expected) if expected else True
    steps.append(
        step(
            "1-binary-fingerprint",
            "记录被测二进制指纹",
            {"path": str(exe), "expectedSha256": expected},
            {"sizeBytes": binary_before["sizeBytes"], "sha256": binary_before["sha256"],
             "lastWriteTime": binary_before["lastWriteTime"], "matchesExpected": matches,
             "rebuiltByHarness": False},
            [check("binary-exists", True, True, binary_before["relativePath"]),
             check("sha256-matches-expected", expected or "(record only)", matches,
                   binary_before["sha256"])],
            status="harness-failure",
        )
    )
    if not matches:
        state["exitOverride"] = ("binary-mismatch", 2)
        return finish(steps, binary_before, None, state, started_at, environment_failures,
                      ["SHA256 与 --expected-sha256 不一致，未执行测试"], args)

    # ---------------------------------------------------------------- step 2
    # The M8 tools must not touch the player a human is actually listening to. Both the TUI and
    # this harness's adapters run under an explicit --data-dir, which src/bridge.cpp turns into a
    # private pipe name; only that profile's own pipe is a genuine conflict.
    profile = Path(args.data_dir).resolve()
    suffix = profile_pipe_suffix(profile)
    all_pipes_before = m4.list_mora_pipes()
    pipes_before = owned_profile_pipes(suffix)
    state["profile"] = {"dataDir": str(profile), "pipeSuffix": suffix,
                        "ownedPipes": pipes_before, "allPipes": all_pipes_before}
    steps.append(
        step(
            "2-preflight",
            "预检：本测试 profile 的会话管道空闲（不打扰用户正在使用的播放器）",
            {"listedPipeRoot": "\\\\.\\pipe\\", "dataDir": str(profile)},
            {"ownedProfilePipes": pipes_before, "allMoraPipes": all_pipes_before,
             "pipeSuffix": suffix},
            [check("no-foreign-player-on-this-profile", "[]", len(pipes_before) == 0,
                   pipes_before)],
            status="environment-failure",
            note="默认 profile 上可能有用户自己的播放器在跑；本用例用 --data-dir 隔离开，"
                 "既不结束它，也不对它发任何命令。",
        )
    )
    if pipes_before:
        state["exitOverride"] = ("environment-failed", 3)
        environment_failures.append("本 profile 的会话管道已被占用 %s" % (pipes_before,))
        return finish(steps, binary_before, None, state, started_at, environment_failures,
                      ["同一 profile 已有播放器，无法验证 needs_player 路径"], args)

    alone = m4.McpClient(exe, artifacts, "alone")
    tui = None
    attached = None
    try:
        # ------------------------------------------------------------ step 3
        alone.start()
        init = alone.request(
            "initialize",
            {"protocolVersion": "2025-11-25", "capabilities": {},
             "clientInfo": {"name": "mora-m8-queue-check", "version": "1"}},
            timeout=20.0,
        )
        init_result = init["message"].get("result") or {}
        alone.notify("notifications/initialized")
        tools_resp = alone.request("tools/list", {}, timeout=20.0)
        tools = (tools_resp["message"].get("result") or {}).get("tools", [])
        by_name = {tool.get("name"): tool for tool in tools if isinstance(tool, dict)}
        list_tool = by_name.get("music_queue_list") or {}
        edit_tool = by_name.get("music_queue_edit") or {}
        list_schema = list_tool.get("inputSchema") or {}
        edit_schema = edit_tool.get("inputSchema") or {}
        op_schema = (edit_schema.get("properties") or {}).get("op") or {}
        list_annotations = list_tool.get("annotations") or {}
        edit_annotations = edit_tool.get("annotations") or {}
        list_limit = (list_schema.get("properties") or {}).get("limit") or {}
        edit_ids = (edit_schema.get("properties") or {}).get("ids") or {}
        edit_description = edit_tool.get("description") or ""
        list_description = list_tool.get("description") or ""
        search_description = (by_name.get("music_search") or {}).get("description") or ""
        play_description = (by_name.get("music_play") or {}).get("description") or ""
        random_description = (by_name.get("music_play_random") or {}).get("description") or ""
        series_text = " ".join((edit_description, list_description, search_description,
                                play_description))

        # No-player negatives, in two batches so the report stays readable.
        invalid_records = []
        for tool, arguments, expected_code in INVALID_ARGUMENTS:
            record = alone.call_tool(tool, arguments, timeout=20.0)
            invalid_records.append({"tool": tool, "arguments": arguments, "expected": expected_code,
                                    "isError": record.get("isError"),
                                    "error": tool_error(record)})
        needs_records = []
        for tool, arguments in NEEDS_PLAYER:
            record = alone.call_tool(tool, arguments, timeout=20.0)
            needs_records.append({"tool": tool, "arguments": arguments,
                                  "isError": record.get("isError"),
                                  "error": tool_error(record)})

        status_resp = alone.call_tool("music_get_status", {}, timeout=20.0)
        attached_flag = structured(status_resp).get("playerSessionAttached")
        alone_tool_names = sorted(by_name)
        leaked = sorted({marker for marker in HYGIENE_MARKERS
                         if marker.lower() in json.dumps(
                             {"invalid": invalid_records, "needs": needs_records},
                             ensure_ascii=False).lower()})

        steps.append(
            step(
                "3-queue-tools-without-player",
                "无播放器：参数错误先于播放器校验，合法调用如实 needs_player",
                {"command": [str(exe), "--mcp"], "toolCount": len(tools)},
                {
                    "initialize": {"protocolVersion": init_result.get("protocolVersion"),
                                   "serverInfo": init_result.get("serverInfo")},
                    "tools": alone_tool_names,
                    "listDescription": list_description,
                    "editDescription": edit_description,
                    "searchDescription": search_description,
                    "playDescription": play_description,
                    "randomDescription": random_description,
                    "listAnnotations": list_annotations,
                    "editAnnotations": edit_annotations,
                    "opEnum": op_schema.get("enum"),
                    "invalidArguments": invalid_records,
                    "needsPlayer": needs_records,
                    "playerSessionAttached": attached_flag,
                    "leakedMarkers": leaked,
                },
                [
                    check("queue-tools-advertised", sorted(QUEUE_TOOLS),
                          all(name in by_name for name in QUEUE_TOOLS), alone_tool_names),
                    check("queue_list-readOnly", True, list_annotations.get("readOnlyHint") is True,
                          list_annotations),
                    check("queue_edit-writable", False,
                          edit_annotations.get("readOnlyHint") is False, edit_annotations),
                    check("schemas-close-objects", False,
                          list_schema.get("additionalProperties") is False and
                          edit_schema.get("additionalProperties") is False,
                          {"list": list_schema.get("additionalProperties"),
                           "edit": edit_schema.get("additionalProperties")}),
                    check("queue_list-limit<=100", 100, list_limit.get("maximum") == 100,
                          list_limit),
                    check("queue_edit-op-enum", list(OP_VALUES),
                          op_schema.get("enum") == list(OP_VALUES), op_schema.get("enum")),
                    check("replace-ids<=1000", 1000, edit_ids.get("maxItems") == 1000,
                          edit_ids.get("maxItems")),
                    check("descriptions-document-0-based", True,
                          "0-based" in list_description and "0-based" in edit_description,
                          {"list": "0-based" in list_description,
                           "edit": "0-based" in edit_description}),
                    check("descriptions-cover-player-semantics", True,
                          all(word in edit_description for word in
                              ("shuffle", "repeat_one", "repeat_all", "sequential", "play next",
                               "clear", "replace", "append", "add_next")),
                          edit_description),
                    check("description-documents-series-playback", True,
                          all(word in series_text for word in
                              ("album", "music_search", "music_recommend", "replace", "5..20")),
                          {"edit": "replace" in edit_description,
                           "search": "op=replace" in search_description,
                           "play": "album" in play_description}),
                    check("description-warns-single-song-defect", True,
                          "defect" in edit_description and "defect" in search_description,
                          {"edit": "defect" in edit_description,
                           "search": "defect" in search_description}),
                    check("description-covers-vague-requests", True,
                          "music_play_random" in edit_description and
                          "vague" in random_description,
                          {"edit": "music_play_random" in edit_description,
                           "random": "vague" in random_description}),
                    check("argument-errors-are-invalid_arguments",
                          [entry[2] for entry in INVALID_ARGUMENTS],
                          all(entry["isError"] is True and entry["error"] == entry["expected"]
                              for entry in invalid_records),
                          [{"expected": entry["expected"], "observed": entry["error"]}
                           for entry in invalid_records
                           if entry["error"] != entry["expected"]] or "all as expected"),
                    check("valid-calls-are-needs_player", "needs_player",
                          all(entry["isError"] is True and entry["error"] == "needs_player"
                              for entry in needs_records),
                          needs_records),
                    check("status-not-attached", False, attached_flag is False, attached_flag),
                    check("no-sensitive-markers", [], len(leaked) == 0, leaked),
                ],
                status="product-defect",
                note="参数校验发生在 session() 之前，因此无播放器时也要先回 invalid_arguments。",
            )
        )
        if not steps[-1]["passed"]:
            raise m4.Abort("queue tools are missing or misbehave without a player", "product")
        alone.close(timeout=20.0)

        # ------------------------------------------------------------ step 4
        tui = ProfileTui(exe, console, artifacts, args.tui_seconds, args.data_dir)
        tui.start()
        frame, waited = tui.wait(lambda text: len(text) > 100, args.tui_timeout)
        started_ok = frame is not None
        profile_pipes = owned_profile_pipes(suffix)
        steps.append(
            step(
                "4-tui-start",
                "启动真实 TUI 并等待首页快照",
                {"argv": tui.argv},
                {"waitSeconds": waited, "frameLength": len(frame or ""),
                 "pid": tui.proc.pid if tui.proc else None,
                 "ownedProfilePipes": profile_pipes,
                 "dataDir": str(profile)},
                [check("tui-rendered-home", True, started_ok, {"waitSeconds": waited}),
                 check("tui-owns-private-profile-pipe", ">=1", len(profile_pipes) >= 1,
                       profile_pipes)],
                status="environment-failure",
                note="--data-dir 让 TUI 拥有独立会话管道；--seconds/--snapshot/--report 与 "
                     "m4_bridge_check.py 相同。",
            )
        )
        if not started_ok:
            environment_failures.append("TUI 未在 %ss 内渲染首页快照" % args.tui_timeout)
            state["exitOverride"] = ("environment-failed", 3)
            raise m4.Abort("TUI did not render", "environment")

        # ------------------------------------------------------------ step 5
        attached = m4.McpClient(exe, artifacts, "attached")
        attached.start()
        attached.request(
            "initialize",
            {"protocolVersion": "2025-11-25", "capabilities": {},
             "clientInfo": {"name": "mora-m8-queue-check-attached", "version": "1"}},
            timeout=20.0,
        )
        attached.notify("notifications/initialized")
        status_view = structured(attached.call_tool("music_get_status", {}, timeout=20.0))
        session_view = status_view.get("session") if isinstance(status_view.get("session"), dict) else {}
        steps.append(
            step(
                "5-mcp-attach",
                "第二个 MCP 进程接入同一播放会话",
                {"command": [str(exe), "--mcp"]},
                {"playerSessionAttached": status_view.get("playerSessionAttached"),
                 "queueSize": session_view.get("queueSize")},
                [check("playerSessionAttached", True,
                       status_view.get("playerSessionAttached") is True,
                       status_view.get("playerSessionAttached"))],
                status="environment-failure",
            )
        )
        if not steps[-1]["passed"]:
            environment_failures.append("MCP 无法接入 TUI 会话（可能有别的适配器占用管道）")
            state["exitOverride"] = ("environment-failed", 3)
            raise m4.Abort("MCP could not attach to the live session", "environment")

        # ------------------------------------------------------------ step 6
        if args.song_ids.strip():
            song_ids = []
            for piece in args.song_ids.replace(" ", ",").split(","):
                if piece.strip().isdigit() and int(piece) > 0 and int(piece) not in song_ids:
                    song_ids.append(int(piece))
            search_items = []
            search_note = "cli"
        else:
            search = attached.call_tool(
                "music_search",
                {"query": args.search_query, "kind": "song", "limit": 10},
                timeout=args.search_timeout,
            )
            search_items = structured(search).get("items")
            search_items = search_items if isinstance(search_items, list) else []
            song_ids = []
            for item in search_items:
                if isinstance(item, dict) and isinstance(item.get("id"), int) and item["id"] > 0:
                    if item["id"] not in song_ids:
                        song_ids.append(item["id"])
            search_note = "search"
        seed_ids = song_ids[:4]
        build_observations = {"searchQuery": args.search_query, "searchSource": search_note,
                              "searchIds": song_ids, "usedIds": seed_ids}
        build_assertions = [
            check("search-real-ids", ">=4 positive ids", len(song_ids) >= 4, song_ids),
        ]
        if len(song_ids) >= 4:
            play_record, busy_retries = retry_on_busy(
                lambda: attached.call_tool("music_play", {"id": seed_ids[0]}, timeout=30.0)
            )
            settled = wait_player_settled(attached, timeout=args.settle_timeout)
            first_list = queue_list(attached)
            first_state = structured(first_list)
            # First append attempt is captured verbatim: a busy or rejected call must leave the
            # queue untouched (the tool layer never retries; the harness may re-issue on busy).
            append_probe = queue_edit_once(attached,
                                           {"op": "append", "ids": [seed_ids[1], seed_ids[2]]})
            append_probe_state = structured(queue_list(attached))
            if append_probe.get("isError"):
                append_probe_assertion = check(
                    "busy-append-writes-nothing", ids_of(first_state),
                    ids_of(append_probe_state) == ids_of(first_state),
                    {"error": tool_error(append_probe), "ids": ids_of(append_probe_state)})
                append_record = queue_edit(attached,
                                           {"op": "append", "ids": [seed_ids[1], seed_ids[2]]})
                after_append = structured(queue_list(attached))
            else:
                append_probe_assertion = check(
                    "append-is-atomic", 2,
                    (append_probe_state.get("total") or 0) == (first_state.get("total") or 0) + 2,
                    {"before": first_state.get("total"),
                     "after": append_probe_state.get("total")})
                append_record = append_probe
                after_append = append_probe_state
            add_next_record = queue_edit(attached, {"op": "add_next", "ids": [seed_ids[3]]})
            built = structured(queue_list(attached))

            build_observations.update(
                {
                    "play": {"isError": play_record.get("isError"),
                             "error": tool_error(play_record), "busyRetries": busy_retries},
                    "settled": settled,
                    "afterPlay": first_state,
                    "appendProbe": {"isError": append_probe.get("isError"),
                                    "error": tool_error(append_probe),
                                    "state": append_probe_state},
                    "append": {"isError": append_record.get("isError"),
                               "error": tool_error(append_record)},
                    "afterAppend": after_append,
                    "addNext": {"isError": add_next_record.get("isError"),
                                "error": tool_error(add_next_record)},
                    "built": built,
                    "note": "busy 由 harness 有界重发同一意图；service.cpp 本身不重试写操作。",
                }
            )
            expected_order = [seed_ids[0], seed_ids[3], seed_ids[1], seed_ids[2]]
            build_assertions.extend(
                [
                    check("music_play-established-a-row", seed_ids[0],
                          first_state.get("total") == 1 and ids_of(first_state) == [seed_ids[0]],
                          {"total": first_state.get("total"), "ids": ids_of(first_state)}),
                    check("append-adds-two-without-replacing", 3,
                          after_append.get("total") == 3 and
                          ids_of(after_append) == [seed_ids[0], seed_ids[1], seed_ids[2]],
                          {"total": after_append.get("total"), "ids": ids_of(after_append)}),
                    check("add_next-inserts-without-replacing", expected_order,
                          built.get("total") == 4 and ids_of(built) == expected_order,
                          {"total": built.get("total"), "ids": ids_of(built),
                           "index": built.get("index")}),
                    append_probe_assertion,
                    check("inserted-rows-carry-real-metadata", True,
                          any(row.get("name") for row in rows_of(built)),
                          [{"id": row.get("id"), "name": row.get("name")}
                           for row in rows_of(built)]),
                ]
            )
            if not all(item["passed"] for item in build_assertions):
                # Distinguish "the player has not implemented the queue bridge yet" from a real
                # environment limit: unknown_method means the adapter is fine and the TUI is old.
                bridge_missing = any(tool_error(record) == "unknown_method"
                                     for record in (first_list, append_record, add_next_record))
                if bridge_missing:
                    environment_failures.append(
                        "播放器桥接未实现 queue_list/queue_edit（unknown_method），队列写入路径"
                        "尚未端到端打通")
                    state["exitOverride"] = ("blocked", 1)
                else:
                    environment_failures.append("无法建立 4 行队列（真实搜索/播放环境受限）")
                    state["exitOverride"] = ("environment-failed", 3)
        build_status = "blocked" if state.get("exitOverride", ("", 0))[0] == "blocked" \
            else "environment-failure"
        steps.append(
            step("6-build-queue", "music_play + append + add_next 建立确定性队列",
                 {"searchQuery": args.search_query},
                 build_observations, build_assertions, status=build_status,
                 note="append 加队尾、add_next 插在当前曲目之后。")
        )
        if len(song_ids) < 4 or not steps[-1]["passed"]:
            if steps[-1]["status"] == "blocked":
                raise m4.Abort("player bridge does not implement queue_list/queue_edit yet",
                               "blocked")
            raise m4.Abort("could not build a deterministic queue from real search results",
                           "environment")
        queue_state = built

        # ------------------------------------------------------------ step 7
        list_snapshot = structured(queue_list(attached, {"limit": 100}))
        rows = rows_of(list_snapshot)
        positions = [row.get("position") for row in rows]
        current_rows = [row for row in rows if row.get("current") is True]
        row_key_sets = {tuple(sorted(row.keys())) for row in rows if isinstance(row, dict)}
        expected_keys = tuple(sorted(ROW_KEYS))
        steps.append(
            step(
                "7-list-shape",
                "music_queue_list 投影：total/index/mode/行字段/唯一 current",
                {"tool": "music_queue_list", "arguments": {"limit": 100}},
                {"state": list_snapshot, "rowKeySets": [list(keys) for keys in row_key_sets]},
                [
                    check("total-matches-built", 4, list_snapshot.get("total") == 4,
                          list_snapshot.get("total")),
                    check("mode-is-a-known-name", list(MODE_VALUES),
                          list_snapshot.get("mode") in MODE_VALUES, list_snapshot.get("mode")),
                    check("index-in-range", "0..total-1",
                          isinstance(list_snapshot.get("index"), int) and
                          0 <= list_snapshot["index"] < 4, list_snapshot.get("index")),
                    check("positions-are-0-based", [0, 1, 2, 3], positions == [0, 1, 2, 3],
                          positions),
                    check("exactly-one-current-row", 1, len(current_rows) == 1,
                          len(current_rows)),
                    check("row-keys-exact", list(expected_keys),
                          row_key_sets == {expected_keys}, [list(keys) for keys in row_key_sets]),
                    check("row-ids-positive", True,
                          all(isinstance(row.get("id"), int) and row["id"] > 0 for row in rows),
                          ids_of(list_snapshot)),
                ],
            )
        )

        # ------------------------------------------------------------ step 8
        mode_records = []
        mode_assertions = []
        baseline = structured(queue_list(attached))
        for mode, expected_mode in (("shuffle", "shuffle"), ("random", "shuffle"),
                                    ("repeat_one", "repeat_one"), ("single", "repeat_one"),
                                    ("repeat_all", "repeat_all"),
                                    ("sequential", "sequential")):
            arguments = {"op": "set_mode", "mode": mode}
            if mode == "shuffle":
                arguments["seed"] = 7
            record, retries = retry_on_busy(lambda arguments=arguments: queue_edit(attached, arguments))
            observed = structured(record)
            mode_records.append({"mode": mode, "expectedMode": expected_mode,
                                 "isError": record.get("isError"), "error": tool_error(record),
                                 "observed": observed, "busyRetries": retries})
            mode_assertions.append(
                check("set_mode:%s" % mode, expected_mode,
                      record.get("isError") is False and observed.get("mode") == expected_mode and
                      observed.get("total") == baseline.get("total") and
                      observed.get("index") == baseline.get("index"),
                      {"mode": observed.get("mode"), "total": observed.get("total"),
                       "index": observed.get("index"), "error": tool_error(record)})
            )
        # Deterministic shuffle: the same seed reproduces the same walk.
        first_walk = shuffle_walk(attached, 99, 3)
        second_walk = shuffle_walk(attached, 99, 3)
        mode_assertions.append(
            check("shuffle-seed-reproducible", first_walk,
                  first_walk == second_walk and sorted(first_walk) == [0, 1, 2, 3],
                  {"first": first_walk, "second": second_walk})
        )
        steps.append(
            step("8-modes", "播放模式：shuffle/repeat_one/repeat_all/sequential 与别名",
                 {"ops": [record["mode"] for record in mode_records]},
                 {"records": mode_records, "shuffleWalkFirst": first_walk,
                  "shuffleWalkSecond": second_walk},
                 mode_assertions)
        )

        # ------------------------------------------------------------ step 9
        restored = structured(queue_edit(attached, {"op": "set_mode", "mode": "sequential"}))
        index_before = restored.get("index")
        target = None
        for candidate in range(4):
            if candidate != index_before:
                target = candidate
                break
        jump_record = structured(queue_edit(attached, {"op": "play_index", "index": target}))
        jump_rows = rows_of(jump_record)
        expected_track = jump_rows[target].get("id") if target < len(jump_rows) else None

        def index_agrees():
            view = now_playing(attached)
            return view if view.get("queueIndex") == target else None

        matched_index, matched_after = wait_for(index_agrees, 6.0)

        def track_settled():
            view = now_playing(attached)
            track = view.get("track")
            if isinstance(track, dict) and track.get("id") == expected_track:
                return view
            if view.get("lastError") or view.get("audioError"):
                return view
            return None

        settled_view, settled_after = wait_for(track_settled, 8.0)
        final_view = settled_view if isinstance(settled_view, dict) else now_playing(attached)
        final_track = final_view.get("track")
        observed_track = final_track.get("id") if isinstance(final_track, dict) else None
        audio_started = final_view.get("playing") is True
        track_agrees = observed_track == expected_track
        player_error = bool(final_view.get("lastError") or final_view.get("audioError"))
        if player_error:
            limitations.append(
                "隔离 profile 没有账号会话，%s；起播失败时播放器保留上一首，因此曲目 id 一致性"
                "只在播放器没有报错时断言。" % (final_view.get("lastError") or
                                              final_view.get("audioError")))
        steps.append(
            step(
                "9-play-index",
                "play_index 跳转到指定行，music_now_playing 的队列状态与快照一致",
                {"op": "play_index", "index": target, "indexBefore": index_before},
                {"state": jump_record, "nowPlaying": final_view,
                 "matchedAfterSeconds": matched_after, "settledAfterSeconds": settled_after,
                 "audioStarted": audio_started, "expectedTrackId": expected_track,
                 "observedTrackId": observed_track,
                 "playerError": final_view.get("lastError") or final_view.get("audioError")},
                [
                    check("index-moved", target, jump_record.get("index") == target,
                          jump_record.get("index")),
                    check("total-unchanged", 4, jump_record.get("total") == 4,
                          jump_record.get("total")),
                    check("now-playing-queueIndex-agrees", target, matched_index is not None,
                          {"matchedAfterSeconds": matched_after,
                           "queueIndex": final_view.get("queueIndex")}),
                    check("track-id-agrees-when-playing", True,
                          track_agrees or player_error or not audio_started,
                          {"playing": audio_started, "trackAgrees": track_agrees,
                           "error": final_view.get("lastError") or final_view.get("audioError"),
                           "expected": expected_track, "observed": observed_track}),
                ],
                note="music_play.startIndex 与这里的 index 都是 0 基。",
            )
        )

        # ------------------------------------------------------------ step 10
        before_remove = structured(queue_list(attached))
        before_ids = ids_of(before_remove)
        before_index = before_remove.get("index")
        remove_record = structured(queue_edit(attached, {"op": "remove", "index": 0}))
        after_remove = structured(queue_list(attached))
        expected_ids = before_ids[1:]
        expected_index = before_index - 1 if before_index > 0 else 0
        steps.append(
            step(
                "10-remove",
                "remove 删除指定行，一次调用只删一行且当前行索引按规则回退",
                {"op": "remove", "index": 0},
                {"before": before_remove, "record": remove_record, "after": after_remove},
                [
                    check("total-decremented-once", 3, after_remove.get("total") == 3,
                          after_remove.get("total")),
                    check("removed-id-gone", expected_ids, ids_of(after_remove) == expected_ids,
                          ids_of(after_remove)),
                    check("index-shifted", expected_index, after_remove.get("index") == expected_index,
                          after_remove.get("index")),
                ],
            )
        )

        # ------------------------------------------------------------ step 11
        queue_edit(attached, {"op": "play_index", "index": 0})
        before_move = structured(queue_list(attached))
        before_move_ids = ids_of(before_move)
        last = before_move.get("total") - 1
        move_record = structured(queue_edit(attached, {"op": "move", "from": 0, "to": last}))
        after_move = structured(queue_list(attached))
        expected_move_ids = before_move_ids[1:] + [before_move_ids[0]]
        steps.append(
            step(
                "11-move",
                "move 调整行位置，正在播放的行跟着自己的内容走",
                {"op": "move", "from": 0, "to": last},
                {"before": before_move, "record": move_record, "after": after_move},
                [
                    check("order-rotated", expected_move_ids, ids_of(after_move) == expected_move_ids,
                          ids_of(after_move)),
                    check("total-unchanged", before_move.get("total"),
                          after_move.get("total") == before_move.get("total"),
                          after_move.get("total")),
                    check("index-followed-row", last, after_move.get("index") == last,
                          after_move.get("index")),
                ],
            )
        )

        # ------------------------------------------------------------ step 12
        walk_total = structured(queue_list(attached)).get("total")
        walked = shuffle_walk(attached, 11, max(0, (walk_total or 1) - 1))
        steps.append(
            step(
                "12-shuffle-walk",
                "shuffle 连续 next 走遍其余每一行且不重复",
                {"op": "set_mode", "mode": "shuffle", "seed": 11,
                 "nextCalls": max(0, (walk_total or 1) - 1)},
                {"walkedIndexes": walked, "total": walk_total},
                [
                    check("visits-each-other-row-once", list(range(walk_total or 0)),
                          sorted(walked) == list(range(walk_total or 0)) and
                          len(set(walked)) == walk_total, walked),
                ],
            )
        )

        # ------------------------------------------------------------ step 13
        queue_edit(attached, {"op": "set_mode", "mode": "sequential"})
        queue_edit(attached, {"op": "play_index", "index": 0})
        repeat_one_mode = structured(queue_edit(attached, {"op": "set_mode", "mode": "repeat_one"}))
        row_before_next = structured(queue_list(attached))
        control, retries = retry_on_busy(lambda: attached.call_tool(
            "music_control", {"action": "next"}, timeout=20.0))
        row_after_next = structured(queue_list(attached))
        steps.append(
            step(
                "13-repeat-one-next",
                "repeat_one 下 next 重播同一行而不是跳走",
                {"op": "set_mode", "mode": "repeat_one", "then": "music_control next"},
                {"mode": repeat_one_mode.get("mode"), "before": row_before_next,
                 "after": row_after_next, "busyRetries": retries},
                [
                    check("mode-is-repeat_one", "repeat_one",
                          repeat_one_mode.get("mode") == "repeat_one",
                          repeat_one_mode.get("mode")),
                    check("index-unchanged", row_before_next.get("index"),
                          row_after_next.get("index") == row_before_next.get("index"),
                          row_after_next.get("index")),
                    check("queue-unchanged", row_before_next.get("total"),
                          row_after_next.get("total") == row_before_next.get("total"),
                          row_after_next.get("total")),
                ],
            )
        )
        queue_edit(attached, {"op": "set_mode", "mode": "sequential"})

        # ------------------------------------------------------------ step 14
        before_window = structured(queue_list(attached))
        window = structured(queue_list(attached, {"limit": 1, "offset": 1}))
        window_rows = rows_of(window)
        steps.append(
            step(
                "14-bounded-window",
                "limit/offset 在同一个 total 内做 0 基窗口投影",
                {"tool": "music_queue_list", "arguments": {"limit": 1, "offset": 1}},
                {"state": window, "fullTotal": before_window.get("total")},
                [
                    check("total-is-full-queue", before_window.get("total"),
                          window.get("total") == before_window.get("total"),
                          {"windowTotal": window.get("total"),
                           "fullTotal": before_window.get("total")}),
                    check("window-has-one-row", 1, len(window_rows) == 1, len(window_rows)),
                    check("window-position-is-1", 1,
                          window_rows and window_rows[0].get("position") == 1,
                          window_rows[0].get("position") if window_rows else None),
                    check("limit-echoed", 1, window.get("limit") == 1, window.get("limit")),
                    check("offset-echoed", 1, window.get("offset") == 1, window.get("offset")),
                ],
            )
        )

        # ------------------------------------------------------------ step 15
        guard_before = structured(queue_list(attached))
        out_of_range = []
        for arguments in ({"op": "play_index", "index": 99},
                          {"op": "remove", "index": 99},
                          {"op": "move", "from": 0, "to": 99}):
            record, _retries = retry_on_busy(lambda arguments=arguments: queue_edit(attached, arguments))
            out_of_range.append({"arguments": arguments, "isError": record.get("isError"),
                                 "error": tool_error(record),
                                 "state": structured(record)})
        guard_after = structured(queue_list(attached))
        same_index = structured(queue_edit(attached, {"op": "play_index",
                                                      "index": guard_after.get("index")}))
        guard_final = structured(queue_list(attached))
        steps.append(
            step(
                "15-out-of-range",
                "越界索引 -> invalid_arguments 且不产生部分写入；同索引为幂等 no-op",
                {"ops": [entry["arguments"] for entry in out_of_range]},
                {"records": out_of_range, "before": guard_before, "after": guard_after,
                 "sameIndexState": same_index, "final": guard_final},
                [
                    check("out-of-range-is-invalid_arguments", "invalid_arguments",
                          all(entry["isError"] is True and entry["error"] == "invalid_arguments"
                              for entry in out_of_range),
                          [{"arguments": entry["arguments"], "error": entry["error"]}
                           for entry in out_of_range]),
                    check("no-partial-write", {"ids": ids_of(guard_before),
                                               "index": guard_before.get("index")},
                          ids_of(guard_after) == ids_of(guard_before) and
                          guard_after.get("index") == guard_before.get("index") and
                          guard_after.get("mode") == guard_before.get("mode"),
                          {"ids": ids_of(guard_after), "index": guard_after.get("index"),
                           "mode": guard_after.get("mode")}),
                    check("same-index-is-noop", True,
                          same_index.get("index") == guard_after.get("index") and
                          ids_of(guard_final) == ids_of(guard_before), same_index),
                ],
            )
        )

        # ------------------------------------------------------------ step 16
        clear_record = structured(queue_edit(attached, {"op": "clear"}))
        cleared = structured(queue_list(attached))
        cleared_view = now_playing(attached)
        steps.append(
            step(
                "16-clear",
                "clear 清空队列且 music_now_playing 如实报告 queueSize=0",
                {"op": "clear"},
                {"record": clear_record, "state": cleared, "nowPlaying": cleared_view},
                [
                    check("total-zero", 0, cleared.get("total") == 0, cleared.get("total")),
                    check("items-empty", 0, len(rows_of(cleared)) == 0, rows_of(cleared)),
                    check("index-zero", 0, cleared.get("index") == 0, cleared.get("index")),
                    check("now-playing-queueSize-zero", 0,
                          cleared_view.get("queueSize") == 0, cleared_view.get("queueSize")),
                ],
            )
        )

        # ------------------------------------------------------------ step 16b
        # The user-reported defect: an agent asked for a whole series queued a single song, and
        # "add more" replaced the queue instead of extending it. Both directions are asserted here.
        replace_ids = [seed_ids[0], seed_ids[1], seed_ids[2]]
        replace_record = structured(queue_edit(attached, {"op": "replace", "ids": replace_ids}))
        replaced = structured(queue_list(attached))
        append_record2 = structured(queue_edit(attached, {"op": "append", "ids": [seed_ids[3]]}))
        appended_after_replace = structured(queue_list(attached))
        add_next_record2 = structured(queue_edit(attached, {"op": "add_next", "ids": [seed_ids[0]]}))
        add_next_after = structured(queue_list(attached))
        steps.append(
            step(
                "16b-replace",
                "replace 换掉整组并从第一首播放；append/add_next 只增不替换",
                {"replace": replace_ids, "thenAppend": [seed_ids[3]],
                 "thenAddNext": [seed_ids[0]]},
                {"record": replace_record, "state": replaced,
                 "appendRecord": append_record2, "afterAppend": appended_after_replace,
                 "addNextRecord": add_next_record2, "afterAddNext": add_next_after},
                [
                    check("replace-sets-exact-order", replace_ids,
                          replaced.get("total") == 3 and ids_of(replaced) == replace_ids,
                          {"total": replaced.get("total"), "ids": ids_of(replaced),
                           "error": tool_error(replace_record)}),
                    check("replace-starts-at-first", 0, replaced.get("index") == 0,
                          replaced.get("index")),
                    check("replace-op-echoed", "replace", replace_record.get("op") == "replace",
                          replace_record.get("op")),
                    check("append-keeps-existing-rows", 4,
                          appended_after_replace.get("total") == 4 and
                          ids_of(appended_after_replace)[:3] == replace_ids,
                          {"total": appended_after_replace.get("total"),
                           "ids": ids_of(appended_after_replace)}),
                    check("add_next-keeps-existing-rows", replace_ids + [seed_ids[3]],
                          add_next_after.get("total") == 5 and
                          ids_of(add_next_after)[1:] == replace_ids + [seed_ids[3]],
                          {"total": add_next_after.get("total"),
                           "ids": ids_of(add_next_after)}),
                    check("add_next-lands-after-current", seed_ids[0],
                          len(ids_of(add_next_after)) > 1 and
                          ids_of(add_next_after)[1] == seed_ids[0],
                          ids_of(add_next_after)[:2]),
                ],
                note="replace=换成这一组；append=加到队尾；add_next=插到当前曲之后（后两者不替换）。",
            )
        )

        # ------------------------------------------------------------ step 16c
        # The user-reported defect for a "series" request: an album must replace the queue with
        # the whole collection instead of queueing a single song.
        album_search = attached.call_tool(
            "music_search", {"query": args.search_query, "kind": "album", "limit": 5},
            timeout=args.search_timeout)
        album_items = structured(album_search).get("items")
        album_items = album_items if isinstance(album_items, list) else []
        album_id = None
        for item in album_items:
            if isinstance(item, dict) and isinstance(item.get("id"), int) and item["id"] > 0:
                album_id = item["id"]
                break
        if album_id is None:
            steps.append(not_run("16c-album-series",
                                 "music_play(kind=album) 用整张专辑替换队列",
                                 "music_search(kind=album) 没有返回可用的专辑 id"))
        else:
            before_album = structured(queue_list(attached))
            album_play = attached.call_tool(
                "music_play", {"kind": "album", "id": album_id, "startIndex": 0}, timeout=30.0)
            album_state = before_album
            settled_album = {}
            deadline = time.monotonic() + max(30.0, args.settle_timeout + 20.0)
            while time.monotonic() < deadline:
                album_state = structured(queue_list(attached))
                settled_album = now_playing(attached)
                if (ids_of(album_state) != ids_of(before_album) and
                        (album_state.get("total") or 0) > 1):
                    break
                if settled_album.get("lastError") or settled_album.get("audioError"):
                    break
                time.sleep(0.4)
            album_changed = ids_of(album_state) != ids_of(before_album)
            album_error = (settled_album.get("lastError") or settled_album.get("audioError") or
                           (tool_error(album_play) if album_play.get("isError") else ""))
            if not album_changed and not album_error:
                album_status = "product-defect"
            elif not album_changed:
                album_status = "environment-failure"
                environment_failures.append(
                    "隔离 profile 无法匿名取到专辑曲目（%s），music_play(kind=album) 未改变队列"
                    % album_error)
            else:
                album_status = "product-defect"
            steps.append(
                step(
                    "16c-album-series",
                    "music_play(kind=album) 用整张专辑替换队列而不是只播一首",
                    {"tool": "music_play",
                     "arguments": {"kind": "album", "id": album_id, "startIndex": 0}},
                    {"before": before_album,
                     "play": {"isError": album_play.get("isError"),
                              "error": tool_error(album_play)},
                     "after": album_state, "playerError": album_error},
                    [
                        check("album-queue-has-many-rows", ">1",
                              album_changed and (album_state.get("total") or 0) > 1,
                              album_state.get("total")),
                        check("album-starts-at-first", 0, album_state.get("index") == 0,
                              album_state.get("index")),
                        check("album-replaced-the-queue", True, album_changed,
                              {"before": ids_of(before_album)[:3],
                               "after": ids_of(album_state)[:3]}),
                    ],
                    status=album_status,
                    note="整张专辑走 music_play(kind=album)：替换队列并整组播放。",
                )
            )

        # ------------------------------------------------------------ step 17
        queue_payloads = [before_remove, list_snapshot, after_remove, after_move, window,
                          guard_before, guard_after, cleared, mode_records, replaced,
                          appended_after_replace, add_next_after]
        blob = json.dumps(queue_payloads, ensure_ascii=False).lower()
        leaked_live = sorted({marker for marker in HYGIENE_MARKERS if marker.lower() in blob})
        steps.append(
            step(
                "17-hygiene",
                "队列工具结果不含流地址/Cookie/Token/密钥",
                {"sampledResults": len(queue_payloads)},
                {"leakedMarkers": leaked_live},
                [check("no-sensitive-markers", 0, len(leaked_live) == 0, leaked_live)],
            )
        )

        # ------------------------------------------------------------ step 18
        audit = m4.audit_stdout(attached)
        steps.append(
            step(
                "18-mcp-stdout",
                "MCP stdout 只有 JSON-RPC（无 ANSI、无散文）",
                {"lineCount": audit["lineCount"]},
                audit,
                [
                    check("no-ansi", False, audit["ansiFound"] is False, audit["ansiFound"]),
                    check("no-non-json", False, audit["nonJsonFound"] is False,
                          audit["problems"][:5]),
                    check("ids-match-in-order", True, audit["idsMatchInOrder"] is True,
                          {"expected": len(audit["expectedRequestIds"]),
                           "observed": len(audit["observedResponseIds"])}),
                ],
            )
        )
    except m4.Abort as exc:
        state.setdefault("abort", {"reason": exc.reason, "classification": exc.classification})
        known = {item["id"] for item in steps}
        for sid, title in PENDING_STEPS:
            if sid not in known:
                steps.append(not_run(sid, title, "aborted earlier: " + exc.reason))
    finally:
        # ------------------------------------------------------------ step 19
        cleanup = {"mcpReturncodes": {}, "tui": None}
        if attached is not None:
            cleanup["mcpReturncodes"]["attached"] = attached.close(timeout=20.0)
        if alone.proc is not None and alone.proc.poll() is None:
            cleanup["mcpReturncodes"]["alone"] = alone.close(timeout=20.0)
        if tui is not None and tui.proc is not None:
            cleanup["tui"] = {"returncode": tui.quit(args.quit_timeout),
                              "report": str(tui.report), "snapshot": str(tui.snapshot),
                              "stderrTail": tui.stderr_tail(800)}
            tui.stop_handles()
        time.sleep(0.4)
        cleanup["ownedPipesAfter"] = owned_profile_pipes(suffix)
        cleanup["allPipesAfter"] = m4.list_mora_pipes()
        binary_after = m4.fingerprint(exe)
        cleanup["binaryUnchanged"] = (binary_after.get("sha256") == binary_before.get("sha256"))
        cleanup["binaryAfterSha256"] = binary_after.get("sha256")
        if not cleanup["binaryUnchanged"]:
            environment_failures.append(
                "构建产物在运行期间被并发构建替换（共享 checkout 允许多人同时 ./build.ps1）；"
                "本轮被测进程的行为仍然有效，但指纹以开始时记录的 " + str(binary_before.get("sha256")) +
                " 为准，重跑前请确认构建已静止")
        steps.append(
            step(
                "19-cleanup",
                "退出 TUI、释放本 profile 会话管道、二进制未被替换",
                {"quitTimeoutSeconds": args.quit_timeout},
                cleanup,
                [
                    check("mcp-exited-cleanly", {role: 0 for role in cleanup["mcpReturncodes"]},
                          all(code in (0, None) for code in cleanup["mcpReturncodes"].values()),
                          cleanup["mcpReturncodes"]),
                    check("tui-exited", 0,
                          (not cleanup["tui"]) or cleanup["tui"]["returncode"] == 0,
                          cleanup["tui"]["returncode"] if cleanup["tui"] else "not started"),
                    check("profile-pipe-released", "[]", len(cleanup["ownedPipesAfter"]) == 0,
                          cleanup["ownedPipesAfter"]),
                    check("binary-unchanged", True, cleanup["binaryUnchanged"],
                          cleanup["binaryUnchanged"]),
                ],
                status="environment-failure",
            )
        )

    return finish(steps, binary_before, None, state, started_at, environment_failures, limitations,
                  args)


def finish(steps, binary_before, binary_after, state, started_at, environment_failures,
           limitations, args) -> dict:
    passed = sum(1 for item in steps if item.get("passed"))
    failed = [item["id"] for item in steps if item.get("status") == "product-defect"]
    blocked = [item["id"] for item in steps if item.get("status") == "blocked"]
    env_failed = [item["id"] for item in steps if item.get("status") == "environment-failure"]
    not_run = [item["id"] for item in steps if item.get("status") == "not-run"]
    aborted = state.get("abort")
    payload = {
        "script": "scripts/m8_queue_check.py",
        "stage": "m8-queue-modes",
        "startedAt": None,
        "finishedAt": now_iso(),
        "elapsedSeconds": round(time.monotonic() - started_at, 3),
        "binary": binary_before,
        "tools": list(QUEUE_TOOLS),
        "profile": state.get("profile"),
        "steps": steps,
        "summary": {
            "stepCount": len(steps),
            "passed": passed,
            "failed": len(failed),
            "blocked": len(blocked),
            "environmentFailures": len(env_failed),
            "notRun": len(not_run),
            "failedSteps": failed,
            "blockedSteps": blocked,
            "environmentFailedSteps": env_failed,
            "notRunSteps": not_run,
            "environmentFailureReasons": environment_failures,
            "abort": aborted,
            "exitOverride": state.get("exitOverride"),
        },
        "limitations": limitations,
    }
    if failed or (aborted and aborted.get("classification") == "harness"):
        payload["result"] = "failed"
    elif blocked or (aborted and aborted.get("classification") == "blocked"):
        payload["result"] = "blocked"
    elif env_failed or environment_failures or (aborted and
                                               aborted.get("classification") == "environment"):
        payload["result"] = "environment-failed"
    else:
        payload["result"] = "passed"
    return payload


def print_summary(payload: dict) -> None:
    summary = payload["summary"]
    print("MORA M8 queue check: %s  (passed=%d failed=%d blocked=%d environment=%d notRun=%d)" % (
        payload["result"], summary["passed"], summary["failed"], summary.get("blocked", 0),
        summary["environmentFailures"], summary.get("notRun", 0)))
    for item in payload["steps"]:
        print("  [%s] %s - %s" % (item["status"], item["id"], item["title"]))
        for assertion in item["assertions"]:
            if not assertion["passed"]:
                print("      FAIL %s expected=%r observed=%r" % (
                    assertion["name"], assertion["expected"], assertion["observed"]))
    if payload.get("summary", {}).get("abort"):
        print("  abort: %s" % payload["summary"]["abort"])
    for limitation in payload.get("limitations", []):
        print("  limitation: %s" % limitation)
    print("REPORT_JSON " + json.dumps(payload, ensure_ascii=False, separators=(",", ":")))


def write_payload(payload: dict, out: Path) -> None:
    payload["reportPath"] = str(out)
    try:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        payload["reportWritten"] = True
    except OSError as exc:
        payload["reportWritten"] = False
        payload["reportError"] = str(exc)
        print("note: could not write %s (%s); printing only" % (out, exc))


def exit_code(payload: dict, allow_environment_failures: bool) -> int:
    override = payload.get("summary", {}).get("exitOverride")
    result = payload["result"]
    if result == "passed":
        return 0
    if result == "environment-failed":
        return 0 if allow_environment_failures else 3
    return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="MORA M8 queue control end-to-end check")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--artifacts", default=str(DEFAULT_ARTIFACTS))
    parser.add_argument("--search-query", default="周杰伦")
    parser.add_argument("--search-timeout", type=float, default=45.0)
    parser.add_argument(
        "--song-ids",
        default="",
        help="Optional comma-separated real song ids (>=4). Skips music_search; useful when the "
             "isolated profile has no signed-in account.",
    )
    parser.add_argument("--tui-seconds", type=int, default=240)
    parser.add_argument("--tui-timeout", type=float, default=40.0)
    parser.add_argument("--settle-timeout", type=float, default=12.0,
                        help="Seconds to wait for the player to finish the previous request.")
    parser.add_argument("--quit-timeout", type=float, default=25.0)
    parser.add_argument("--columns", type=int, default=100)
    parser.add_argument("--rows", type=int, default=30)
    parser.add_argument("--expected-sha256", default=None)
    parser.add_argument("--allow-environment-failures", action="store_true")
    parser.add_argument(
        "--data-dir",
        default=str(DEFAULT_ARTIFACTS / "profile"),
        help="Isolated profile for TUI+MCP (src/bridge.cpp derives a private pipe from it). "
             "Must live under build/.",
    )
    parser.add_argument("--keep-profile", action="store_true",
                        help="Keep the isolated profile directory instead of deleting it.")
    parser.add_argument("--shuffle-forensics", action="store_true",
                        help="Run only the shuffle-seed forensics (QUE-12): locate why the same "
                             "seed does not always replay the same order on a live player.")
    parser.add_argument("--forensics-out",
                        default=str(ROOT / "docs" / "verification" / "m8-shuffle-forensics.json"))
    return parser


def run_shuffle_forensics(args, console) -> dict:
    """Locate the QUE-12 divergence from outside the player, using only MCP-visible behaviour.

    Hypothesis under test: `PlaybackSession::setMode(value, seed)` returns early when the mode is
    already `value`, so issuing set_mode(shuffle, seed) a second time does NOT rebuild the shuffle
    order - the order left over from `Queue::assign` (which rebuilds with the hard-coded seed 1
    when the mode is already Shuffle) is what the caller hears. The bridge response exposes the
    early return as `changed:false`, which is visible from this side without touching src/.
    """
    exe = Path(args.exe).resolve()
    artifacts = Path(args.artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    tui = None
    mcp = None
    payload = {
        "script": "scripts/m8_queue_check.py",
        "mode": "shuffle-forensics",
        "hypothesis": "set_mode(shuffle, seed) is a no-op when the mode is already shuffle, so the "
                      "audible shuffle order comes from the earlier rebuild (assign uses seed 1)",
        "binary": m4.fingerprint(exe),
        "scenarios": [],
        "assertions": [],
        "result": "failed",
    }
    try:
        tui = ProfileTui(exe, console, artifacts, args.tui_seconds, args.data_dir)
        tui.start()
        rendered, waited = tui.wait(lambda text: len(text) > 100, args.tui_timeout)
        payload["tuiStart"] = {"rendered": rendered is not None, "waitSeconds": waited}
        if rendered is None:
            payload["result"] = "environment-failed"
            return payload
        mcp = m4.McpClient(exe, artifacts, "forensics")
        mcp.start()
        mcp.request("initialize",
                    {"protocolVersion": "2025-11-25", "capabilities": {},
                     "clientInfo": {"name": "mora-m8-shuffle-forensics", "version": "1"}},
                    timeout=20.0)
        mcp.notify("notifications/initialized")

        ids = []
        for piece in args.song_ids.replace(" ", ",").split(","):
            if piece.strip().isdigit() and int(piece) > 0 and int(piece) not in ids:
                ids.append(int(piece))
        if len(ids) < 5:
            search = mcp.call_tool(
                "music_search", {"query": args.search_query, "kind": "song", "limit": 10},
                timeout=args.search_timeout)
            for item in structured(search).get("items") or []:
                if (isinstance(item, dict) and isinstance(item.get("id"), int) and
                        item["id"] > 0 and item["id"] not in ids):
                    ids.append(item["id"])
        ids = ids[:5]
        payload["queueIds"] = ids
        if len(ids) < 5:
            payload["result"] = "environment-failed"
            payload["reason"] = "需要 5 首真实歌曲 id 才能观测排列"
            return payload

        def run_scenario(label, setup, description):
            before = structured(queue_list(mcp))
            detail = setup()
            after = structured(queue_list(mcp))
            quiescent = [after.get("index")]
            time.sleep(0.25)
            quiescent.append(structured(queue_list(mcp)).get("index"))
            samples = [quiescent[-1]]
            errors = []
            for _ in range(len(ids)):
                control, _retries = retry_on_busy(lambda: mcp.call_tool(
                    "music_control", {"action": "next"}, timeout=20.0))
                if control.get("isError"):
                    errors.append(tool_error(control))
                    break
                samples.append(structured(queue_list(mcp)).get("index"))
            record = {
                "label": label,
                "description": description,
                "modeBefore": before.get("mode"),
                "modeAfter": after.get("mode"),
                "indexAtSetup": after.get("index"),
                "indexQuiescent": quiescent,
                "samples": samples,
                "errors": errors,
                "detail": detail,
            }
            payload["scenarios"].append(record)
            return record

        # Known starting point: sequential + replace(ids), so every scenario starts from the same
        # 5-row queue. `replace` in sequential mode does not rebuild any shuffle order.
        queue_edit(mcp, {"op": "set_mode", "mode": "sequential"})
        queue_edit(mcp, {"op": "replace", "ids": ids})

        def transition():
            queue_edit(mcp, {"op": "set_mode", "mode": "sequential"})
            queue_edit(mcp, {"op": "play_index", "index": 0})
            reply = structured(queue_edit(mcp, {"op": "set_mode", "mode": "shuffle", "seed": 7}))
            return {"setShuffleChanged": reply.get("changed")}

        def same_mode():
            queue_edit(mcp, {"op": "play_index", "index": 0})
            reply = structured(queue_edit(mcp, {"op": "set_mode", "mode": "shuffle", "seed": 7}))
            return {"setShuffleChanged": reply.get("changed")}

        def same_mode_without_seed():
            queue_edit(mcp, {"op": "play_index", "index": 0})
            reply = structured(queue_edit(mcp, {"op": "set_mode", "mode": "shuffle"}))
            return {"setShuffleChanged": reply.get("changed")}

        def assign_in_shuffle():
            reply = structured(queue_edit(mcp, {"op": "replace", "ids": ids}))
            queue_edit(mcp, {"op": "play_index", "index": 0})
            seeded = structured(queue_edit(mcp, {"op": "set_mode", "mode": "shuffle", "seed": 7}))
            return {"modeAtAssign": reply.get("mode"),
                    "setShuffleChanged": seeded.get("changed")}

        def assign_in_sequential():
            queue_edit(mcp, {"op": "set_mode", "mode": "sequential"})
            reply = structured(queue_edit(mcp, {"op": "replace", "ids": ids}))
            queue_edit(mcp, {"op": "play_index", "index": 0})
            seeded = structured(queue_edit(mcp, {"op": "set_mode", "mode": "shuffle", "seed": 7}))
            return {"modeAtAssign": reply.get("mode"),
                    "setShuffleChanged": seeded.get("changed")}

        s1 = run_scenario("S1-mode-transition", transition,
                          "sequential -> play_index 0 -> set_mode(shuffle, seed 7)")
        s2 = run_scenario("S2-same-mode-again", same_mode,
                          "play_index 0 -> set_mode(shuffle, seed 7) while already shuffle")
        s3 = run_scenario("S3-mode-transition-again", transition,
                          "repeat S1: should replay S1 exactly")
        s4 = run_scenario("S4-assign-while-shuffle", assign_in_shuffle,
                          "replace while already shuffle -> then set_mode(shuffle, seed 7)")
        s5 = run_scenario("S5-assign-while-sequential", assign_in_sequential,
                          "sequential -> replace -> play_index 0 -> set_mode(shuffle, seed 7)")
        s6 = run_scenario("S6-no-seed-while-shuffle", same_mode_without_seed,
                          "play_index 0 -> set_mode(shuffle) without a seed while already shuffle")

        def same(a, b):
            return a["samples"] == b["samples"]

        def permutation(record):
            head = record["samples"][:len(ids)]
            return sorted(head) == list(range(len(ids))) and len(set(head)) == len(ids)

        payload["assertions"] = [
            check("all-paths-reproduce-the-same-seed-order",
                  {"S1": s1["samples"], "S2": s2["samples"], "S3": s3["samples"],
                   "S4": s4["samples"], "S5": s5["samples"]},
                  same(s1, s2) and same(s1, s3) and same(s1, s4) and same(s1, s5),
                  {"S1": s1["samples"], "S2": s2["samples"], "S3": s3["samples"],
                   "S4": s4["samples"], "S5": s5["samples"]}),
            check("explicit-seed-rebuilds-even-when-mode-unchanged",
                  "S2 == S1 (set_mode(shuffle,7) while already shuffle)",
                  same(s2, s1),
                  {"S1": s1["samples"], "S2": s2["samples"],
                   "S2.setShuffleChanged": s2["detail"].get("setShuffleChanged")}),
            check("replace-while-shuffle-still-honors-seed",
                  "S4 == S1 (replace in shuffle, then set_mode(shuffle,7))",
                  same(s4, s1),
                  {"S1": s1["samples"], "S4": s4["samples"],
                   "S4.setShuffleChanged": s4["detail"].get("setShuffleChanged")}),
            check("explicit-seed-reports-changed-true", True,
                  s2["detail"].get("setShuffleChanged") is True,
                  {"S2.setShuffleChanged": s2["detail"].get("setShuffleChanged")}),
            check("no-seed-repeat-reports-changed-false", False,
                  s6["detail"].get("setShuffleChanged") is False,
                  {"S6.setShuffleChanged": s6["detail"].get("setShuffleChanged"),
                   "S6.samples": s6["samples"],
                   "note": "changed=false 只说明可见投影未变，不保证走序未重排"}),
            check("every-walk-is-a-permutation", True,
                  all(permutation(record) for record in payload["scenarios"]),
                  [{"label": record["label"], "head": record["samples"][:len(ids)],
                    "permutation": permutation(record)} for record in payload["scenarios"]]),
            check("no-foreign-advance-between-next-calls", True,
                  all(record["indexQuiescent"][0] == record["indexQuiescent"][1]
                      for record in payload["scenarios"]),
                  [{"label": record["label"], "indexQuiescent": record["indexQuiescent"]}
                   for record in payload["scenarios"]]),
        ]
        payload["result"] = "confirmed" if all(
            item["passed"] for item in payload["assertions"]) else "regressed"
        payload["conclusion"] = (
            "QUE-12 已闭环（回归检查通过）：根因是 `PlaybackSession::setMode(v, seed)` 在 v 与当前"
            "模式相同时提前 return，且 `Queue::assign` 在 shuffle 模式下用硬编码 seed 1 重建，"
            "导致调用方的 seed 被忽略；修好后（session.cpp：`value == Shuffle && seed != 0` 时"
            "强制重建）同一 seed 在 5 条不同前序路径下都得到同一走序，且每条都是合法排列、"
            "next 之间没有额外推进。`changed` 契约：显式 seed 的 set_mode 在重建顺序时回 true，"
            "不带 seed 的重复 set_mode 仍回 false（可见投影未变但走序可能重排）。"
            "若本项失败即为回归。"
            if payload["result"] == "confirmed" else
            "回归：同一 seed 在不同前序路径下给出的走序不一致，见 scenarios 原始数据。")
        return payload
    finally:
        if mcp is not None:
            payload["mcpReturncode"] = mcp.close(timeout=20.0)
        if tui is not None and tui.proc is not None:
            payload["tuiReturncode"] = tui.quit(args.quit_timeout)
            tui.stop_handles()
        time.sleep(0.3)
        payload["ownedPipesAfter"] = owned_profile_pipes(profile_pipe_suffix(
            Path(args.data_dir).resolve()))


def main(argv=None) -> int:
    m4.configure_stdout()
    args = build_parser().parse_args(argv)
    out = Path(args.forensics_out if args.shuffle_forensics else args.out).resolve()
    profile = Path(args.data_dir).resolve()
    build_root = (ROOT / "build").resolve()
    inside_build = profile == build_root or str(profile).lower().startswith(
        str(build_root).lower() + os.sep)
    if not inside_build:
        print("error: --data-dir must live under %s (got %s); refusing to isolate or clean a user "
              "data directory" % (build_root, profile))
        return 2
    profile.mkdir(parents=True, exist_ok=True)
    os.environ["MORA_DATA_DIR"] = str(profile)
    payload = {
        "script": "scripts/m8_queue_check.py",
        "result": "failed",
        "summary": {"passed": 0, "failed": 0, "environmentFailures": 1,
                    "environmentFailureReasons": ["harness could not create a console"]},
        "steps": [],
        "limitations": [],
    }
    try:
        console = m4.TestConsole(args.columns, args.rows)
    except m4.ConsoleUnavailable as exc:
        payload["summary"]["environmentFailureReasons"] = ["隐藏测试控制台不可用: %s" % exc]
        write_payload(payload, out)
        print_summary(payload)
        return 3
    try:
        payload = run_shuffle_forensics(args, console) if args.shuffle_forensics \
            else execute(args, console)
    finally:
        console.close()
    if not args.keep_profile:
        try:
            shutil.rmtree(profile)
            payload["profileRemoved"] = True
        except OSError as exc:
            payload["profileRemoved"] = False
            payload["profileRemoveError"] = str(exc)
    else:
        payload["profileRemoved"] = False
    write_payload(payload, out)
    if args.shuffle_forensics:
        print_shuffle_forensics(payload)
    else:
        print_summary(payload)
    if args.shuffle_forensics:
        return 0 if payload.get("result") == "confirmed" else (
            3 if payload.get("result") == "environment-failed" else 1)
    return exit_code(payload, args.allow_environment_failures)


def print_shuffle_forensics(payload: dict) -> None:
    print("MORA M8 shuffle forensics: %s" % payload.get("result"))
    for scenario in payload.get("scenarios", []):
        print("  %-26s modeAfter=%-10s samples=%s changed=%s" % (
            scenario["label"], scenario.get("modeAfter"), scenario.get("samples"),
            scenario.get("detail", {}).get("setShuffleChanged")))
    for assertion in payload.get("assertions", []):
        print("  [%s] %s" % ("PASS" if assertion["passed"] else "FAIL", assertion["name"]))
        if not assertion["passed"]:
            print("        observed=%s" % (assertion["observed"],))
    print("  conclusion: %s" % payload.get("conclusion", ""))
    print("REPORT_JSON " + json.dumps(payload, ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    sys.exit(main())
