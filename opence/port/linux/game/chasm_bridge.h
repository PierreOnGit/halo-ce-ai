/*
CHASM_BRIDGE.H

The game's AI characters (chasm_bridge.c): the campaign's marines, crew and
Covenant talked to with the players' own voices, alone or in co-op,
answering in theirs, through a chasm backend (port/linux/src/chasm_client.c).
What the main loop, the HUD, the netcode and the saved game ask of it.
*/

#ifndef __CHASM_BRIDGE_H
#define __CHASM_BRIDGE_H
#pragma once

/* ---------- prototypes/CHASM_BRIDGE.C */

/* every frame (main.c): push to talk, who is spoken to, the answers */
void chasm_bridge_update(void);
/* (hud.c, in the HUD of each local player's view) the subtitles, and what
the bridge is doing (listening, thinking) */
void chasm_bridge_draw(short local_player_index);
/* (network_distributed.c) whether a message is the bridge's (co-op: a
client's recording to the host, the host's lines and voices to the clients),
and its handling (machine_index: the sender's on the host, NONE on a client) */
boolean chasm_bridge_handles_message(word const *message, word size);
void chasm_bridge_handle_message(long machine_index, word const *message, word size);
/* (game_state.c) a checkpoint saved or gone back to ("checkpoint saved",
"checkpoint revert", "map start"): chasm's memories saved or put back with it */
void chasm_bridge_game_state_event(char const *event);

#endif // __CHASM_BRIDGE_H
