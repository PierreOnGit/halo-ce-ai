# Plan

## Where it stands

The single-player bridge works. You hold push to talk and speak. chasm transcribes it, then the
character you named, the one under your crosshair, or the nearest one answers. They answer in voice,
from where they stand, with subtitles, and can act: follow, hold, advance, charge, get in or out of a
vehicle, hand over a weapon, turn hostile or stand down. Checkpoints roll chasm's memories back with
the game. It is tested against a real chasm backend and inside the real engine (`tests/run_e2e.py`).

What it lacked:

1. **Multiplayer.** In network games the bridge is off, and V belongs to voice chat.
2. **Initiative.** Characters only speak when spoken to.

## 1. Co-op: a friend talks to the AI too

OpenCE already plays the campaign in co-op over the network (LAN, or the internet through invite
links). The host runs the map's scripts and AI; clients follow. The bridge follows the same split.

```
 friend (client)                          host                                chasm (host's PC only)
 hold B, speak ──► Opus frames ─────────► collects them per machine
                   (reliable message)     decodes to 16 kHz ─────────────────► /speech/recognize
                                          what was heard ◄──────────────────── text
 "<friend>: ..."  ◄── line message ────── shows it, picks the speaker
                                          around the FRIEND's unit
                                          turn ──────────────────────────────► /api/game/v1/turn
 plays the voice  ◄── voice message ───── voices ◄──────────────────────────── WAV chunks + action
 from where the       (Opus, caption,     plays them, re-encodes for clients,
 speaker stands,       speaker's place)    gives the order to the squad
 subtitle as it starts
```

- **Only the host needs chasm and the models.** The friend needs only the bridge build and their
  disc image. Nothing is exposed to the internet: everything goes over the game's own connection.
- **A key of its own:** `controls.talk_to_ai` (default **B**) talks to the AI everywhere. In single
  player, V (push to talk) still works too. In network games V stays voice chat's, so friends still
  hear each other.
- **Messages** (three new distributed message types, so `HALO_PORT_NETWORK_VERSION` 32 → 33; bridge
  builds play only with bridge builds):
  - `chasm_audio_up` (client → host, reliable): an utterance's Opus frames, a quarter-second at a
    time, with the last one marked.
  - `chasm_line_down` (host → clients, reliable): what a player said, a notice, or "stop" (someone
    spoke over the character).
  - `chasm_voice_down` (host → clients, reliable): a character's voice as Opus frames, with its
    subtitle (on a line's first message), who speaks, and where they stand.
- One conversation at a time. Whoever speaks last takes over, as in single player. The speaker is
  chosen around the player who spoke. Subtitles say who: "You" on your own machine, the player's
  name on the others.

## 2. Initiative: characters speak up on their own

chasm can run unprompted reaction turns: an empty player text and a `reaction` narration of what just
happened. The host watches the game every half second and narrates events to the nearest friendly
witness, at most once per `chasm.reaction_cooldown` seconds (and never over a conversation):

| Event | Narration (to the witness) |
| --- | --- |
| A friendly character dies near a player | "Bisenti was just killed, right beside you." |
| Covenant contact after a quiet spell | "Covenant contact: 3 Grunts and an Elite closing in." |
| The fight around the players is over | "The last of the Covenant nearby has fallen." |
| A player's shields collapse under fire | "The Chief's shields just went down under fire." |
| A player boards a vehicle | "The Chief just climbed into a Warthog." |
| Nothing has been said for `chasm.idle_chatter` seconds, no fight | "A quiet moment between fights." |

Their replies are voiced and subtitled like any line, and relayed to co-op clients. Settings:
`chasm.reactions` (on/off), `chasm.reaction_cooldown` (20 s) and `chasm.idle_chatter` (150 s; 0 is
off).

What stays as it is: the combat AI is Halo's own. The LLM decides what a character says and which of
the allowed orders it gives. The orders are carried out by the game's own AI, as the levels' scripts
give them.

## 2b. A second Spartan: Fred-104

A companion Spartan the players can talk to, built from what the levels already have:

- **Body and mind.** The host creates the players' own biped (the Master Chief's) and gives it a Marine
  actor variant of the level's, with its weapon and grenades, as `actor_place` does. If Marines are near,
  he joins their encounter and squad; otherwise he is a free actor.
- **Keeping up.** Halo's "follow" needs an encounter's firing positions, which end where the encounter does.
  So the bridge keeps a trail of the player's footsteps (on pathfinding ground, every 1.5 m). When every
  player is beyond `chasm.spartan_leash` and nobody looks his way, he is moved onto the newest footstep
  2-18 m from the player and out of sight. There he joins the Marines near him, or goes on his own. He
  comes in the same way: at map start, and after a death at the next checkpoint.
- **Orders** are his own: follow, wait, enter or exit a vehicle (gunner's seat first).
- **Who he is:** `chasm_bridge.c` knows his unit, so speaker picking, reactions and subtitles treat him
  as the character Fred-104 (`tools/build_profile.py`'s card).
- **Not covered by CI:** StageKit maps have no actors and no pathfinding, so he only appears on the real
  maps.

## 3. Tests

- `tests/run_e2e.py` (real chasm, fake models):
  - The client harness covers six orders, reaction turns and the voice relay codec.
  - The single-player game: a StageKit map, `debug.chasm_say`, then Cortana's reply.
  - **Co-op:** two game instances on loopback, one hosting the StageKit map in co-op, one joining.
    The joiner "speaks" (`debug.chasm_say` with `debug.voice_test`'s tone as its microphone). The host
    recognizes it, runs the turn and relays the voice. The joiner shows Cortana's subtitle when the
    relayed voice starts.
  - **Reactions:** `debug.chasm_react` makes the host narrate an event; the reply must show.
- CI runs all of this on every push, with the Linux and Windows builds.

## 4. Later

- Voices from the game: extract each character's lines from the maps for chasm to clone.
- Feeding the game's events to chasm's event log, so its journals and self-made habits (skills) grow
  out of what characters witnessed.
- A Controls Setup row for `controls.talk_to_ai`. For now it is set in `config.toml`.
