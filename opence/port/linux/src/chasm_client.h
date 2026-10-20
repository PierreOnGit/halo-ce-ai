/*
CHASM_CLIENT.H

The platform's side of the AI characters (chasm_client.c): talking to a
chasm backend (https://github.com/chasmlol/chasm) over HTTP on a thread of
its own, the microphone's words recorded for it, the voices it answers with
mixed into the game's output, and the files chasm finds the game by (its
heartbeat, its bridge settings, the profile of characters). Called by
port/linux/game/chasm_bridge.c (the game's side, so its flags are int) and
dsound_sdl.c (the mix).

Nothing here knows the game: the game's side says who is spoken to, in
JSON, and takes back what happened as events.
*/

#ifndef CHASM_CLIENT_H
#define CHASM_CLIENT_H

/* what a turn gives back (chasm_client_next_event) */
enum
{
	_chasm_event_none,
	/* what the player said, as chasm heard it (text) */
	_chasm_event_heard,
	/* a line of the character's, as its voice starts (text) */
	_chasm_event_subtitle,
	/* an action the character chose (action_id, confidence) */
	_chasm_event_action,
	/* the turn is over (text: the whole reply) */
	_chasm_event_done,
	/* the turn failed (text: why) */
	_chasm_event_error,
};

struct chasm_event
{
	int type;
	/* the turn it belongs to (chasm_client_send_turn's) */
	int turn;
	char npc_key[64];
	char action_id[96];
	float confidence;
	char text[1024];
};

/* every frame, on the game's thread: the microphone read while listening,
the heartbeat written, the profile and bridge settings put where chasm
finds them (once); enabled 0 stops it all */
void chasm_client_update(int enabled);
/* whether chasm's address (chasm.url) is usable on this platform */
int chasm_client_available(void);

/* recording the player's words (push to talk): 1 starts, 0 stops; whether
the microphone is open */
int chasm_client_listen(int listen);
/* how much was recorded since listening started, in milliseconds */
int chasm_client_recorded_ms(void);

/* the recording given to chasm's speech recognition: what was heard comes
back as a _chasm_event_heard (or an error). A turn running is abandoned
(its voice stopped). Returns the new turn's number, or 0 if none could start
(nothing recorded) */
int chasm_client_recognize(void);
/* a turn of a character's: members are the request's JSON members, without
the braces ("\"npc_key\":\"Bisenti\",..."); a turn running is abandoned
(its voice stopped). Returns the new turn's number, or 0 if none could
start */
int chasm_client_send_turn(const char *members);
/* a request whose answer is not waited for (save-sync, events): POSTed to
path (below chasm.url) after the turns waiting */
void chasm_client_post(const char *path, const char *body);
/* the turn running abandoned, and its voice stopped */
void chasm_client_cancel(void);
/* whether a turn is running or its voice still plays */
int chasm_client_busy(void);
/* the next event, 1 if there was one */
int chasm_client_next_event(struct chasm_event *event);

/* the voice playing: its loudness now (0 to about 1, for the speaker's
mouth), and where it is heard from (gain 0 to 2, pan -1 left to 1 right) */
float chasm_client_voice_level(void);
void chasm_client_voice_place(float gain, float pan);
/* (dsound_sdl.c, on the audio thread) the voices added to the output
(stereo, 48 kHz) */
void chasm_client_mix(float *output, unsigned long frames);

/* text made safe inside a JSON string (quotes, backslashes, control
characters escaped); the length written, without the NUL */
int chasm_json_escape(const char *text, char *buffer, int size);

#endif
