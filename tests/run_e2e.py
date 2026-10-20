#!/usr/bin/env python3
"""End to end, without the game or a GPU: OpenCE's chasm client
(opence/port/linux/src/chasm_client.c, run by tests/client_harness) against a
real chasm backend whose models are tests/fake_providers.py.

What it checks, for each spoken order below:
  - chasm imports and activates the profile the client staged (chasm-profile/)
  - the recording reaches speech recognition, and what was heard comes back
  - the character's turn streams back subtitles with voice, the action chosen
    (each of the Halo action book's that the bridge does), and the end
  - the LLM was given the character's card and the Halo lore

With --opence (an OpenCE checkout the bridge was applied to and built:
tools/apply_to_opence.py, then configure.py and ninja linux), the game itself
plays too: a StageKit map (OpenCE's tools/stagekit, no game data), on which
debug.chasm_say speaks to whoever is near - no one, so Cortana answers - and
the game's log must show her reply as subtitles.

Needs a chasm binary (cargo build -p chasm in a chasm checkout) and, for it,
ONNX Runtime's library (LD_LIBRARY_PATH), and the harness built for 32-bit
with SDL3 (tests/client_harness/build.sh).

    python tests/run_e2e.py --chasm /path/to/chasm/target/debug/chasm --harness /path/to/harness [--opence OpenCE]
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
PROVIDER_PORT = 5099

# (what the player says to whom, the action the turn must give back)
ORDERS = [
    ("Sergeant Johnson", "Johnson, follow me.", "movement.follow_target"),
    ("Bisenti", "Bisenti, hold this position.", "ai.wait_here"),
    ("Mendoza", "Mendoza, get in the Warthog.", "halo.enter_vehicle"),
    ("Chips Dubbo", "Chips, charge them!", "halo.berserk"),
    ("Jenkins", "Jenkins, give me your weapon.", "world.give_item"),
    ("Grunt", "Stand down and you live.", "combat.stop"),
]


def members(speaker: str, words: str) -> str:
    nearby = [
        {"npc_key": speaker, "npc_name": speaker, "characterId": speaker, "distance_m": 3.2,
         "under_crosshair": True, "hostile": speaker == "Grunt"},
        {"npc_key": "Fitzgerald", "npc_name": "Fitzgerald", "characterId": "Fitzgerald", "distance_m": 7.5,
         "under_crosshair": False, "hostile": False},
    ]
    state = {"player_name": "Master Chief", "major_location": "Halo", "mission": "Halo", "map": "a30",
             "health": "100%", "shield": "80%", "weapon": "assault rifle", "difficulty": "Heroic", "in_combat": "no"}
    body = {
        "request_id": f"e2e_{int(time.time() * 1000)}",
        "npc_key": speaker,
        "npc_name": speaker,
        "player_text": words,
        "want_tts": True,
        "location": {"cell": "a30", "worldspace": "Halo", "region": "Halo", "major": "Halo", "minor": "a30"},
        "metadata": {
            "targeting": {"nearby_npcs": nearby},
            "combat_with": [],
            "in_combat": False,
            "npc_state": {"teammate": True, "in_combat": False},
            "macros": state,
            "gamestate": state,
        },
    }
    return json.dumps(body)[1:-1]


def play_in_game(opence: Path, environment: dict, scratch: Path) -> list:
    """the game on a StageKit map, saying debug.chasm_say's words; its failures"""
    sys.path.insert(0, str(opence / "tools" / "stagekit" / "tests"))
    import runner  # (OpenCE's tools/stagekit/tests/runner.py)

    folder = scratch / "game"
    folder.mkdir()
    (scratch / "stagekit").mkdir()
    program = runner.build(scratch / "stagekit")
    runner.generate(program, "elevator", folder)
    shutil.copytree(REPOSITORY / "chasm-profile", folder / "chasm-profile")
    (folder / "maps").mkdir(exist_ok=True)
    (folder / "maps" / "ui.map").write_bytes(
        runner.struct.pack("<II", runner.MAP_HEADER_SIGNATURE, runner.XBOX_MAP_VERSION))
    os.environ["CHASM_BRIDGE_ROOT"] = environment["CHASM_BRIDGE_ROOT"]
    game = runner.launch(folder, "bot:1", "127.10.9.10", "127.10.9.11", "log",
                         init="map_name custom_maps\\void\n", HALO_FIXED_STEP="1",
                         HALO_CHASM_URL="http://127.0.0.1:7341", HALO_CHASM_SAY="Cortana, where are we?",
                         HALO_CHASM_MICROPHONE_WARM="false")
    log = folder / "game.log"
    deadline = time.time() + 60
    try:
        while time.time() < deadline and game.poll() is None:
            text = log.read_text(errors="replace") if log.exists() else ""
            if "chasm: subtitle: Cortana:" in text or "chasm: subtitle: chasm:" in text:
                time.sleep(1)
                break
            time.sleep(0.25)
    finally:
        game.kill()
        game.wait()
    text = log.read_text(errors="replace") if log.exists() else ""
    lines = [line for line in text.splitlines() if "chasm" in line]
    print("   " + "\n   ".join(lines[-12:]))
    failures = []
    if "segmentation fault" in text or "exception" in text.lower():
        failures.append("the game crashed")
    if "chasm: subtitle: You: Cortana, where are we?" not in text:
        failures.append("the game did not say debug.chasm_say's words")
    if "chasm: subtitle: Cortana:" not in text:
        failures.append("Cortana's reply was not shown in the game")
    return failures


def wait_for(url: str, seconds: float) -> None:
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            urllib.request.urlopen(url, timeout=1).read()
            return
        except OSError:
            time.sleep(0.25)
    raise SystemExit(f"nothing answered at {url}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--chasm", required=True, help="the chasm binary")
    parser.add_argument("--harness", required=True, help="tests/client_harness's binary")
    parser.add_argument("--opence", help="an OpenCE checkout with the bridge, built (ninja linux)")
    parser.add_argument("--keep", action="store_true", help="keep the scratch folder")
    arguments = parser.parse_args()

    scratch = Path(tempfile.mkdtemp(prefix="halo-chasm-e2e-"))
    for name in ("data", "profiles", "voices", "engines", "bridge"):
        (scratch / name).mkdir()
    base = f"http://127.0.0.1:{PROVIDER_PORT}/v1"
    settings = {
        "profile": "",
        "llm": {"provider": "openai_compat", "api": {"openai_compat": {"api_key": "test", "model": "fake", "base_url": base}}},
        "stt": {"provider": "openai", "api": {"openai": {"api_key": "test", "model": "whisper-1", "base_url": base}}},
        "tts": {"provider": "elevenlabs", "api": {"elevenlabs": {"api_key": "test", "model": "eleven_flash",
                                                              "base_url": base, "voice": "fakevoice"}}},
        "launcher": {"helper_config": str(scratch / "bridge" / "nvbridge.config.json")},
    }
    (scratch / "settings.json").write_text(json.dumps(settings, indent=1))
    environment = dict(os.environ,
                       CHASM_FNV_BRIDGE="1",
                       CHASM_DATA_ROOT=str(scratch / "data"),
                       CHASM_SETTINGS=str(scratch / "settings.json"),
                       CHASM_PROFILES_DIR=str(scratch / "profiles"),
                       CHASM_VOICES_DIR=str(scratch / "voices"),
                       CHASM_ENGINES_DIR=str(scratch / "engines"),
                       CHASM_BRIDGE_ROOT=str(scratch / "bridge"),
                       RUST_LOG="info")
    log = scratch / "providers.jsonl"
    processes = [
        subprocess.Popen([sys.executable, str(REPOSITORY / "tests" / "fake_providers.py"), "--port", str(PROVIDER_PORT),
                          "--log", str(log)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([arguments.chasm], env=environment, cwd=scratch,
                         stdout=open(scratch / "chasm.out", "w"), stderr=subprocess.STDOUT),
    ]
    failures = []
    try:
        wait_for(f"http://127.0.0.1:{PROVIDER_PORT}/v1/models", 15)
        wait_for("http://127.0.0.1:7341/health", 60)
        for speaker, words, action in ORDERS:
            print(f"== {speaker}: \"{words}\" (expecting {action})")
            run = subprocess.run([arguments.harness, str(REPOSITORY), members(speaker, words)],
                                 env=dict(environment, CHASM_URL="http://127.0.0.1:7341"),
                                 capture_output=True, text=True, timeout=300)
            summary = run.stdout.strip().splitlines()[-1] if run.stdout.strip() else "(no output)"
            print("   " + summary)
            if run.returncode != 0 or f"({action})" not in summary:
                failures.append(f"{speaker}: {summary}")
                print(run.stdout[-3000:])
                print(run.stderr[-3000:])
        if arguments.opence:
            print("== in the game: \"Cortana, where are we?\"")
            failures += play_in_game(Path(arguments.opence).resolve(), environment, scratch)
        # what chasm made of it
        imported = (scratch / "profiles" / "halo-combat-evolved" / "profile.json").exists()
        active = json.loads((scratch / "settings.json").read_text()).get("profile")
        print(f"== profile imported: {imported}, active: {active!r}")
        if not imported or active != "halo-combat-evolved":
            failures.append("the profile was not imported and activated")
        prompts = [json.loads(line) for line in log.read_text().splitlines()]
        chats = [p for p in prompts if p["path"].endswith("/chat/completions")]
        first = json.dumps(chats[0]["body"]["messages"]) if chats else ""
        for needed in ("Staff Sergeant Avery Johnson", "Pillar of Autumn", "Activated lore"):
            ok = needed in first
            print(f"== the first prompt has {needed!r}: {ok}")
            if not ok:
                failures.append(f"the prompt lacks {needed!r}")
    finally:
        for process in processes:
            process.send_signal(signal.SIGINT)
        for process in processes:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
        if arguments.keep or failures:
            print(f"scratch: {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
