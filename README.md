# halo-ce-ai: talk to Halo's marines

Hold a key and speak to the marine you're looking at in *Halo: Combat Evolved*. They answer out loud, in character, with subtitles, and can do what you ask: follow you, hold position, charge, get in the Warthog, hand you their weapon. A Grunt might even defect. If nobody is nearby, Cortana answers from inside your helmet.

**A second Spartan fights beside you.** Fred-104, the Chief's oldest friend, joins you on every campaign level: Master Chief's armor, a Marine's combat AI, and a voice and memory of his own through chasm. He fights with the Marines around you, keeps up when you move on, holds when told to, jumps into the Warthog's gun when asked, and comes back at the next checkpoint if he falls.

**Play it with a friend.** In network co-op, both of you can talk to the characters. Only the host needs the AI models; your friend just needs the game. The characters also **speak up on their own**: a squadmate killed beside them, Covenant contact, the fight over, your shields down, someone taking the Warthog, a long quiet.

**What to download, which models fit your GPU, and the settings:** see [`docs/SETUP.md`](docs/SETUP.md). The plan behind it is in [`docs/PLAN.md`](docs/PLAN.md).

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
| `opence/port/linux/src/chasm_opus.[ch]` | Opus for co-op: a client's words to the host, and the characters' voices back to the clients. |
| `opence/port/linux/src/chasm_client.[ch]` | The platform half. It talks HTTP to chasm on its own thread, records push to talk from voice chat's microphone, decodes the voice WAVs and mixes them into the game's sound. It also writes the files chasm uses to find the game: the heartbeat, the profile and the bridge settings. |
| `opence/port/linux/game/chasm_bridge.[ch]` | The game half. It works out who you're talking to, builds the request (who is near, the mission, your health and weapon, the difficulty, whether there's a fight), draws the subtitles, moves the speaker's mouth, positions the voice, gives the chosen order to the speaker's squad, and saves and restores chasm's memories with checkpoints. |
| `opence/patches/opence-chasm-bridge.patch` | Small hooks in OpenCE's own files: the main loop, the HUD, checkpoints, the sound mix, three co-op network messages (network version 33), the `controls.talk_to_ai` key and the new settings. |
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
python tools/apply_to_opence.py            # clones OpenCE at the pinned commit into ./OpenCE-build, applies the bridge
cd OpenCE-build
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

Start a campaign mission. Look at a marine, **hold B** (`controls.talk_to_ai`; alone, V works too), say something and let go.

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

### Co-op

Host a campaign co-op game (LAN or internet). Whoever holds **B** (`controls.talk_to_ai`) talks to the characters; V stays voice chat between players.

- Only the host runs chasm. A friend's words travel to the host as Opus frames over the game's own connection; the host recognizes them, picks the character near *that* player, runs the turn and gives the order.
- The voice comes back to every client as Opus, with the subtitle and where the speaker stands, so each player hears it from their own position.
- Bridge builds use network version 33, so they play only with each other.

### The characters' own initiative

The host watches the game twice a second. When something happens that a squadmate would remark on, the nearest friendly witness (or Cortana, for your shields) gets a chasm *reaction turn*: no player words, just a narration of the event. Then they speak. This happens at most once every `chasm.reaction_cooldown` seconds, and never over a conversation:

- a friendly character killed nearby
- Covenant contact after a quiet spell
- the fight being over
- a player's shields collapsing under fire
- a player boarding a vehicle
- `chasm.idle_chatter` seconds of quiet

### Fred-104, the second Spartan

On the host (or alone), Fred-104 comes in a few seconds into each campaign level, on a spot you just walked, out of your sight.

- **His body:** the players' own biped, the Master Chief's armor. **His mind:** a Marine's actor variant from the level, with its weapon and grenades. He joins the squad of the Marines nearest you, so he fights from their positions and goes where the level's scripts send them.
- **Keeping up:** when every player is more than `chasm.spartan_leash` meters from him and he is out of everyone's sight, he catches up on a spot you walked lately, behind you. He also joins the Marines there, or goes on his own if there are none.
- **His orders** are his alone, not his squad's. "Follow me" (or "advance") makes him keep up. "Wait here" (or "stop following") makes him hold where he is. "Get in" puts him in the nearest vehicle's gunner seat, else a passenger seat. "Get out" takes him out. He can also hand you his weapon.
- **If he falls,** he comes back at the next checkpoint. Everyone nearby will remark on it.
- **Levels with no Marines at all** (the Library, for one) have no Marine mind to give him, so he sits those out.
- In co-op he is the host's, and every player sees him: the host's objects are everyone's.
- To give him another name, rename his card in chasm and change `chasm_spartan_character` in `chasm_bridge.c`.

### Settings (`config.toml`, next to the executable)

| Setting | Default | What it does |
| --- | --- | --- |
| `controls.talk_to_ai` | `"B"` | Hold it to talk to the AI, alone or in co-op. When playing alone, `controls.push_to_talk` (V) works too. |
| `chasm.enabled` | `true` | Turns the bridge on in the campaign, alone or in network co-op. |
| `chasm.reactions` | `true` | Characters speak up on their own (see above). |
| `chasm.reaction_cooldown` | `20.0` | The fewest seconds between two of their own remarks. |
| `chasm.idle_chatter` | `150.0` | Seconds of quiet before the nearest friend may say something. `0` is never. |
| `chasm.spartan` | `true` | Fred-104 fights beside you in the campaign (see above). |
| `chasm.spartan_leash` | `18.0` | How far, in meters, Fred may fall behind before he catches up. |
| `chasm.url` | `"http://127.0.0.1:7341"` | Where chasm is (`http://` only). It can be on another machine. |
| `chasm.distance` | `12.0` | How near, in meters, a character must be for you to talk to them. |
| `chasm.microphone_warm` | `true` | Keeps the microphone open during the campaign so the first word isn't cut off. Audio is only recorded while the key is held. |
| `chasm.bridge_folder` | `""` | The folder chasm finds the game by. Empty means chasm's own: `CHASM_BRIDGE_ROOT`, else `%LOCALAPPDATA%\chasm\bridge` (`/tmp/chasm/bridge` on Linux). |
| `debug.chasm_say` | `""` | For tests: these words are said, as if heard, two seconds into each map. On a co-op client, push to talk is held for 1.5 s instead. |
| `debug.chasm_react` | `""` | For tests: this narration is given to the nearest friend (else Cortana) as a reaction, four seconds into each map. |

Each setting has a `HALO_*` environment variable, as OpenCE's settings do: `HALO_CHASM`, `HALO_CHASM_URL`, `HALO_CHASM_DISTANCE`, `HALO_CHASM_MICROPHONE_WARM`, `HALO_CHASM_BRIDGE_FOLDER`, `HALO_CHASM_REACTIONS`, `HALO_CHASM_REACTION_COOLDOWN`, `HALO_CHASM_IDLE_CHATTER`, `HALO_KEY_TALK_TO_AI`, `HALO_CHASM_SAY`, `HALO_CHASM_REACT`.

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
    [--opence OpenCE-build [--maps <your extracted maps/>]]
```

**The client harness** checks the following:

- chasm imports and activates the staged profile.
- A push-to-talk recording comes back as text.
- Six spoken orders each come back with subtitles, voice and the expected action: `movement.follow_target`, `ai.wait_here`, `halo.enter_vehicle`, `halo.berserk`, `world.give_item` and `combat.stop`.
- **Co-op:**
  - a client's recording, encoded as the client sends it, is recognized by the host;
  - the host keeps the character's voice as relays;
  - a client plays those relays with their subtitles.
- **A reaction turn** (no player words, a narration) is answered.
- The LLM's prompt contains the character's card and the Halo lore.

**With `--opence`**, the patched game plays a map from OpenCE's StageKit (no game data). It says `debug.chasm_say` and narrates `debug.chasm_react`, and both answers must show as subtitles once the voice starts in the game's sound mix.

**With `--maps` too** (your own extracted Xbox maps), it plays a30 among its marines, then a **real two-instance co-op game** on a30: the joiner talks to the AI, and must get its words and the answer back from the host. That last test needs the retail maps (hosting needs the menus' `ui.map`), so CI can't run it. Run it on your machine.

CI runs everything that needs no game data on every push, plus OpenCE's StageKit suite on the patched game, and builds Linux and Windows.

## Licenses

OpenCE's decompilation is AGPL-3.0 (from 17 October 2026) and chasm is AGPL-3.0. The patch and the files it adds to OpenCE are distributed under OpenCE's terms. Halo: Combat Evolved belongs to Microsoft. Neither this repository nor the builds include any of its data.
