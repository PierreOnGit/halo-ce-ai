# Setting it up: what to download, which models, which settings

This is the checklist for a Windows laptop. The **host** is the machine that runs chasm and the AI
models; it can be the only machine. A **friend** joining in co-op needs only the game (step A).

## A. Everyone who plays (host and friend)

| What | Where | Size | Notes |
| --- | --- | --- | --- |
| Your Halo: Combat Evolved **Xbox disc image** (`.iso` or `.xiso`) | Your own disc, ripped | ~2–4 GB | Any version works. OpenCE extracts `maps/` from it at first start. The PC or MCC versions are not usable. |
| **The game with the bridge** (`halo-ce-ai-windows`) | This repository's GitHub Actions run, under Artifacts | ~40 MB zipped | It includes `chasm-profile/`. Unzip it, then put the disc image next to `halo.exe`. Both players need **the same build**: network version 33. |
| A headset with a microphone | | | Headphones keep the characters' voices out of your mic. |
| An up-to-date GPU driver | NVIDIA, AMD or Intel | | The game needs OpenGL 4.5. |

To build the game yourself instead of downloading it, see "Building it yourself" at the end.

## B. The host only: chasm and its models

1. **chasm.** Install it from [github.com/chasmlol/chasm/releases](https://github.com/chasmlol/chasm/releases)
   (`chasm_<version>_x64-setup.exe`, Windows only). It's the backend from the video.
2. **Settings → Runtimes**: install the local engines with one click each: llama.cpp, the TTS engine
   and Parakeet. They download their own Python environments, a few GB in all.
3. **The models.** chasm opens each model's download page and shows you the folder to put it in.
   Choose by your laptop's GPU memory (VRAM):

| Your GPU | LLM (Settings → LLM) | Speech to text (STT) | Voices (TTS) | Total VRAM |
| --- | --- | --- | --- | --- |
| **No NVIDIA GPU / under 6 GB** | a hosted API: OpenRouter with a fast small model, or OpenAI | Groq (Whisper), hosted | PocketTTS (CPU) or ElevenLabs/Cartesia, hosted | ~0 |
| **6 GB** (RTX 3060 laptop, 4050) | **Gemma 4 E2B** (`unsloth/gemma-4-E2B-it-GGUF`, ~3 GB) | **Parakeet TDT 0.6B v3** (`nvidia/parakeet-tdt-0.6b-v3`, ~2.4 GB) | **PocketTTS** (CPU) | ~5.5 GB |
| **8 GB** (RTX 4060/4070 laptop) | **Gemma 4 E4B** (`unsloth/gemma-4-E4B-it-GGUF`, ~5 GB) | Parakeet TDT 0.6B v3 | PocketTTS, or faster-qwen3-tts if it fits | ~7.5 GB |
| **12 GB+** (RTX 4080 laptop, 3080 Ti) | **Gemma 4 12B** (`unsloth/gemma-4-12b-it-GGUF`, ~7 GB) | Parakeet TDT 0.6B v3 | **faster-qwen3-tts**, the best at cloning voices | ~11–12 GB |
| **16 GB+ desktop** | Gemma 4 26B-A4B (~15 GB) | Parakeet | faster-qwen3-tts | 20 GB+ |

   - The game itself takes under 1 GB of VRAM.
   - Gemma 4 and Parakeet are the "google/ge…" and "nvidia/pa…" in the video's description.
   - "BreezeBlu…" is Breeze TTS 2. It's excellent but needs a 12 GB GPU and is non-commercial; chasm's
     own engines (faster-qwen3-tts, PocketTTS) do the same job.
   - Smaller LLMs answer faster but follow instructions less well. E4B is the sweet spot on a laptop.
4. **Settings → Retrieval**: place the embedder, **Small (BGE-small, INT8 on CPU)**. **Characters can't
   act without it**: chasm only offers them actions through a search that runs on it.
5. **Settings → Bridge → Helper config path**: `%LOCALAPPDATA%\chasm\bridge\nvbridge.config.json`.
   The game writes that file the first time it runs with chasm. Without this setting, chasm uses its
   built-in Fallout action book instead of Halo's.
6. Leave chasm running. Start the game, then start a campaign mission. chasm shows **Connected**,
   imports the Halo profile, makes it active and starts its models.

**Disk space for the host:** about 20–30 GB: chasm's engines (~8 GB), the models (5–20 GB) and the
game's maps (~1.5 GB).

### Voices (optional, but it's what makes it)

chasm clones a character's voice from `profiles/halo-combat-evolved/voices/<Name>/reference.wav`. Give
each character 10–20 seconds of clean speech from your own copy of the game: Sergeant Johnson,
Cortana, Captain Keyes, 343 Guilty Spark, Bisenti, Mendoza, Jenkins, Fitzgerald, Chips Dubbo, a
Grunt and an Elite. Without a clip, they speak in the TTS engine's default voice.

## C. Playing together

1. The host starts a **campaign co-op** game: Map → a campaign level → co-op, on the LAN or as an
   internet game through OpenCE's invite link or server browser.
2. The friend joins with the same bridge build. **The friend doesn't need chasm.** Their voice goes to
   the host over the game's own connection, and the characters' voices come back the same way.
3. Keys:
   - **B** (`controls.talk_to_ai`): hold it and talk to the AI, for everyone.
   - **V**: still voice chat between the two of you.
   - When playing alone, V talks to the AI too.

   The character who answers is picked around **whoever spoke**. Both players hear the voice from
   where the character stands, and both see the subtitles: "You:" for your own words, your friend's
   name for theirs.

## D. Settings worth knowing (`config.toml`, next to `halo.exe`)

| Setting | Default | What it's for |
| --- | --- | --- |
| `controls.talk_to_ai` | `"B"` | The talk-to-AI key. |
| `chasm.url` | `"http://127.0.0.1:7341"` | Where chasm runs. It can be another machine on your network. |
| `chasm.distance` | `12.0` | How near, in meters, a character must be to hear you. |
| `chasm.reactions` | `true` | Characters speak up on their own: a friend killed, Covenant contact, the fight over, shields down, a vehicle, a long quiet. |
| `chasm.reaction_cooldown` | `20.0` | The fewest seconds between two of their own remarks. Raise it if they're chatty. |
| `chasm.idle_chatter` | `150.0` | Seconds of quiet before someone may say something. `0` turns it off. |
| `chasm.microphone_warm` | `true` | Keeps the mic open so your first word isn't cut off. Audio is only recorded while the key is held. |

## E. Checking it works without the disc in the way

On the host, with chasm set up, you can run the repository's end-to-end test against your real chasm
(Python 3 and the built game are needed). It speaks for you, checks the answer and the reaction, and,
given your extracted `maps/` folder, plays a30 among its marines and a two-instance co-op game. See
`tests/run_e2e.py`.

## Building it yourself (instead of downloading)

You need Git, Python 3.12+, LLVM 18+ (clang and lld) on the PATH, ninja, and Visual Studio 2022 Build
Tools with "Desktop development with C++" (it provides the x86 libraries and the Windows SDK). Then:

```
python tools/apply_to_opence.py
cd OpenCE-build
python configure.py --release
ninja windows
```

The result is `build/windows/halo.exe` and `SDL3.dll`. Put `chasm-profile/` next to them.
