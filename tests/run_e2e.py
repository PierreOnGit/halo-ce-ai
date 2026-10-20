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
debug.chasm_react narrates an event she reacts to; the game's log must show
both answers as subtitles. With --maps too (the retail Xbox maps, extracted
from your disc), that is played on a30 among its marines, and then network
co-op: a second game joins the first, talks to the AI, and must get its
words and the answer back from the host.

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


def stage_folder(runner, program, folder: Path, maps: Path = None) -> None:
    """a game's folder: OpenCE's StageKit map (or, with maps, the retail maps), the profile"""
    folder.mkdir()
    if maps:
        (folder / "maps").symlink_to(maps.resolve(), target_is_directory=True)
    else:
        runner.generate(program, "elevator", folder)
        (folder / "maps").mkdir(exist_ok=True)
        (folder / "maps" / "ui.map").write_bytes(
            runner.struct.pack("<II", runner.MAP_HEADER_SIGNATURE, runner.XBOX_MAP_VERSION))
    shutil.copytree(REPOSITORY / "chasm-profile", folder / "chasm-profile")


def wait_for_log(game, log: Path, needles, seconds: float) -> str:
    """the game's log once it has one of the needles (or the time is up)"""
    deadline = time.time() + seconds
    while time.time() < deadline and game.poll() is None:
        text = log.read_text(errors="replace") if log.exists() else ""
        if any(needle in text for needle in needles):
            time.sleep(1.5)
            break
        time.sleep(0.25)
    return log.read_text(errors="replace") if log.exists() else ""


def show(text: str) -> None:
    lines = [line for line in text.splitlines() if "chasm" in line or "fault" in line]
    print("   " + "\n   ".join(lines[-14:]))


def crashed(text: str) -> bool:
    return "segmentation fault" in text or "exception" in text.lower()


def play_in_game(opence: Path, environment: dict, scratch: Path, maps: Path = None) -> list:
    """the game alone, saying debug.chasm_say's words and narrating debug.chasm_react's event: on a StageKit map
    (no one near: Cortana answers both), or with maps on The Silent Cartographer... a30, Halo, among its marines"""
    sys.path.insert(0, str(opence / "tools" / "stagekit" / "tests"))
    import runner  # (OpenCE's tools/stagekit/tests/runner.py)

    (scratch / "stagekit").mkdir()
    program = runner.build(scratch / "stagekit")
    folder = scratch / "game"
    stage_folder(runner, program, folder, maps)
    os.environ["CHASM_BRIDGE_ROOT"] = environment["CHASM_BRIDGE_ROOT"]
    level = "levels\\a30\\a30" if maps else "custom_maps\\void"
    game = runner.launch(folder, "bot:1", "127.10.9.10", "127.10.9.11", "log",
                         init=f"map_name {level}\n", HALO_FIXED_STEP="1",
                         HALO_CHASM_URL="http://127.0.0.1:7341", HALO_CHASM_SAY="Johnson, follow me.",
                         HALO_CHASM_REACT="Covenant dropships are coming in over the ridge.",
                         HALO_CHASM_MICROPHONE_WARM="false", HALO_CHASM_IDLE_CHATTER="0")
    try:
        text = wait_for_log(game, folder / "game.log", ["chasm: subtitle: chasm:"], 1)
        # (the words' answer, then the reaction's)
        deadline = time.time() + 90
        while time.time() < deadline and game.poll() is None and text.count("chasm: subtitle: ") < 3:
            time.sleep(0.5)
            text = (folder / "game.log").read_text(errors="replace")
        time.sleep(1.5)
        text = (folder / "game.log").read_text(errors="replace")
    finally:
        game.kill()
        game.wait()
    show(text)
    failures = []
    if crashed(text):
        failures.append("the game crashed")
    if "chasm: subtitle: You: Johnson, follow me." not in text:
        failures.append("the game did not say debug.chasm_say's words")
    if " reacts to: World: Covenant dropships" not in text:
        failures.append("the game did not give debug.chasm_react's reaction")
    answers = [line for line in text.splitlines() if "chasm: subtitle: " in line and "subtitle: You:" not in line]
    if len(answers) < 2:
        failures.append(f"the game showed {len(answers)} answer(s), not the words' and the reaction's")
    if maps and "Master Chief to Cortana" in text:
        failures.append("on a30 Cortana answered: no marine was found near the player")
    return failures


def play_coop(opence: Path, environment: dict, scratch: Path, maps: Path) -> list:
    """network co-op on a30 (the retail maps: hosting needs the menus' ui.map): a joiner holds talk to AI (its
    microphone debug.voice_test's tone; the fake recognition hears "Johnson, follow me."), the host recognizes
    it, runs the turn and relays the voice, and the joiner shows its own words and the answer"""
    sys.path.insert(0, str(opence / "tools" / "stagekit" / "tests"))
    import runner

    games = []
    for name in ("host", "join"):
        stage_folder(runner, None, scratch / name, maps)
    os.environ["CHASM_BRIDGE_ROOT"] = environment["CHASM_BRIDGE_ROOT"]
    common = dict(HALO_CHASM_MICROPHONE_WARM="false", HALO_CHASM_IDLE_CHATTER="0", HALO_CHASM_REACTIONS="false")
    games.append(runner.launch(scratch / "host", "bot:1", "127.10.21.10", "127.10.21.255", "host:levels\\a30\\a30:coop",
                               HALO_NETWORK_TEST_START="5", HALO_CHASM_URL="http://127.0.0.1:7341", **common))
    time.sleep(3)
    games.append(runner.launch(scratch / "join", "bot:1", "127.10.21.11", "127.10.21.255", "join",
                               HALO_CHASM_SAY="(spoken)", HALO_VOICE_TEST="1", **common))
    try:
        joiner = wait_for_log(games[1], scratch / "join" / "game.log",
                              ["chasm: subtitle: Sergeant", "chasm: subtitle: Cortana:", "chasm: subtitle: chasm:"], 180)
    finally:
        for game in games:
            game.kill()
            game.wait()
    host = (scratch / "host" / "game.log").read_text(errors="replace")
    print("   host:")
    show(host)
    print("   joiner:")
    show(joiner)
    failures = []
    if crashed(host) or crashed(joiner):
        failures.append("co-op: a game crashed")
    if "chasm: a recording of" not in host:
        failures.append("co-op: the host did not get the joiner's recording")
    if "chasm: subtitle: You: Johnson, follow me." not in joiner:
        failures.append("co-op: the joiner did not see its words come back")
    if not [line for line in joiner.splitlines() if "chasm: subtitle: " in line and "You:" not in line]:
        failures.append("co-op: the joiner showed no answer")
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
    parser.add_argument("--maps", help="with --opence: the retail Xbox maps folder, for a30 alone and in co-op")
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
            maps = Path(arguments.maps) if arguments.maps else None
            print("== in the game: \"Johnson, follow me.\" and a reaction")
            failures += play_in_game(Path(arguments.opence).resolve(), environment, scratch, maps)
            if maps:
                print("== in co-op: the joiner talks to the AI")
                failures += play_coop(Path(arguments.opence).resolve(), environment, scratch, maps)
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
