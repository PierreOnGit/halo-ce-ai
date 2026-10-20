# halo-ce-ai: talk to Halo's marines

Hold a key and speak to the marine you're looking at in *Halo: Combat Evolved*. They answer out loud, in character, with subtitles, and can do what you ask: follow you, hold position, charge, get in the Warthog, hand you their weapon. A Grunt might even defect. If nobody is nearby, Cortana answers from inside your helmet.

This rebuilds the setup from chasm's video [*Giving Video Game NPC's Consciousness*](https://youtu.be/EOfm3h4xYU8). It joins two open-source projects:

- **[OpenCE](https://github.com/OpenCommunityEdition/OpenCE)** is the Halo CE decompilation ported to Windows, Linux, Android and the web. It is the "halo decomp" linked in the video. This repository adds a bridge to it.
- **[chasm](https://github.com/chasmlol/chasm)** is the video author's local backend for AI characters. It handles the language model, speech recognition (Parakeet), voice synthesis, memory and actions. This repository gives it a Halo profile: character cards, lore and an action book.

```
 you speak ──► OpenCE (push to talk, mic) ──► chasm: speech → text
                 │ picks who you're talking to         │
                 │ (the name you said, your crosshair, │
                 │  else the nearest marine, else      ▼
                 │  Cortana)                     LLM + character card + lore
                 ◄── voice, subtitles, an action ◄── TTS + action book
              plays the voice from where the marine stands, moves
              their mouth, shows the subtitle, gives the order
```

## What's in this repository

| Path | What it is |
| --- | --- |
| `opence/port/linux/src/chasm_client.[ch]` | The platform half. It talks HTTP to chasm on its own thread, records push to talk from voice chat's microphone, decodes the voice WAVs and mixes them into the game's sound. It also writes the files chasm uses to find the game: the heartbeat, the profile and the bridge settings. |
| `opence/port/linux/game/chasm_bridge.[ch]` | The game half. It works out who you're talking to, builds the request (who is near, the mission, your health and weapon, the difficulty, whether there's a fight), draws the subtitles, moves the speaker's mouth, positions the voice, gives the chosen order to the speaker's squad, and saves and restores chasm's memories with checkpoints. |
| `opence/patches/opence-chasm-bridge.patch` | Small hooks in OpenCE's own files: the main loop, the HUD, checkpoints, the sound mix and the new `chasm.*` settings. |
| `opence/OPENCE_COMMIT` | The OpenCE commit the patch is made against. |
| `chasm-profile/halo-combat-evolved/` | The chasm profile: 15 character cards (Cortana, 343 Guilty Spark, Sergeant Johnson, Captain Keyes, Bisenti, Mendoza, Jenkins, Fitzgerald, Chips Dubbo, Marine, Crewman, Grunt, Elite, Jackal, Hunter), a lorebook and the action book. |
| `tools/build_profile.py` | Generates `chasm-profile/` from the data in the script. Edit it there, bump `BUNDLE_VERSION`, and run it again. |
| `tools/apply_to_opence.py` | Copies the bridge into an OpenCE checkout and applies the patch. |
| `tests/` | The end-to-end tests (see Testing). |

## Setting it up

You need four things: a build of OpenCE with the bridge, your own Halo CE Xbox disc image (as OpenCE always requires), chasm, and a microphone.

### 1. Build the game with the bridge

The GitHub Actions workflow builds Linux and Windows with the bridge applied. Download `halo-ce-ai-windows` or `halo-ce-ai-linux` from a run's artifacts. Each one already contains `chasm-profile/` next to the executable.

To build it yourself:

```sh
python tools/apply_to_opence.py            # clones OpenCE at the pinned commit into ./OpenCE, applies the bridge
cd OpenCE
python configure.py --release              # then follow OpenCE's README for your platform's tools
ninja                                      # build/linux/halo or build/windows/halo.exe
```

Put `chasm-profile/` in the game's data folder, the one that holds `maps/` (next to the executable on the desktop). `apply_to_opence.py --copy-profile <data folder>` does it for you. Then give the game your disc image as OpenCE's README describes.

Bridge builds don't look for OpenCE updates (`update.auto = false`). OpenCE's own releases don't include the bridge, so updating to one would remove it.

### 2. Install and set up chasm

Install chasm from its [releases](https://github.com/chasmlol/chasm/releases). It's a Windows app. Then, in its control panel:

1. **Settings → LLM, TTS, STT**: download or place a model for each, or point each at a hosted API. Nothing is preselected. These are the "models used" in the video's description.
2. **Settings → Retrieval**: place the embedder. **Characters can't act without it**: chasm only offers them an action through its `find_action` search, and that search runs on the embedder. Without it they still talk, but they don't act.
3. **Settings → Bridge → Helper config path**: set it to `%LOCALAPPDATA%\chasm\bridge\nvbridge.config.json`. The game writes that file the first time it runs. Without this setting, chasm uses its built-in Fallout: New Vegas action book instead of Halo's.

Leave chasm running while you play. When the game starts, chasm shows **Connected**, imports the Halo profile, makes it active and starts its models.

### 3. Play

Start a campaign mission. Look at a marine, **hold V** (`controls.push_to_talk`, the voice chat key), say something and let go.

- **"Johnson, follow me."** A name you say picks who answers, among the characters near you. Otherwise the one under your crosshair answers, otherwise the nearest friendly. Cortana answers when nobody is near, or when you say her name.
- The speaker turns to face you. Their voice comes from where they stand and their mouth moves with it. Subtitles appear above the motion tracker: your words in blue, theirs in white.
- Orders apply to the speaker's squad, the way the levels' own scripts give orders:

| You say something like | The action | In the game |
| --- | --- | --- |
| "follow me", "on me", "let's go" | `movement.follow_target` | the squad follows you |
| "stop following", "stay back" | `movement.stop_follow_target` | the squad stops following |
| "hold this position", "stay here" | `ai.wait_here` | the squad stops following and defends where it is |
| "move up", "take them out" | `halo.advance` | the squad attacks and maneuvers |
| "charge!" | `halo.berserk` | the squad charges recklessly |
| "get in the Warthog", "mount up" | `halo.enter_vehicle` | the squad boards the vehicle nearest you |
| "everybody out" | `halo.exit_vehicle` | the squad gets out of its vehicle |
| "give me your weapon" | `world.give_item` | the speaker drops their weapon for you |
| (betrayal, provocation) | `combat.start` | the speaker turns on you |
| "stand down", "truce" (to a Covenant soldier) | `combat.stop` | the speaker joins your side |

- Speaking again interrupts whoever is talking. When a character dies, their voice stops.
- Checkpoints are sent to chasm (save-sync). When you die and go back to a checkpoint, the characters forget what happened after it.

### Settings (`config.toml`, next to the executable)

| Setting | Default | What it does |
| --- | --- | --- |
| `chasm.enabled` | `true` | Turns the bridge on in the campaign. It never runs in network games, where push to talk is voice chat's. |
| `chasm.url` | `"http://127.0.0.1:7341"` | Where chasm is (`http://` only). It can be on another machine. |
| `chasm.distance` | `12.0` | How near, in meters, a character must be for you to talk to them. |
| `chasm.microphone_warm` | `true` | Keeps the microphone open during the campaign so the first word isn't cut off. Audio is only recorded while the key is held. |
| `chasm.bridge_folder` | `""` | The folder chasm finds the game by. Empty means chasm's own: `CHASM_BRIDGE_ROOT`, else `%LOCALAPPDATA%\chasm\bridge` (`/tmp/chasm/bridge` on Linux). |
| `debug.chasm_say` | `""` | For tests: these words are said, as if heard, two seconds into each map. |

Each setting has a `HALO_*` environment variable, as OpenCE's settings do: `HALO_CHASM`, `HALO_CHASM_URL`, `HALO_CHASM_DISTANCE`, `HALO_CHASM_MICROPHONE_WARM`, `HALO_CHASM_BRIDGE_FOLDER`, `HALO_CHASM_SAY`.

### Voices

chasm gives each character their own voice by cloning a clip: `voices/<Name>/reference.wav` in chasm's profile folder (`profiles/halo-combat-evolved/voices/Sergeant Johnson/reference.wav`, and so on). The profile doesn't ship clips, because they would be the game's audio. Give each character 10 to 20 seconds of clean speech taken from your own copy of the game. Without a clip, chasm uses the TTS model's default voice.

### Linux, and chasm on another machine

The Linux build works the same way. chasm is a Windows app, though, so it usually runs on another machine: set `chasm.url` to that machine's address. The heartbeat and profile can't reach it over the network, so copy `chasm-profile/halo-combat-evolved` into chasm's bridge folder there yourself. chasm imports it on connect, and the game's turns still work. Android and the web build have no bridge; it compiles there as a stub that does nothing.

### When something goes wrong

The game logs everything the bridge does, with a `chasm:` prefix (stderr, or `halo.log` next to `halo.exe`):

- `chasm is not running at 127.0.0.1:7341`: start chasm, or check `chasm.url`.
- `no character for the unit ... (dialogue ...)`: a unit whose dialogue tag matches no character. Add it to `chasm_characters` in `chasm_bridge.c` and to `CHARACTERS` in `tools/build_profile.py`.
- `<name>'s action <id> not done here`: chasm chose an action the bridge doesn't perform, or the speaker has no squad (an actor without an encounter).
- The characters talk but never act: chasm's retrieval embedder isn't placed, or the helper config path isn't set (see step 2).

## Testing

`tests/run_e2e.py` runs the bridge against a **real chasm backend**, with its models replaced by `tests/fake_providers.py`: an OpenAI-compatible LLM, OpenAI-style speech recognition and ElevenLabs-style TTS. So no GPU or API key is needed:

```sh
tests/client_harness/build.sh /tmp/harness                  # chasm_client.c outside the game (32-bit, SDL3)
python tests/run_e2e.py --chasm <chasm>/target/debug/chasm --harness /tmp/harness \
    [--opence OpenCE]                                         # also the game itself, on a StageKit map
```

It checks the following:

- chasm imports and activates the staged profile when it first sees the heartbeat.
- A push-to-talk recording goes through recognition and comes back as text.
- Six spoken orders each come back as a turn with subtitles, voice audio and the expected action: `movement.follow_target`, `ai.wait_here`, `halo.enter_vehicle`, `halo.berserk`, `world.give_item` and `combat.stop`.
- The LLM's prompt contains the character's card and the Halo lore.
- With `--opence`, the patched game itself plays a map from OpenCE's StageKit (no game data), says `debug.chasm_say`, and shows Cortana's reply as a subtitle once her voice starts in the game's sound mix.

The GitHub Actions workflow runs all of this, plus OpenCE's StageKit suite on the patched game.

**Not covered by tests:** finding the speaker among real AI units, and the squad orders (`chasm_act`). Both need the campaign's maps, and StageKit stages no AI. They use the same `ai_scripting_*` functions the levels' scripts use, and the logs above show what they do.

## Licenses

OpenCE's decompilation is AGPL-3.0 (from 17 October 2026) and chasm is AGPL-3.0. The patch and the files it adds to OpenCE are distributed under OpenCE's terms. Halo: Combat Evolved belongs to Microsoft. Neither this repository nor the builds include any of its data.
