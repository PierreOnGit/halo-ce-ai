/*
CHASM_BRIDGE.C

The campaign's characters given a mind and a voice by a chasm backend
(https://github.com/chasmlol/chasm; port/linux/src/chasm_client.c talks to
it): the player holds push to talk (controls.push_to_talk) and speaks; what
chasm heard picks who is spoken to - a character named in it ("Johnson,
follow me"), else the one under the crosshair, else the nearest - and that
character answers in its own voice, from where it stands, with its words
as subtitles, and may act on them: follow the player, hold here, charge,
board the vehicle, hand over its weapon, or turn on the player.

Who a character is comes from its unit: the name of its dialogue's tag (the
marines' voices: Bisenti, Mendoza, Johnson...), else its unit's (crewman,
grunt, elite). chasm knows each by a character card of that name, in the
profile chasm-profile/halo-combat-evolved (the repository's profile/, which
chasm_client.c puts where chasm imports it). With no one near to answer,
Cortana does, from the player's own head.

What the turn tells chasm: who is near (their distances, who is under the
crosshair, friend or foe), where (the mission), the player's health and
weapon, the difficulty, and whether there is a fight. A checkpoint saved
or gone back to is told too (save-sync), so the characters' memories go back
with the game.

The characters also speak up on their own (chasm.reactions): the game is
watched for what they would remark on - a friend killed beside them,
Covenant contact, the fight over, a player's shields down, a player taking a
vehicle, a long quiet - and the nearest friendly witness is given it as a
reaction turn (chasm's: no words of the player's, a narration of what
happened), no more often than chasm.reaction_cooldown.

In the campaign, alone or in network co-op (port/linux/game/network_coop.c),
with chasm.enabled. controls.talk_to_ai talks to them; alone, push to talk
does too (in network games it is voice chat's). In co-op only the host talks
to chasm and runs the turns, as it alone runs the AI: a client's words go to
it as Opus frames (_distributed_message_chasm_audio_up), what was said and
the notices come back as lines (..._chasm_line_down), and the characters'
voices as Opus too, with their subtitles and where the speaker stands
(..._chasm_voice_down), played where each client's player is. The speaker is
picked around the player who spoke.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/hud_messaging.h"
#include "main/console.h"
#include "main/main.h"
#include "objects/objects.h"
#include "units/units.h"
#include "ai/actors.h"
#include "ai/ai_script.h"
#include "ai/encounters.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "network_coop.h"
#include "network_chat.h"
#include "chasm_bridge.h"
#include "../src/chasm_client.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* port_config.c's, xinput_sdl.c's, cseries_windows.c's */
int config_boolean(const char *name);
double config_real(const char *name);
const char *config_string(const char *name);
unsigned long system_milliseconds(void);
int halo_push_to_talk_held(void);
int halo_talk_to_ai_held(void);
void platform_log(char const *format, ...);
/* network_distributed.c's */
boolean network_distributed_client_send_reliably(void *message, word size);
boolean network_distributed_server_send_to_all_reliably(void *message, word size);

/* ---------- constants */

enum
{
	/* the characters near enough to be spoken to, at most */
	CHASM_NEARBY = 12,
	/* a press shorter than this is not words (ms) */
	CHASM_SHORTEST_RECORDING_MS = 350,
	/* subtitles: how many show, and how long a line stays (ms, and more for
	each character) */
	CHASM_SUBTITLES = 4,
	CHASM_SUBTITLE_MS = 4000,
	CHASM_SUBTITLE_MS_PER_CHARACTER = 55,
	CHASM_SUBTITLE_FADE_MS = 600,
	CHASM_SUBTITLE_SIZE = 512,
	CHASM_SUBTITLE_ROWS = 4,
	/* the subtitles' column, as a percentage of the view's width */
	CHASM_COLUMN_PERCENT = 60,

	/* co-op: a client's recording, at most (frames of 20 ms), sent this many
	frames at a time; the machines whose recordings the host keeps at once */
	CHASM_UPLOAD_FRAMES = 1500,
	CHASM_UPLOAD_FRAMES_A_MESSAGE = 12,
	CHASM_UPLOADS = 8,
	/* a client waiting on the host's answer gives up after this (ms) */
	CHASM_CLIENT_WAIT_MS = 30000,
	/* a message: its payload at most (a reliable message holds 4095 bytes) */
	CHASM_MESSAGE_PAYLOAD = 3900,

	/* the lines' kinds (_distributed_message_chasm_line_down) */
	_chasm_line_player = 0,
	_chasm_line_notice,
	/* someone spoke over the character: its voice stops */
	_chasm_line_stop,

	/* reactions: how often the game is looked at (ms); the friends watched
	at once */
	CHASM_REACTION_SCAN_MS = 500,
	CHASM_WATCHED = 32,
};

/* a world unit is ten feet */
#define CHASM_METERS_PER_WORLD_UNIT 3.048f
/* under the crosshair: this near the aim (its cosine, about 12 degrees) */
#define CHASM_CROSSHAIR_COSINE 0.978f
/* a fight: an enemy alive this near (world units) */
#define CHASM_COMBAT_DISTANCE 12.0f
/* a voice at full volume this near, quieter further (world units) */
#define CHASM_VOICE_NEAR 2.0f
#define CHASM_VOICE_FAR 15.0f
#define CHASM_VOICE_FARTHEST_GAIN 0.35f
/* reactions: a witness is a friend this near what happened (world units) */
#define CHASM_WITNESS_DISTANCE 15.0f

#define CHASM_GAME_ID "halo-combat-evolved"
#define CHASM_CORTANA "Cortana"

enum
{
	_chasm_state_idle,
	_chasm_state_listening,
	/* what was said being recognized */
	_chasm_state_hearing,
	/* a character's turn: thinking, then talking */
	_chasm_state_turn,
};

/* ---------- structures */

/* who a unit is: the first whose tag matches a part of the name of its
dialogue's tag, else of its unit's */
struct chasm_character
{
	char const *tag_part;
	char const *name;
	/* what the player may call them, "|" between ("" for the name alone) */
	char const *called;
	boolean covenant;
};

struct chasm_nearby
{
	long unit_index;
	long actor_index;
	char const *name;
	real distance;
	/* the aim's cosine to it */
	real aim;
	boolean hostile;
	boolean under_crosshair;
};

struct chasm_subtitle
{
	unsigned long time;
	unsigned long lasts;
	/* 0 a character's line, 1 the player's, 2 a notice */
	short kind;
	wchar_t name[32];
	wchar_t text[CHASM_SUBTITLE_SIZE];
};

/* ---------- globals */

static struct chasm_character const chasm_characters[] =
{
	{ "monitor", "343 Guilty Spark", "spark|guilty spark|monitor", FALSE },
	{ "johnson", "Sergeant Johnson", "johnson|sergeant|sarge|sarg", FALSE },
	{ "keyes", "Captain Keyes", "keyes|keys|captain|skipper", FALSE },
	{ "captain", "Captain Keyes", "keyes|keys|captain|skipper", FALSE },
	{ "bisenti", "Bisenti", "bisenti|benny", FALSE },
	{ "fitzgerald", "Fitzgerald", "fitzgerald|fitz", FALSE },
	{ "jenkins", "Jenkins", "jenkins", FALSE },
	{ "mendoza", "Mendoza", "mendoza", FALSE },
	{ "aussie", "Chips Dubbo", "chips|dubbo|aussie", FALSE },
	{ "dubbo", "Chips Dubbo", "chips|dubbo|aussie", FALSE },
	{ "crewman", "Crewman", "crewman|crew", FALSE },
	{ "marine", "Marine", "marine|private|soldier", FALSE },
	{ "elite", "Elite", "elite|sangheili", TRUE },
	{ "grunt", "Grunt", "grunt|unggoy", TRUE },
	{ "jackal", "Jackal", "jackal|kig-yar", TRUE },
	{ "hunter", "Hunter", "hunter|mgalekgolo", TRUE },
};

static struct
{
	short state;
	int turn;
	boolean was_enabled;
	boolean was_held;
	/* the turn's speaker (NONE: Cortana), and what they are called */
	long speaker_unit_index;
	char speaker[64];
	/* when the answer was asked for (the "thinking" shown until it comes) */
	unsigned long asked_at;
	boolean answered;
	struct chasm_subtitle subtitles[CHASM_SUBTITLES];
	short subtitle_count;
	short newest_subtitle;
	/* debug.chasm_say's words said on this map (and debug.chasm_react's
	narration) */
	boolean said;
	boolean reacted;
	/* the mode this machine is in (chasm_client.h's), and the player who
	spoke this turn's words (an absolute index; NONE: a reaction) */
	short mode;
	long talker_player_index;
	/* (a co-op client) the speaker's place and unit, as the host last said;
	when its voice last came; this machine's recording sent so far, and the
	recording's number */
	real_point3d speaker_position;
	long relayed_unit_index;
	unsigned long relayed_at;
	byte utterance;
	short uploaded_frames;
	unsigned long say_started_at;
	/* (the host) when a reaction was last given, and anything last said */
	unsigned long reacted_at;
	unsigned long talked_at;
	/* units whose dialogue's tag matched nothing: logged once */
	long logged_definition_indices[16];
	short logged_count;
} chasm_bridge;

/* ---------- private code */

/* what this machine is to chasm now (chasm_client.h's modes): in the
campaign, alone or the host of a co-op game, it talks to chasm; a co-op
client, to the host */
static short chasm_mode_now(
	void)
{
	if (!config_boolean("chasm.enabled") || !game_in_progress() || game_engine_running())
		return _chasm_mode_off;
	switch (game_connection())
	{
	case _game_connection_local:
		return chasm_client_available() ? _chasm_mode_host : _chasm_mode_off;
	case _game_connection_network_server:
		return network_coop_active() && chasm_client_available() ? _chasm_mode_host : _chasm_mode_off;
	case _game_connection_network_client:
		return network_coop_active() ? _chasm_mode_client : _chasm_mode_off;
	default:
		return _chasm_mode_off;
	}
}

static long chasm_player_unit(
	void)
{
	long player_index = local_player_get_player_index(0);

	if (player_index == NONE)
		return NONE;
	return player_get(player_index)->unit_index;
}

/* a tag's name without its folders ("weapons\assault rifle\assault rifle":
"assault rifle") */
static char const *chasm_tag_base_name(
	long tag_index)
{
	char const *name;
	char const *slash;

	if (tag_index == NONE)
		return "";
	name = tag_get_name(tag_index);
	if (!name)
		return "";
	slash = strrchr(name, '\\');
	return slash ? slash + 1 : name;
}

static boolean chasm_contains(
	char const *text,
	char const *part)
{
	size_t length = strlen(part);

	for (; text && *text; text++)
	{
		if (!_strnicmp(text, part, length))
			return TRUE;
	}
	return FALSE;
}

static struct chasm_character const *chasm_character_find(
	char const *tag_name)
{
	short index;

	for (index = 0; index < (short)NUMBEROF(chasm_characters); index++)
	{
		if (chasm_contains(tag_name, chasm_characters[index].tag_part))
			return &chasm_characters[index];
	}
	return NULL;
}

/* who a unit is, or NULL for none that talks */
static struct chasm_character const *chasm_unit_character(
	long unit_index)
{
	struct unit_datum *unit = (struct unit_datum *)object_get_and_verify_type(unit_index, _object_mask_unit);
	struct chasm_character const *character = NULL;
	char const *unit_name = tag_get_name(unit->definition_index);
	short index;

	/* (the Flood's combat forms keep their hosts' dialogue, and say nothing;
	nor do the Sentinels) */
	if (chasm_contains(unit_name, "flood") || chasm_contains(unit_name, "sentinel"))
		return NULL;
	if (unit->unit.dialogue_index != NONE)
		character = chasm_character_find(tag_get_name(unit->unit.dialogue_index));
	if (!character)
		character = chasm_character_find(tag_get_name(unit->definition_index));
	if (!character)
	{
		for (index = 0; index < chasm_bridge.logged_count; index++)
		{
			if (chasm_bridge.logged_definition_indices[index] == unit->definition_index)
				return NULL;
		}
		if (chasm_bridge.logged_count < (short)NUMBEROF(chasm_bridge.logged_definition_indices))
		{
			chasm_bridge.logged_definition_indices[chasm_bridge.logged_count++] = unit->definition_index;
			platform_log("chasm: no character for the unit %s (dialogue %s)", tag_get_name(unit->definition_index),
				unit->unit.dialogue_index != NONE ? tag_get_name(unit->unit.dialogue_index) : "none");
		}
	}
	return character;
}

static real chasm_distance(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

/* the characters near the player, nearest first; how many */
static short chasm_find_nearby(
	long player_unit_index,
	struct chasm_nearby *nearby,
	short maximum)
{
	struct unit_datum *player = (struct unit_datum *)object_get_and_verify_type(player_unit_index, _object_mask_unit);
	real_point3d eye;
	real_vector3d aim;
	real reach = (real)(config_real("chasm.distance") / CHASM_METERS_PER_WORLD_UNIT);
	struct object_iterator iterator;
	short count = 0;

	unit_get_head_position(player_unit_index, (union real_point3d *)&eye);
	aim = player->unit.aiming_vector;
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while (object_iterator_next(&iterator))
	{
		struct unit_datum *unit = (struct unit_datum *)object_get_and_verify_type(iterator.index, _object_mask_unit);
		struct chasm_character const *character;
		struct chasm_nearby candidate;
		real_point3d head;
		short at;

		if (iterator.index == player_unit_index || unit->unit.player_index != NONE ||
			TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		{
			continue;
		}
		unit_get_head_position(iterator.index, (union real_point3d *)&head);
		candidate.distance = chasm_distance(&head, &eye);
		if (candidate.distance > reach)
			continue;
		character = chasm_unit_character(iterator.index);
		if (!character)
			continue;
		candidate.unit_index = iterator.index;
		candidate.actor_index = unit->unit.actor_index;
		candidate.name = character->name;
		candidate.hostile = game_team_is_enemy(player->object.owner_team_index, unit->object.owner_team_index);
		candidate.aim = candidate.distance > 0.0f ?
			((head.x - eye.x) * aim.i + (head.y - eye.y) * aim.j + (head.z - eye.z) * aim.k) / candidate.distance : 1.0f;
		candidate.under_crosshair = candidate.aim >= CHASM_CROSSHAIR_COSINE;
		/* (in order of distance) */
		for (at = count; at > 0 && nearby[at - 1].distance > candidate.distance; at--)
		{
			if (at < maximum)
				nearby[at] = nearby[at - 1];
		}
		if (at < maximum)
		{
			nearby[at] = candidate;
			if (count < maximum)
				count++;
		}
	}
	return count;
}

/* whether the words name a character ("|" between its names), as a word */
static boolean chasm_words_name(
	char const *words,
	char const *called)
{
	while (*called)
	{
		size_t length = strcspn(called, "|");
		char const *at;

		for (at = words; *at; at++)
		{
			if (!_strnicmp(at, called, length) && (at == words || !isalpha((unsigned char)at[-1])) &&
				!isalpha((unsigned char)at[length]))
			{
				return TRUE;
			}
		}
		called += length;
		if (*called == '|')
			called++;
	}
	return FALSE;
}

static char const *chasm_called(
	char const *name)
{
	short index;

	for (index = 0; index < (short)NUMBEROF(chasm_characters); index++)
	{
		if (!strcmp(chasm_characters[index].name, name))
			return chasm_characters[index].called;
	}
	return "";
}

/* who is spoken to: one named, else under the crosshair, else the nearest
friend (an enemy only under the crosshair); NONE for Cortana */
static short chasm_pick_speaker(
	char const *words,
	struct chasm_nearby const *nearby,
	short count)
{
	short index;
	short best = NONE;

	if (chasm_words_name(words, "cortana"))
		return NONE;
	for (index = 0; index < count; index++)
	{
		if (chasm_words_name(words, chasm_called(nearby[index].name)) && (!nearby[index].hostile || nearby[index].under_crosshair))
			return index;
	}
	for (index = 0; index < count; index++)
	{
		if (nearby[index].under_crosshair && (best == NONE || nearby[index].aim > nearby[best].aim))
			best = index;
	}
	if (best != NONE)
		return best;
	for (index = 0; index < count; index++)
	{
		if (!nearby[index].hostile)
			return index;
	}
	return NONE;
}

/* ---------- JSON, written */

struct chasm_json
{
	char *text;
	int length;
	int size;
};

static void chasm_json_raw(
	struct chasm_json *json,
	char const *text)
{
	int length = (int)strlen(text);

	if (json->length + length >= json->size)
		length = json->size - json->length - 1;
	if (length <= 0)
		return;
	memcpy(json->text + json->length, text, length);
	json->length += length;
	json->text[json->length] = 0;
}

/* "name":"value" (escaped), after a comma unless it is the first; a name
of NULL: an array's element */
static void chasm_json_string(
	struct chasm_json *json,
	char const *name,
	char const *value)
{
	char escaped[2048];

	chasm_json_escape(value ? value : "", escaped, sizeof(escaped));
	if (json->length && json->text[json->length - 1] != '{' && json->text[json->length - 1] != '[')
		chasm_json_raw(json, ",");
	if (name)
	{
		chasm_json_raw(json, "\"");
		chasm_json_raw(json, name);
		chasm_json_raw(json, "\":");
	}
	chasm_json_raw(json, "\"");
	chasm_json_raw(json, escaped);
	chasm_json_raw(json, "\"");
}

/* "name":<raw value> */
static void chasm_json_value(
	struct chasm_json *json,
	char const *name,
	char const *value)
{
	if (json->length && json->text[json->length - 1] != '{' && json->text[json->length - 1] != '[')
		chasm_json_raw(json, ",");
	if (name)
	{
		chasm_json_raw(json, "\"");
		chasm_json_raw(json, name);
		chasm_json_raw(json, "\":");
	}
	chasm_json_raw(json, value);
}

static void chasm_json_number(
	struct chasm_json *json,
	char const *name,
	real value)
{
	char text[32];

	_snprintf(text, sizeof(text), "%.2f", value);
	text[sizeof(text) - 1] = 0;
	chasm_json_value(json, name, text);
}

static char const *chasm_mission_name(
	char const *map_name)
{
	static struct
	{
		char const *map;
		char const *mission;
	} const missions[] =
	{
		{ "a10", "The Pillar of Autumn" },
		{ "a30", "Halo" },
		{ "a50", "The Truth and Reconciliation" },
		{ "b30", "The Silent Cartographer" },
		{ "b40", "Assault on the Control Room" },
		{ "c10", "343 Guilty Spark" },
		{ "c20", "The Library" },
		{ "c40", "Two Betrayals" },
		{ "d20", "Keyes" },
		{ "d40", "The Maw" },
	};
	short index;

	for (index = 0; index < (short)NUMBEROF(missions); index++)
	{
		if (!_stricmp(map_name, missions[index].map))
			return missions[index].mission;
	}
	return map_name;
}

static char const *chasm_difficulty_name(
	void)
{
	switch (game_difficulty_level_get())
	{
	case _game_difficulty_level_easy: return "Easy";
	case _game_difficulty_level_normal: return "Normal";
	case _game_difficulty_level_hard: return "Heroic";
	default: return "Legendary";
	}
}

/* the turn's request: its JSON members, without the braces (speaker_index:
nearby's spoken to, NONE for Cortana; talker: the player's name; reaction:
for a reaction turn, its narration, else NULL) */
static void chasm_build_turn(
	struct chasm_json *json,
	char const *words,
	char const *talker,
	char const *reaction,
	char const *speaker,
	short speaker_index,
	long player_unit_index,
	struct chasm_nearby const *nearby,
	short count)
{
	struct unit_datum *player = (struct unit_datum *)object_get_and_verify_type(player_unit_index, _object_mask_unit);
	char const *map = chasm_tag_base_name(global_scenario_index);
	char const *mission = chasm_mission_name(map);
	char const *weapon = "";
	char text[256];
	boolean fighting = FALSE;
	boolean seen[NUMBEROF(chasm_characters)];
	short index;

	csmemset(seen, 0, sizeof(seen));
	if (player->unit.current_weapon_index >= 0 && player->unit.current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT &&
		player->unit.weapon_object_indices[player->unit.current_weapon_index] != NONE)
	{
		weapon = chasm_tag_base_name(
			object_get(player->unit.weapon_object_indices[player->unit.current_weapon_index])->definition_index);
	}
	_snprintf(text, sizeof(text), "halo_%lu", system_milliseconds());
	text[sizeof(text) - 1] = 0;
	chasm_json_string(json, "request_id", text);
	chasm_json_string(json, "npc_key", speaker);
	chasm_json_string(json, "npc_name", speaker);
	chasm_json_string(json, "player_text", words);
	chasm_json_value(json, "want_tts", "true");
	chasm_json_value(json, "location", "{");
	chasm_json_string(json, "cell", map);
	chasm_json_string(json, "worldspace", "Halo");
	chasm_json_string(json, "region", mission);
	chasm_json_string(json, "major", mission);
	chasm_json_string(json, "minor", map);
	chasm_json_raw(json, "}");
	chasm_json_value(json, "metadata", "{");
	if (reaction)
	{
		chasm_json_value(json, "reaction", "{");
		chasm_json_string(json, "narration", reaction);
		chasm_json_raw(json, "}");
	}
	chasm_json_value(json, "targeting", "{");
	chasm_json_value(json, "nearby_npcs", "[");
	/* (Cortana, in the player's head, only when she is spoken to, and then
	alone: the others do not hear her) */
	if (!strcmp(speaker, CHASM_CORTANA))
	{
		count = 0;
		chasm_json_value(json, NULL, "{");
		chasm_json_string(json, "npc_key", CHASM_CORTANA);
		chasm_json_string(json, "npc_name", CHASM_CORTANA);
		chasm_json_string(json, "characterId", CHASM_CORTANA);
		chasm_json_number(json, "distance_m", 0.0f);
		chasm_json_value(json, "under_crosshair", "true");
		chasm_json_raw(json, "}");
	}
	for (index = 0; index < count; index++)
	{
		short other;
		boolean listed = speaker_index != NONE && index != speaker_index &&
			!strcmp(nearby[speaker_index].name, nearby[index].name);

		/* (each character once - two plain Marines are one to chasm - the
		one spoken to, else the nearest) */
		for (other = 0; other < index && !listed; other++)
			listed = index != speaker_index && !strcmp(nearby[other].name, nearby[index].name);
		if (listed)
			continue;
		chasm_json_value(json, NULL, "{");
		chasm_json_string(json, "npc_key", nearby[index].name);
		chasm_json_string(json, "npc_name", nearby[index].name);
		chasm_json_string(json, "characterId", nearby[index].name);
		chasm_json_number(json, "distance_m", nearby[index].distance * CHASM_METERS_PER_WORLD_UNIT);
		chasm_json_value(json, "under_crosshair", index == speaker_index ? "true" : "false");
		chasm_json_value(json, "hostile", nearby[index].hostile ? "true" : "false");
		chasm_json_raw(json, "}");
	}
	chasm_json_raw(json, "]}");
	/* (a fight: an enemy near; who with) */
	chasm_json_value(json, "combat_with", "[");
	{
		struct object_iterator iterator;
		real_point3d *position = &player->object.position;

		object_iterator_new(&iterator, _object_mask_biped, 0);
		while (object_iterator_next(&iterator))
		{
			struct unit_datum *unit = (struct unit_datum *)object_get_and_verify_type(iterator.index, _object_mask_unit);
			struct chasm_character const *character;

			if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || unit->unit.actor_index == NONE ||
				!game_team_is_enemy(player->object.owner_team_index, unit->object.owner_team_index) ||
				chasm_distance(&unit->object.position, position) > CHASM_COMBAT_DISTANCE)
			{
				continue;
			}
			fighting = TRUE;
			character = chasm_unit_character(iterator.index);
			if (character && !seen[character - chasm_characters])
			{
				seen[character - chasm_characters] = TRUE;
				chasm_json_string(json, NULL, character->name);
			}
		}
	}
	chasm_json_raw(json, "]");
	chasm_json_value(json, "in_combat", fighting ? "true" : "false");
	chasm_json_value(json, "npc_state", "{");
	chasm_json_value(json, "teammate", "true");
	chasm_json_value(json, "in_combat", fighting ? "true" : "false");
	chasm_json_raw(json, "}");
	/* (chasm's {{macros}}, and the same as the turn's game state) */
	{
		char health[16], shield[16];
		short pass;

		_snprintf(health, sizeof(health), "%d%%", (int)(PIN(player->object.body_vitality, 0.0f, 1.0f) * 100.0f));
		health[sizeof(health) - 1] = 0;
		_snprintf(shield, sizeof(shield), "%d%%", (int)(PIN(player->object.shield_vitality, 0.0f, 1.0f) * 100.0f));
		shield[sizeof(shield) - 1] = 0;
		for (pass = 0; pass < 2; pass++)
		{
			chasm_json_value(json, pass ? "gamestate" : "macros", "{");
			chasm_json_string(json, "player_name", talker);
			chasm_json_string(json, "major_location", mission);
			chasm_json_string(json, "mission", mission);
			chasm_json_string(json, "map", map);
			chasm_json_string(json, "health", health);
			chasm_json_string(json, "shield", shield);
			chasm_json_string(json, "weapon", weapon);
			chasm_json_string(json, "difficulty", chasm_difficulty_name());
			chasm_json_string(json, "in_combat", fighting ? "yes" : "no");
			chasm_json_raw(json, "}");
		}
	}
	chasm_json_raw(json, "}");
}

/* ---------- subtitles */

/* UTF-8 as the HUD's font draws it: the quotes and dashes it lacks as their
ASCII, the rest of what it lacks as nothing */
static short chasm_text_for_font(
	char const *text,
	wchar_t *drawn,
	short size)
{
	unsigned char const *at = (unsigned char const *)text;
	short length = 0;

	while (*at && length < size - 1)
	{
		unsigned long code = *at++;

		if (code >= 0xc0)
		{
			short more = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : 1;

			code &= more == 3 ? 0x07 : more == 2 ? 0x0f : 0x1f;
			while (more-- > 0 && (*at & 0xc0) == 0x80)
				code = (code << 6) | (*at++ & 0x3f);
		}
		if (code == 0x2018 || code == 0x2019)
			code = '\'';
		else if (code == 0x201c || code == 0x201d)
			code = '"';
		else if (code == 0x2013 || code == 0x2014)
			code = '-';
		else if (code == 0x2026)
		{
			if (length < size - 3)
			{
				drawn[length++] = '.';
				drawn[length++] = '.';
			}
			code = '.';
		}
		else if (code == '\n' || code == '\t')
			code = ' ';
		else if (code < 0x20 || code > 0xff)
			continue;
		drawn[length++] = (wchar_t)code;
	}
	drawn[length] = 0;
	return length;
}

static void chasm_subtitle_add(
	short kind,
	char const *name,
	char const *text)
{
	struct chasm_subtitle *subtitle;
	short length;

	if (!text || !text[0])
		return;
	chasm_bridge.newest_subtitle = (short)((chasm_bridge.newest_subtitle + 1) % CHASM_SUBTITLES);
	if (chasm_bridge.subtitle_count < CHASM_SUBTITLES)
		chasm_bridge.subtitle_count++;
	subtitle = &chasm_bridge.subtitles[chasm_bridge.newest_subtitle];
	platform_log("chasm: subtitle: %s%s%s", name ? name : "", name ? ": " : "", text);
	chasm_text_for_font(name ? name : "", subtitle->name, NUMBEROF(subtitle->name));
	length = chasm_text_for_font(text, subtitle->text, NUMBEROF(subtitle->text));
	subtitle->kind = kind;
	subtitle->time = system_milliseconds();
	subtitle->lasts = CHASM_SUBTITLE_MS + (unsigned long)length * CHASM_SUBTITLE_MS_PER_CHARACTER;
}

/* ---------- actions */

/* an actor's AI references: its squad (or encounter), its encounter; NONE
for none (an actor of no encounter) */
static void chasm_actor_references(
	long actor_index,
	long *squad_reference,
	long *encounter_reference)
{
	struct actor_datum *actor = actor_get(actor_index);
	long encounter = actor->meta.encounter_index;

	*squad_reference = *encounter_reference = NONE;
	if (encounter == NONE)
		return;
	/* (ai_script.c's: the type in the top two bits, the squad's in the
	third byte, the encounter's in the low word) */
	*encounter_reference = DATUM_INDEX_TO_ABSOLUTE_INDEX(encounter);
	*squad_reference = actor->meta.squad_index != NONE ?
		(long)((2UL << 30) | ((unsigned long)(actor->meta.squad_index & 0xff) << 16) |
			(unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(encounter)) :
		*encounter_reference;
}

/* the vehicle nearest the player, within reach; NONE for none */
static long chasm_nearest_vehicle(
	long player_unit_index)
{
	struct object_datum *player = object_get(player_unit_index);
	struct object_iterator iterator;
	real best = (real)(config_real("chasm.distance") / CHASM_METERS_PER_WORLD_UNIT) * 2.0f;
	long vehicle = NONE;

	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		real distance = chasm_distance(&object->object.position, &player->object.position);

		if (!TEST_FLAG(object->object.damage_flags, _object_dead_bit) && distance < best)
		{
			best = distance;
			vehicle = iterator.index;
		}
	}
	return vehicle;
}

/* an action the speaker chose, done; whether it was one this knows */
static boolean chasm_act(
	char const *action_id,
	long unit_index,
	long player_unit_index)
{
	struct unit_datum *unit;
	long actor_index;
	long squad, encounter;

	if (unit_index == NONE || !object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
		return FALSE;
	unit = (struct unit_datum *)object_get_and_verify_type(unit_index, _object_mask_unit);
	actor_index = unit->unit.actor_index;
	if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || actor_index == NONE)
		return FALSE;
	chasm_actor_references(actor_index, &squad, &encounter);
	if (!strcmp(action_id, "world.give_item"))
	{
		return unit_drop_current_weapon(unit_index, FALSE);
	}
	if (!strcmp(action_id, "combat.start"))
	{
		actor_set_team(actor_index, _game_team_covenant);
		if (squad != NONE)
			ai_scripting_try_to_fight_player(squad);
		return TRUE;
	}
	if (!strcmp(action_id, "combat.stop"))
	{
		actor_set_team(actor_index, _game_team_human);
		if (squad != NONE)
			ai_scripting_try_to_fight_nothing(squad);
		return TRUE;
	}
	if (!strcmp(action_id, "npc.look_at_player"))
	{
		ai_scripting_look_at_object(unit_index, player_unit_index);
		return TRUE;
	}
	/* (the rest are orders to the character's squad, as the levels' scripts
	give them) */
	if (encounter == NONE)
		return FALSE;
	if (!strcmp(action_id, "movement.follow_target"))
	{
		ai_scripting_follow_target_players(encounter);
		ai_scripting_attack(squad);
		return TRUE;
	}
	if (!strcmp(action_id, "movement.stop_follow_target"))
	{
		ai_scripting_follow_target_disable(encounter);
		return TRUE;
	}
	if (!strcmp(action_id, "ai.wait_here"))
	{
		ai_scripting_follow_target_disable(encounter);
		ai_scripting_defend(squad);
		return TRUE;
	}
	if (!strcmp(action_id, "halo.advance"))
	{
		ai_scripting_attack(squad);
		ai_scripting_maneuver_enable(squad, TRUE);
		return TRUE;
	}
	if (!strcmp(action_id, "halo.berserk"))
	{
		ai_scripting_berserk(squad, TRUE);
		return TRUE;
	}
	if (!strcmp(action_id, "halo.enter_vehicle"))
	{
		long vehicle = chasm_nearest_vehicle(player_unit_index);

		if (vehicle == NONE)
			return FALSE;
		ai_scripting_go_to_vehicle(squad, vehicle, "");
		return TRUE;
	}
	if (!strcmp(action_id, "halo.exit_vehicle"))
	{
		ai_scripting_exit_vehicle(squad);
		return TRUE;
	}
	return FALSE;
}

/* ---------- the turn */

/* a player's unit (an absolute index; NONE: the first local player's), or
NONE */
static long chasm_player_unit_of(
	long player_index)
{
	struct player_datum *player;

	if (player_index == NONE)
		return chasm_player_unit();
	player = (struct player_datum *)datum_try_and_get(player_data, player_index);
	return player ? player->unit_index : NONE;
}

/* a player's name (an absolute index), in ASCII: "Master Chief" alone */
static void chasm_player_name(
	long player_index,
	char *name,
	short size)
{
	struct player_datum *player = player_index != NONE ? (struct player_datum *)datum_try_and_get(player_data, player_index) : NULL;
	short index;

	if (!player || game_connection() == _game_connection_local)
	{
		csstrncpy(name, "Master Chief", size);
		name[size - 1] = 0;
		return;
	}
	for (index = 0; index < size - 1 && index < (short)NUMBEROF(player->name) && player->name[index]; index++)
		name[index] = player->name[index] < 0x80 && player->name[index] >= 0x20 ? (char)player->name[index] : '?';
	name[index] = 0;
	if (!index)
		csstrncpy(name, "Spartan", size);
}

/* a turn: words a player said (talker_player_index's, NONE: the first local
player), or a reaction (its narration) given to a witness (a unit; NONE:
Cortana) */
static void chasm_start_turn(
	char const *words,
	long talker_player_index,
	char const *reaction,
	long witness_unit_index)
{
	long player_unit_index = chasm_player_unit_of(talker_player_index);
	struct chasm_nearby nearby[CHASM_NEARBY];
	short count, speaker = NONE, index;
	static char members[24 * 1024];
	struct chasm_json json;
	char talker[32];

	if (player_unit_index == NONE || !object_try_and_get_and_verify_type(player_unit_index, _object_mask_unit))
	{
		chasm_bridge.state = _chasm_state_idle;
		return;
	}
	chasm_player_name(talker_player_index, talker, sizeof(talker));
	count = chasm_find_nearby(player_unit_index, nearby, CHASM_NEARBY);
	if (reaction)
	{
		for (index = 0; index < count; index++)
		{
			if (nearby[index].unit_index == witness_unit_index)
				speaker = index;
		}
	}
	else
	{
		speaker = chasm_pick_speaker(words, nearby, count);
	}
	chasm_bridge.talker_player_index = reaction ? NONE : talker_player_index;
	chasm_bridge.speaker_unit_index = speaker != NONE ? nearby[speaker].unit_index : NONE;
	csstrncpy(chasm_bridge.speaker, speaker != NONE ? nearby[speaker].name : CHASM_CORTANA, sizeof(chasm_bridge.speaker));
	chasm_bridge.speaker[sizeof(chasm_bridge.speaker) - 1] = 0;
	json.text = members;
	json.length = 0;
	json.size = sizeof(members);
	members[0] = 0;
	chasm_build_turn(&json, reaction ? "" : words, talker, reaction, chasm_bridge.speaker, speaker, player_unit_index,
		nearby, count);
	chasm_bridge.turn = chasm_client_send_turn(members);
	chasm_bridge.state = chasm_bridge.turn ? _chasm_state_turn : _chasm_state_idle;
	chasm_bridge.asked_at = system_milliseconds();
	chasm_bridge.talked_at = chasm_bridge.asked_at;
	chasm_bridge.answered = FALSE;
	if (reaction)
		platform_log("chasm: %s reacts to: %s", chasm_bridge.speaker, reaction);
	else
		platform_log("chasm: %s to %s (%d near): \"%s\"", talker, chasm_bridge.speaker, (int)count, words);
	/* (they turn to the player) */
	if (!reaction && chasm_bridge.speaker_unit_index != NONE && nearby[speaker].actor_index != NONE &&
		!nearby[speaker].hostile)
	{
		ai_scripting_look_at_object(chasm_bridge.speaker_unit_index, player_unit_index);
	}
}

/* the voice placed where its speaker stands (position), as the first local
player hears it: the side by the aim, quieter further */
static void chasm_place_voice(
	real_point3d const *position)
{
	long player_unit_index = chasm_player_unit();
	struct unit_datum *player;
	real_vector3d to;
	real distance, right, gain;

	if (player_unit_index == NONE || !object_try_and_get_and_verify_type(player_unit_index, _object_mask_unit))
	{
		chasm_client_voice_place(1.0f, 0.0f);
		return;
	}
	player = (struct unit_datum *)object_get_and_verify_type(player_unit_index, _object_mask_unit);
	to.i = position->x - player->object.position.x;
	to.j = position->y - player->object.position.y;
	to.k = 0.0f;
	distance = (real)sqrt(to.i * to.i + to.j * to.j);
	/* (the side: the aim's right, in the ground's plane) */
	right = distance > 0.01f ? (to.i * player->unit.aiming_vector.j - to.j * player->unit.aiming_vector.i) / distance :
		0.0f;
	gain = distance <= CHASM_VOICE_NEAR ? 1.0f :
		distance >= CHASM_VOICE_FAR ? CHASM_VOICE_FARTHEST_GAIN :
		1.0f - (1.0f - CHASM_VOICE_FARTHEST_GAIN) * (distance - CHASM_VOICE_NEAR) / (CHASM_VOICE_FAR - CHASM_VOICE_NEAR);
	chasm_client_voice_place(gain, PIN(right, -1.0f, 1.0f) * 0.7f);
}

/* a unit's mouth moved with the voice playing */
static void chasm_move_mouth(
	long unit_index)
{
	if (unit_index != NONE && object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
		unit_set_mouth_aperture(unit_index, PIN(chasm_client_voice_level() * 6.0f, 0.0f, 1.0f));
}

/* (the host) the speaker's mouth moved with its voice, and the voice placed
where they stand */
static void chasm_update_speaker(
	void)
{
	long unit_index = chasm_bridge.speaker_unit_index;
	struct unit_datum *unit;

	if (unit_index == NONE || !object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
	{
		/* (Cortana, or one gone: in the player's head) */
		chasm_client_voice_place(1.0f, 0.0f);
		return;
	}
	unit = (struct unit_datum *)object_get_and_verify_type(unit_index, _object_mask_unit);
	if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
	{
		/* (the dead do not talk) */
		chasm_client_cancel();
		return;
	}
	chasm_move_mouth(unit_index);
	chasm_place_voice(&unit->object.position);
}

/* ---------- co-op */

static boolean chasm_coop_host(
	void)
{
	return game_connection() == _game_connection_network_server;
}

/* a message's header, as voice chat's are made (network_voice.c) */
static void chasm_message_header(
	byte *message,
	byte type,
	word size)
{
	struct distributed_message_header header;

	csmemset(&header, 0, sizeof(header));
	build_message_header(&header.header, size, 2, 0);
	header.type = type;
	header.count = 1;
	header.game_time = game_in_progress() ? game_time_get() : 0;
	csmemcpy(message, &header, sizeof(header));
}

/* (the host) a line to every client: what a player said, a notice, or that
the voice stops */
static void chasm_send_line(
	short kind,
	long player_index,
	char const *text)
{
	byte message[sizeof(struct distributed_message_header) + 4 + CHASM_RELAY_CAPTION_SIZE];
	word length = (word)MIN(strlen(text ? text : ""), CHASM_RELAY_CAPTION_SIZE - 1);
	word at = sizeof(struct distributed_message_header);

	if (!chasm_coop_host())
		return;
	message[at++] = (byte)kind;
	message[at++] = player_index != NONE ? distributed_player_to_byte(player_index) : NO_PLAYER;
	csmemcpy(message + at, &length, sizeof(length));
	at += sizeof(length);
	csmemcpy(message + at, text ? text : "", length);
	at += length;
	chasm_message_header(message, _distributed_message_chasm_line_down, at);
	network_distributed_server_send_to_all_reliably(message, at);
}

/* (the host) the voices kept for clients sent to them, with where their
speaker stands */
static void chasm_send_relays(
	void)
{
	struct chasm_relay relay;

	while (chasm_client_next_relay(&relay))
	{
		byte message[sizeof(struct distributed_message_header) + 32 + 64 + CHASM_RELAY_CAPTION_SIZE + CHASM_RELAY_FRAMES +
			CHASM_RELAY_BYTES];
		word at = sizeof(struct distributed_message_header);
		word turn = (word)relay.turn;
		long unit_index = chasm_bridge.speaker_unit_index;
		real_point3d position;
		byte key_length = (byte)MIN(strlen(relay.npc_key), 63);
		word caption_length = (word)MIN(strlen(relay.caption), CHASM_RELAY_CAPTION_SIZE - 1);
		word bytes = 0;
		short index;

		csmemset(&position, 0, sizeof(position));
		if (unit_index != NONE && object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
			unit_get_head_position(unit_index, (union real_point3d *)&position);
		else
			unit_index = NONE;
		for (index = 0; index < relay.count; index++)
			bytes = (word)(bytes + relay.lengths[index]);
		csmemcpy(message + at, &turn, sizeof(turn));
		at += sizeof(turn);
		message[at++] = (byte)(relay.first ? 1 : 0);
		message[at++] = (byte)relay.count;
		csmemcpy(message + at, &unit_index, sizeof(unit_index));
		at += sizeof(unit_index);
		csmemcpy(message + at, &position, sizeof(position));
		at += sizeof(position);
		message[at++] = key_length;
		csmemcpy(message + at, &caption_length, sizeof(caption_length));
		at += sizeof(caption_length);
		csmemcpy(message + at, relay.npc_key, key_length);
		at += key_length;
		csmemcpy(message + at, relay.caption, caption_length);
		at += caption_length;
		csmemcpy(message + at, relay.lengths, relay.count);
		at += relay.count;
		csmemcpy(message + at, relay.bytes, bytes);
		at += bytes;
		chasm_message_header(message, _distributed_message_chasm_voice_down, at);
		network_distributed_server_send_to_all_reliably(message, at);
	}
}

/* (a client) this machine's recording sent on to the host, as it comes: the
rest, and its end, when final */
static void chasm_send_recording(
	boolean final)
{
	for (;;)
	{
		byte message[sizeof(struct distributed_message_header) + 8 + CHASM_UPLOAD_FRAMES_A_MESSAGE +
			CHASM_UPLOAD_FRAMES_A_MESSAGE * 255];
		byte lengths[CHASM_UPLOAD_FRAMES_A_MESSAGE];
		byte bytes[CHASM_UPLOAD_FRAMES_A_MESSAGE * 255];
		word at = sizeof(struct distributed_message_header);
		short count;
		short index;
		word size = 0;
		long player_index = local_player_get_player_index(0);
		boolean last;

		count = (short)chasm_client_take_packets(lengths, bytes, CHASM_UPLOAD_FRAMES_A_MESSAGE, sizeof(bytes));
		last = final && count < CHASM_UPLOAD_FRAMES_A_MESSAGE;
		/* (a few frames at a time while held: the rest wait) */
		if (!last && count < CHASM_UPLOAD_FRAMES_A_MESSAGE && !count)
			return;
		for (index = 0; index < count; index++)
			size = (word)(size + lengths[index]);
		message[at++] = chasm_bridge.utterance;
		message[at++] = (byte)((last ? 1 : 0) |
			(last && chasm_client_recorded_ms() < CHASM_SHORTEST_RECORDING_MS ? 2 : 0));
		message[at++] = (byte)count;
		message[at++] = player_index != NONE ? distributed_player_to_byte(player_index) : NO_PLAYER;
		csmemcpy(message + at, &chasm_bridge.uploaded_frames, sizeof(chasm_bridge.uploaded_frames));
		at += sizeof(chasm_bridge.uploaded_frames);
		csmemcpy(message + at, lengths, count);
		at += count;
		csmemcpy(message + at, bytes, size);
		at += size;
		chasm_message_header(message, _distributed_message_chasm_audio_up, at);
		network_distributed_client_send_reliably(message, at);
		chasm_bridge.uploaded_frames = (short)(chasm_bridge.uploaded_frames + count);
		if (last || (!final && count < CHASM_UPLOAD_FRAMES_A_MESSAGE))
			return;
	}
}

/* (the host) the recordings clients are sending, a machine's at a time */
static struct
{
	boolean used;
	long machine_index;
	byte utterance;
	short count;
	word size;
	unsigned long heard_at;
	byte lengths[CHASM_UPLOAD_FRAMES];
	byte bytes[CHASM_UPLOAD_FRAMES * 100];
} chasm_uploads[CHASM_UPLOADS];

static void chasm_receive_recording(
	long machine_index,
	byte const *payload,
	long length)
{
	byte utterance, flags, count, player;
	short first, index, slot = NONE;
	long player_index;
	word size = 0;

	if (length < 6)
		return;
	utterance = payload[0];
	flags = payload[1];
	count = payload[2];
	player = payload[3];
	csmemcpy(&first, payload + 4, sizeof(first));
	if (length < 6 + count)
		return;
	for (index = 0; index < count; index++)
		size = (word)(size + payload[6 + index]);
	if (length < 6 + count + size)
		return;
	player_index = distributed_player_from_byte(player);
	if (player_index == NONE || !distributed_machine_has_player(machine_index, (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index)))
		return;
	/* (the machine's slot, or a free one, or the one longest unheard) */
	for (index = 0; index < CHASM_UPLOADS; index++)
	{
		if (chasm_uploads[index].used && chasm_uploads[index].machine_index == machine_index)
			slot = index;
	}
	for (index = 0; index < CHASM_UPLOADS && slot == NONE; index++)
	{
		if (!chasm_uploads[index].used)
			slot = index;
	}
	if (slot == NONE)
	{
		slot = 0;
		for (index = 1; index < CHASM_UPLOADS; index++)
		{
			if (chasm_uploads[index].heard_at < chasm_uploads[slot].heard_at)
				slot = index;
		}
	}
	/* (a new recording, from its first frame, starts over) */
	if (!chasm_uploads[slot].used || chasm_uploads[slot].machine_index != machine_index ||
		chasm_uploads[slot].utterance != utterance || first == 0)
	{
		chasm_uploads[slot].used = TRUE;
		chasm_uploads[slot].machine_index = machine_index;
		chasm_uploads[slot].utterance = utterance;
		chasm_uploads[slot].count = 0;
		chasm_uploads[slot].size = 0;
	}
	chasm_uploads[slot].heard_at = system_milliseconds();
	if (first == chasm_uploads[slot].count && chasm_uploads[slot].count + count <= CHASM_UPLOAD_FRAMES &&
		chasm_uploads[slot].size + size <= sizeof(chasm_uploads[slot].bytes))
	{
		csmemcpy(chasm_uploads[slot].lengths + chasm_uploads[slot].count, payload + 6, count);
		csmemcpy(chasm_uploads[slot].bytes + chasm_uploads[slot].size, payload + 6 + count, size);
		chasm_uploads[slot].count = (short)(chasm_uploads[slot].count + count);
		chasm_uploads[slot].size = (word)(chasm_uploads[slot].size + size);
	}
	if (!(flags & 1))
		return;
	chasm_uploads[slot].used = FALSE;
	/* (too short to be words) */
	if ((flags & 2) || chasm_uploads[slot].count * 20 < CHASM_SHORTEST_RECORDING_MS)
		return;
	/* (the voice playing stops: someone spoke over it) */
	chasm_client_cancel();
	chasm_send_line(_chasm_line_stop, NONE, "");
	chasm_bridge.turn = chasm_client_recognize_packets(chasm_uploads[slot].lengths, chasm_uploads[slot].bytes,
		chasm_uploads[slot].count);
	chasm_bridge.state = chasm_bridge.turn ? _chasm_state_hearing : _chasm_state_idle;
	chasm_bridge.talker_player_index = player_index;
	chasm_bridge.asked_at = system_milliseconds();
	platform_log("chasm: a recording of %d ms from machine %ld", chasm_uploads[slot].count * 20, machine_index);
}

/* (a client) a line from the host */
static void chasm_receive_line(
	byte const *payload,
	long length)
{
	char text[CHASM_RELAY_CAPTION_SIZE];
	word text_length;
	long player_index;

	if (length < 4)
		return;
	csmemcpy(&text_length, payload + 2, sizeof(text_length));
	if (text_length >= sizeof(text) || length < 4 + text_length)
		return;
	csmemcpy(text, payload + 4, text_length);
	text[text_length] = 0;
	player_index = distributed_player_from_byte(payload[1]);
	switch (payload[0])
	{
	case _chasm_line_player:
	{
		char name[32];

		if (player_index != NONE && distributed_player_is_local(player_index))
			csstrncpy(name, "You", sizeof(name));
		else
			chasm_player_name(player_index, name, sizeof(name));
		chasm_subtitle_add(1, name, text);
		/* (an answer is coming) */
		chasm_bridge.state = _chasm_state_turn;
		chasm_bridge.answered = FALSE;
		chasm_bridge.asked_at = system_milliseconds();
		break;
	}
	case _chasm_line_notice:
		chasm_subtitle_add(2, "chasm", text);
		chasm_bridge.state = _chasm_state_idle;
		break;
	case _chasm_line_stop:
		chasm_client_cancel();
		break;
	}
}

/* (a client) a stretch of a character's voice from the host */
static void chasm_receive_voice(
	byte const *payload,
	long length)
{
	static struct chasm_relay relay;
	word turn, caption_length, bytes = 0;
	byte key_length;
	long unit_index;
	real_point3d position;
	long at = 0;
	short index;

	if (length < 2 + 2 + (long)sizeof(long) + (long)sizeof(position) + 3)
		return;
	csmemcpy(&turn, payload, sizeof(turn));
	at += sizeof(turn);
	relay.first = payload[at++];
	relay.count = payload[at++];
	csmemcpy(&unit_index, payload + at, sizeof(unit_index));
	at += sizeof(unit_index);
	csmemcpy(&position, payload + at, sizeof(position));
	at += sizeof(position);
	key_length = payload[at++];
	csmemcpy(&caption_length, payload + at, sizeof(caption_length));
	at += sizeof(caption_length);
	if (key_length >= sizeof(relay.npc_key) || caption_length >= sizeof(relay.caption) ||
		relay.count > CHASM_RELAY_FRAMES || at + key_length + caption_length + relay.count > length)
	{
		return;
	}
	csmemcpy(relay.npc_key, payload + at, key_length);
	relay.npc_key[key_length] = 0;
	at += key_length;
	csmemcpy(relay.caption, payload + at, caption_length);
	relay.caption[caption_length] = 0;
	at += caption_length;
	csmemcpy(relay.lengths, payload + at, relay.count);
	at += relay.count;
	for (index = 0; index < relay.count; index++)
		bytes = (word)(bytes + relay.lengths[index]);
	if (bytes > sizeof(relay.bytes) || at + bytes > length)
		return;
	csmemcpy(relay.bytes, payload + at, bytes);
	relay.turn = turn;
	chasm_client_play_relay(&relay);
	chasm_bridge.turn = turn;
	chasm_bridge.state = _chasm_state_turn;
	chasm_bridge.speaker_position = position;
	chasm_bridge.relayed_unit_index = unit_index;
	chasm_bridge.relayed_at = system_milliseconds();
	csstrncpy(chasm_bridge.speaker, relay.npc_key, sizeof(chasm_bridge.speaker));
	chasm_bridge.speaker[sizeof(chasm_bridge.speaker) - 1] = 0;
}

/* ---------- reactions */

/* the friends near the players, watched for their deaths */
static struct
{
	unsigned long scanned_at;
	short count;
	struct
	{
		long unit_index;
		char const *name;
		real_point3d position;
	} watched[CHASM_WATCHED];
	/* enemies near the players: how many, and since when it has been so */
	short enemies;
	unsigned long enemies_since;
	unsigned long quiet_since;
	/* each player's (an absolute index's) shields and vehicle, last time */
	real shields[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	long vehicles[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	boolean reset;
} chasm_watch;

/* the living friend (a character) nearest a place, NONE for none */
static long chasm_nearest_friend(
	real_point3d const *position,
	long except_unit_index)
{
	struct object_iterator iterator;
	long best = NONE;
	real best_distance = CHASM_WITNESS_DISTANCE;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while (object_iterator_next(&iterator))
	{
		struct unit_datum *unit = (struct unit_datum *)object_get_and_verify_type(iterator.index, _object_mask_unit);
		real distance;

		if (iterator.index == except_unit_index || unit->unit.player_index != NONE || unit->unit.actor_index == NONE ||
			TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ||
			game_team_is_enemy(_game_team_player, unit->object.owner_team_index))
		{
			continue;
		}
		distance = chasm_distance(&unit->object.position, position);
		if (distance < best_distance && chasm_unit_character(iterator.index))
		{
			best_distance = distance;
			best = iterator.index;
		}
	}
	return best;
}

/* a reaction given, if none was too lately: the witness (NONE: the nearest
friend to the place, else Cortana when cortana) */
static boolean chasm_react(
	char const *narration,
	real_point3d const *place,
	long witness_unit_index,
	boolean cortana,
	long player_index)
{
	unsigned long now = system_milliseconds();
	char line[512];

	if (now - chasm_bridge.reacted_at < (unsigned long)(config_real("chasm.reaction_cooldown") * 1000.0) &&
		chasm_bridge.reacted_at)
	{
		return FALSE;
	}
	if (witness_unit_index == NONE)
		witness_unit_index = chasm_nearest_friend(place, NONE);
	if (witness_unit_index == NONE && !cortana)
		return FALSE;
	_snprintf(line, sizeof(line), "World: %s", narration);
	line[sizeof(line) - 1] = 0;
	chasm_bridge.reacted_at = now;
	chasm_start_turn("", player_index, line, witness_unit_index);
	return TRUE;
}

/* what the characters would remark on, looked for twice a second (the host's
or a single player's game): one reaction at a time at most */
static void chasm_watch_game(
	void)
{
	unsigned long now = system_milliseconds();
	struct data_iterator iterator;
	struct player_datum *player;
	struct object_iterator objects;
	short enemies = 0;
	short index;
	char narration[256];
	boolean reacting = FALSE;
	long nearest_player_index = NONE;
	real_point3d nearest_player_position;
	char enemy_names[64];
	short enemy_kinds = 0;

	if (now - chasm_watch.scanned_at < CHASM_REACTION_SCAN_MS)
		return;
	chasm_watch.scanned_at = now;
	if (!config_boolean("chasm.reactions"))
		return;
	enemy_names[0] = 0;
	csmemset(&nearest_player_position, 0, sizeof(nearest_player_position));
	/* the friends watched: one gone or dead is a death */
	for (index = 0; index < chasm_watch.count && !reacting; index++)
	{
		long unit_index = chasm_watch.watched[index].unit_index;
		struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_unit);

		if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		{
			_snprintf(narration, sizeof(narration), "%s was just killed, right beside you.", chasm_watch.watched[index].name);
			narration[sizeof(narration) - 1] = 0;
			reacting = !chasm_watch.reset && chasm_react(narration, &chasm_watch.watched[index].position, NONE, FALSE, NONE);
		}
	}
	/* (watched again, from the players' surroundings now) */
	chasm_watch.count = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long player_index = iterator.datum_index;
		short absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
		struct unit_datum *unit;
		real shield;
		long vehicle;

		if (player->unit_index == NONE ||
			!(unit = (struct unit_datum *)object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit)))
		{
			continue;
		}
		if (nearest_player_index == NONE)
		{
			nearest_player_index = player_index;
			nearest_player_position = unit->object.position;
		}
		object_iterator_new(&objects, _object_mask_biped, 0);
		while (object_iterator_next(&objects))
		{
			struct unit_datum *other = (struct unit_datum *)object_get_and_verify_type(objects.index, _object_mask_unit);
			struct chasm_character const *character;
			real distance;
			short watched;

			if (other->unit.player_index != NONE || other->unit.actor_index == NONE ||
				TEST_FLAG(other->object.damage_flags, _object_dead_bit))
			{
				continue;
			}
			distance = chasm_distance(&other->object.position, &unit->object.position);
			if (game_team_is_enemy(unit->object.owner_team_index, other->object.owner_team_index))
			{
				if (distance <= CHASM_COMBAT_DISTANCE)
				{
					enemies++;
					character = chasm_unit_character(objects.index);
					if (character && enemy_kinds < 3 && !chasm_contains(enemy_names, character->name))
					{
						_snprintf(enemy_names + strlen(enemy_names), sizeof(enemy_names) - strlen(enemy_names), "%s%ss",
							enemy_kinds ? ", " : "", character->name);
						enemy_names[sizeof(enemy_names) - 1] = 0;
						enemy_kinds++;
					}
				}
				continue;
			}
			if (distance > CHASM_WITNESS_DISTANCE || chasm_watch.count >= CHASM_WATCHED ||
				!(character = chasm_unit_character(objects.index)))
			{
				continue;
			}
			for (watched = 0; watched < chasm_watch.count; watched++)
			{
				if (chasm_watch.watched[watched].unit_index == objects.index)
					break;
			}
			if (watched == chasm_watch.count)
			{
				chasm_watch.watched[watched].unit_index = objects.index;
				chasm_watch.watched[watched].name = character->name;
				chasm_watch.watched[watched].position = other->object.position;
				chasm_watch.count++;
			}
		}
		/* a player's shields down under fire, a player into a vehicle */
		shield = unit->object.shield_vitality;
		vehicle = unit->object.parent_object_index;
		if (absolute >= 0 && absolute < HALO_PORT_MAXIMUM_NETWORK_PLAYERS && !chasm_watch.reset && !reacting)
		{
			char name[32];

			chasm_player_name(player_index, name, sizeof(name));
			if (chasm_watch.shields[absolute] > 0.3f && shield <= 0.0f && chasm_watch.enemies > 0)
			{
				_snprintf(narration, sizeof(narration), "%s's shields just went down under fire.", name);
				narration[sizeof(narration) - 1] = 0;
				reacting = chasm_react(narration, &unit->object.position, NONE, TRUE, player_index);
			}
			else if (chasm_watch.vehicles[absolute] == NONE && vehicle != NONE &&
				object_try_and_get_and_verify_type(vehicle, _object_mask_vehicle))
			{
				_snprintf(narration, sizeof(narration), "%s just climbed into a %s.", name,
					chasm_tag_base_name(object_get(vehicle)->definition_index));
				narration[sizeof(narration) - 1] = 0;
				reacting = chasm_react(narration, &unit->object.position, NONE, FALSE, player_index);
			}
		}
		if (absolute >= 0 && absolute < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		{
			chasm_watch.shields[absolute] = shield;
			chasm_watch.vehicles[absolute] = vehicle;
		}
	}
	/* contact, after a quiet spell; the fight over; a long quiet */
	if (enemies && !chasm_watch.enemies)
	{
		if (!reacting && !chasm_watch.reset && now - chasm_watch.quiet_since > 20000 && nearest_player_index != NONE)
		{
			_snprintf(narration, sizeof(narration), "Covenant contact: %d %s closing in.", (int)enemies,
				enemy_names[0] ? enemy_names : "hostiles");
			narration[sizeof(narration) - 1] = 0;
			reacting = chasm_react(narration, &nearest_player_position, NONE, FALSE, nearest_player_index);
		}
		chasm_watch.enemies_since = now;
	}
	else if (!enemies && chasm_watch.enemies)
	{
		chasm_watch.quiet_since = now;
	}
	if (!enemies && chasm_watch.quiet_since && now - chasm_watch.quiet_since > 6000 &&
		chasm_watch.quiet_since - chasm_watch.enemies_since > 5000 && !reacting && nearest_player_index != NONE)
	{
		reacting = chasm_react("The last of the Covenant nearby has fallen; it is quiet again.", &nearest_player_position,
			NONE, FALSE, nearest_player_index);
		/* (once a fight) */
		chasm_watch.enemies_since = chasm_watch.quiet_since;
	}
	if (!enemies && !reacting && nearest_player_index != NONE && config_real("chasm.idle_chatter") > 0.0 &&
		now - chasm_bridge.talked_at > (unsigned long)(config_real("chasm.idle_chatter") * 1000.0) &&
		now - chasm_watch.quiet_since > 20000)
	{
		/* (a quiet moment: whether anyone breaks it is theirs to choose) */
		if (chasm_react("A quiet moment between fights. Say something in character only if it suits you.",
			&nearest_player_position, NONE, FALSE, nearest_player_index))
		{
			chasm_bridge.talked_at = now;
		}
	}
	if (!enemies && !chasm_watch.quiet_since)
		chasm_watch.quiet_since = now;
	chasm_watch.enemies = enemies;
	chasm_watch.reset = FALSE;
}

static void chasm_handle_event(
	struct chasm_event const *event)
{
	switch (event->type)
	{
	case _chasm_event_heard:
	{
		char name[32];

		if (chasm_bridge.state != _chasm_state_hearing || event->turn != chasm_bridge.turn)
			break;
		if (!event->text[0])
		{
			chasm_subtitle_add(2, NULL, "(nobody caught that)");
			chasm_send_line(_chasm_line_notice, NONE, "(nobody caught that)");
			chasm_bridge.state = _chasm_state_idle;
			break;
		}
		if (chasm_bridge.talker_player_index == NONE || distributed_player_is_local(chasm_bridge.talker_player_index))
			csstrncpy(name, "You", sizeof(name));
		else
			chasm_player_name(chasm_bridge.talker_player_index, name, sizeof(name));
		chasm_subtitle_add(1, name, event->text);
		chasm_send_line(_chasm_line_player, chasm_bridge.talker_player_index, event->text);
		chasm_start_turn(event->text, chasm_bridge.talker_player_index, NULL, NONE);
		break;
	}
	case _chasm_event_subtitle:
		if (event->turn != chasm_bridge.turn)
			break;
		chasm_bridge.answered = TRUE;
		chasm_subtitle_add(0, chasm_bridge.speaker, event->text);
		break;
	case _chasm_event_action:
		if (event->turn != chasm_bridge.turn)
			break;
		if (!chasm_act(event->action_id, chasm_bridge.speaker_unit_index,
			chasm_player_unit_of(chasm_bridge.talker_player_index)))
		{
			platform_log("chasm: %s's action %s not done here", chasm_bridge.speaker, event->action_id);
		}
		break;
	case _chasm_event_error:
		if (event->turn != chasm_bridge.turn)
			break;
		chasm_subtitle_add(2, "chasm", event->text);
		chasm_send_line(_chasm_line_notice, NONE, event->text);
		chasm_bridge.state = _chasm_state_idle;
		break;
	case _chasm_event_done:
		break;
	}
}

/* ---------- drawing */

static short chasm_string_width(
	wchar_t const *string,
	short count)
{
	wchar_t part[CHASM_SUBTITLE_SIZE];
	rectangle2d bounds;
	rectangle2d text_bounds;
	rectangle2d cursor_bounds;
	short index;

	for (index = 0; string[index] && (count == NONE || index < count) && index < (short)NUMBEROF(part) - 1; index++)
		part[index] = string[index];
	part[index] = 0;
	if (!index)
		return 0;
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = SHORT_MAX;
	bounds.y1 = SHORT_MAX;
	draw_unicode_string_compute_bounds(&bounds, part, &text_bounds, &cursor_bounds);
	return (short)cursor_bounds.x0;
}

/* how many characters of text from start fit width (at a space where one
does), and the spaces after them */
static short chasm_wrap(
	wchar_t const *text,
	short width,
	short *skip)
{
	short length, low = 1, high, index;

	*skip = 0;
	for (length = 0; text[length]; length++)
		;
	if (chasm_string_width(text, NONE) <= width)
		return length;
	for (high = length; low < high;)
	{
		short middle = (short)((low + high + 1) / 2);

		if (chasm_string_width(text, middle) <= width)
			low = middle;
		else
			high = (short)(middle - 1);
	}
	for (index = low; index > 0 && text[index] != ' '; index--)
		;
	if (index > 0)
		low = index;
	while (text[low + *skip] == ' ')
		(*skip)++;
	return low;
}

static void chasm_draw_text(
	long font_index,
	rectangle2d const *bounds,
	wchar_t const *text,
	real_argb_color const *color)
{
	real_argb_color shadow;
	rectangle2d shadow_bounds = *bounds;

	shadow.alpha = color->alpha * 0.7f;
	shadow.red = shadow.green = shadow.blue = 0.0f;
	offset_rectangle2d(&shadow_bounds, 1, 1);
	rasterizer_text_set_shadow_color(0x00000001);
	draw_string_set_draw_mode(font_index, NONE, 0 /* left */, 0, &shadow);
	rasterizer_draw_unicode_string(&shadow_bounds, NULL, NULL, 0, text);
	draw_string_set_draw_mode(font_index, NONE, 0 /* left */, 0, color);
	rasterizer_draw_unicode_string(bounds, NULL, NULL, 0, text);
	rasterizer_text_set_shadow_color(0);
}

/* ---------- public code */

void chasm_bridge_update(
	void)
{
	short mode = chasm_mode_now();
	boolean held;
	unsigned long now = system_milliseconds();
	struct chasm_event event;

	chasm_client_update(mode);
	if (mode != chasm_bridge.mode)
	{
		chasm_client_listen(FALSE);
		chasm_bridge.state = _chasm_state_idle;
		chasm_bridge.subtitle_count = 0;
		chasm_bridge.mode = mode;
		chasm_watch.reset = TRUE;
	}
	chasm_bridge.was_enabled = mode != _chasm_mode_off;
	if (mode == _chasm_mode_off)
	{
		chasm_bridge.was_held = FALSE;
		return;
	}
	/* talking to them: controls.talk_to_ai, and alone push to talk too (not
	while the game is paused: its menus) */
	held = (halo_talk_to_ai_held() || (game_connection() == _game_connection_local && halo_push_to_talk_held())) &&
		!console_is_active() && !network_chat_typing() && !game_time_get_paused();
	/* (debug.chasm_say, a co-op client's: a second and a half of the
	microphone, as if held - debug.voice_test makes it a tone) */
	if (mode == _chasm_mode_client && !chasm_bridge.said && game_time_get() > 2 * TICKS_PER_SECOND &&
		config_string("debug.chasm_say")[0])
	{
		if (!chasm_bridge.say_started_at)
			chasm_bridge.say_started_at = now;
		if (now - chasm_bridge.say_started_at < 1500)
			held = TRUE;
		else
			chasm_bridge.said = TRUE;
	}
	if (held && !chasm_bridge.was_held)
	{
		/* (speaking over a character stops it, for everyone) */
		chasm_client_cancel();
		if (mode == _chasm_mode_host)
			chasm_send_line(_chasm_line_stop, NONE, "");
		chasm_client_listen(TRUE);
		chasm_bridge.state = _chasm_state_listening;
		chasm_bridge.utterance++;
		chasm_bridge.uploaded_frames = 0;
	}
	else if (!held && chasm_bridge.was_held && chasm_bridge.state == _chasm_state_listening)
	{
		chasm_client_listen(FALSE);
		if (mode == _chasm_mode_client)
		{
			/* (the rest to the host, which answers with a line) */
			chasm_send_recording(TRUE);
			chasm_bridge.state = chasm_client_recorded_ms() < CHASM_SHORTEST_RECORDING_MS ? _chasm_state_idle :
				_chasm_state_hearing;
			chasm_bridge.asked_at = now;
		}
		else if (chasm_client_recorded_ms() < CHASM_SHORTEST_RECORDING_MS)
		{
			chasm_bridge.state = _chasm_state_idle;
		}
		else
		{
			chasm_bridge.turn = chasm_client_recognize();
			chasm_bridge.state = chasm_bridge.turn ? _chasm_state_hearing : _chasm_state_idle;
			chasm_bridge.talker_player_index = local_player_get_player_index(0);
			chasm_bridge.asked_at = now;
		}
	}
	else if (held && mode == _chasm_mode_client)
	{
		chasm_send_recording(FALSE);
	}
	chasm_bridge.was_held = held;
	if (mode == _chasm_mode_client)
	{
		/* (the voice relayed, from where its speaker stands) */
		chasm_place_voice(&chasm_bridge.speaker_position);
		if (chasm_bridge.relayed_unit_index != NONE && chasm_client_voice_level() > 0.0f)
			chasm_move_mouth(chasm_bridge.relayed_unit_index);
		if (chasm_bridge.state != _chasm_state_listening && chasm_bridge.state != _chasm_state_idle &&
			!chasm_client_busy() && now - chasm_bridge.relayed_at > 1500 &&
			(chasm_bridge.answered || now - chasm_bridge.asked_at > CHASM_CLIENT_WAIT_MS))
		{
			chasm_bridge.state = _chasm_state_idle;
		}
		while (chasm_client_next_event(&event))
		{
			if (event.type == _chasm_event_subtitle)
			{
				chasm_bridge.answered = TRUE;
				chasm_subtitle_add(0, event.npc_key[0] ? event.npc_key : chasm_bridge.speaker, event.text);
			}
		}
		return;
	}
	/* the host's (or a single player's) */
	chasm_client_set_relaying(chasm_coop_host());
	while (chasm_client_next_event(&event))
		chasm_handle_event(&event);
	if (chasm_coop_host())
		chasm_send_relays();
	if (chasm_bridge.state == _chasm_state_turn)
	{
		chasm_update_speaker();
		if (!chasm_client_busy())
			chasm_bridge.state = _chasm_state_idle;
	}
	if (chasm_bridge.state == _chasm_state_idle && !held)
		chasm_watch_game();
	/* (debug.chasm_say and debug.chasm_react: the automated tests' words, as
	if heard, and an event's narration, once a map) */
	if (chasm_bridge.state == _chasm_state_idle && !chasm_bridge.said && game_time_get() > 2 * TICKS_PER_SECOND)
	{
		char const *words = config_string("debug.chasm_say");

		chasm_bridge.said = TRUE;
		if (words[0])
		{
			chasm_subtitle_add(1, "You", words);
			chasm_send_line(_chasm_line_player, local_player_get_player_index(0), words);
			chasm_start_turn(words, local_player_get_player_index(0), NULL, NONE);
		}
	}
	else if (chasm_bridge.state == _chasm_state_idle && !chasm_bridge.reacted && game_time_get() > 4 * TICKS_PER_SECOND)
	{
		char const *narration = config_string("debug.chasm_react");

		chasm_bridge.reacted = TRUE;
		if (narration[0])
		{
			long unit_index = chasm_player_unit();

			chasm_bridge.reacted_at = 0;
			if (unit_index != NONE)
				chasm_react(narration, &object_get(unit_index)->object.position, NONE, TRUE, NONE);
		}
	}
}

void chasm_bridge_draw(
	short local_player_index)
{
	long font_index = hud_get_font_index();
	unsigned long now = system_milliseconds();
	rectangle2d window = render.camera.window_bounds;
	struct font_header *font;
	short line_height, left, right, bottom, rows_drawn = 0;
	short index;
	wchar_t const *status = NULL;
	wchar_t lines[CHASM_SUBTITLES * CHASM_SUBTITLE_ROWS][CHASM_SUBTITLE_SIZE / 2];
	real_argb_color line_colors[CHASM_SUBTITLES * CHASM_SUBTITLE_ROWS];
	short line_count = 0;

	if (!chasm_bridge.was_enabled || local_player_index != 0 || font_index == NONE)
		return;
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	font = font_definition_get(font_index);
	line_height = font->ascending_height + font->descending_height + font->leading_height;
	if (line_height <= 0 || window.x1 <= window.x0)
		return;
	/* (the lines are measured in the HUD's font: the draw mode's) */
	draw_string_set_draw_mode(font_index, NONE, 0 /* left */, 0, global_real_argb_white);
	left = (short)(window.x0 + 2 * line_height);
	right = (short)(window.x0 + (window.x1 - window.x0) * CHASM_COLUMN_PERCENT / 100);
	bottom = (short)(window.y1 - 9 * line_height);
	if (chasm_bridge.state == _chasm_state_listening)
		status = L"Listening...";
	else if (chasm_bridge.state == _chasm_state_hearing)
		status = L"...";
	else if (chasm_bridge.state == _chasm_state_turn && !chasm_bridge.answered && now - chasm_bridge.asked_at > 400)
		status = L"(thinking...)";
	/* the subtitles, oldest first, each "Name: text" wrapped to the column */
	for (index = chasm_bridge.subtitle_count - 1; index >= 0; index--)
	{
		struct chasm_subtitle const *subtitle =
			&chasm_bridge.subtitles[(chasm_bridge.newest_subtitle - index + CHASM_SUBTITLES) % CHASM_SUBTITLES];
		unsigned long age = now - subtitle->time;
		wchar_t whole[CHASM_SUBTITLE_SIZE + 40];
		real_argb_color color;
		short at = 0, length = 0, rows = 0;

		if (age >= subtitle->lasts)
			continue;
		color.alpha = age + CHASM_SUBTITLE_FADE_MS > subtitle->lasts ?
			(real)(subtitle->lasts - age) / CHASM_SUBTITLE_FADE_MS : 1.0f;
		/* (the player's lines pale blue, the characters' white, notices
		amber) */
		color.red = subtitle->kind == 1 ? 0.6f : subtitle->kind == 2 ? 1.0f : 1.0f;
		color.green = subtitle->kind == 1 ? 0.8f : subtitle->kind == 2 ? 0.75f : 1.0f;
		color.blue = subtitle->kind == 1 ? 1.0f : subtitle->kind == 2 ? 0.3f : 1.0f;
		if (subtitle->name[0])
		{
			for (; subtitle->name[length] && length < 30; length++)
				whole[length] = subtitle->name[length];
			whole[length++] = ':';
			whole[length++] = ' ';
		}
		for (at = 0; subtitle->text[at] && length < (short)NUMBEROF(whole) - 1; at++)
			whole[length++] = subtitle->text[at];
		whole[length] = 0;
		at = 0;
		while (whole[at] && rows < CHASM_SUBTITLE_ROWS && line_count < (short)NUMBEROF(lines))
		{
			short skip = 0;
			short count = chasm_wrap(whole + at, (short)(right - left), &skip);
			short copy;

			if (count <= 0)
				break;
			for (copy = 0; copy < count && copy < (short)NUMBEROF(lines[0]) - 1; copy++)
				lines[line_count][copy] = whole[at + copy];
			lines[line_count][copy] = 0;
			line_colors[line_count] = color;
			line_count++;
			rows++;
			at = (short)(at + count + skip);
		}
	}
	/* (from the bottom up: the newest lowest, the status under all) */
	if (status)
	{
		rectangle2d bounds;
		real_argb_color color;

		color.alpha = 0.85f;
		color.red = 0.6f;
		color.green = 0.8f;
		color.blue = 1.0f;
		bounds.x0 = left;
		bounds.x1 = right;
		bounds.y0 = (short)(bottom - line_height);
		bounds.y1 = bottom;
		chasm_draw_text(font_index, &bounds, status, &color);
		rows_drawn++;
	}
	for (index = (short)(line_count - 1); index >= 0; index--)
	{
		rectangle2d bounds;

		bounds.x0 = left;
		bounds.x1 = right;
		bounds.y1 = (short)(bottom - rows_drawn * line_height);
		bounds.y0 = (short)(bounds.y1 - line_height);
		chasm_draw_text(font_index, &bounds, lines[index], &line_colors[index]);
		rows_drawn++;
	}
}

boolean chasm_bridge_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_chasm_audio_up || header.type == _distributed_message_chasm_line_down ||
		header.type == _distributed_message_chasm_voice_down;
}

void chasm_bridge_handle_message(
	long machine_index,
	word const *message,
	word size)
{
	struct distributed_message_header header;
	byte const *payload = (byte const *)message + sizeof(header);
	long length = (long)size - (long)sizeof(header);

	if (!chasm_bridge_handles_message(message, size) || !game_in_progress())
		return;
	csmemcpy(&header, message, sizeof(header));
	switch (header.type)
	{
	case _distributed_message_chasm_audio_up:
		/* (the host's, from a client, with chasm) */
		if (machine_index != NONE && chasm_bridge.mode == _chasm_mode_host && chasm_coop_host())
			chasm_receive_recording(machine_index, payload, length);
		break;
	case _distributed_message_chasm_line_down:
		if (machine_index == NONE && chasm_bridge.mode == _chasm_mode_client)
			chasm_receive_line(payload, length);
		break;
	case _distributed_message_chasm_voice_down:
		if (machine_index == NONE && chasm_bridge.mode == _chasm_mode_client)
			chasm_receive_voice(payload, length);
		break;
	}
}

void chasm_bridge_game_state_event(
	char const *event)
{
	char body[512];
	char const *map;
	char const *kind;

	if (!strcmp(event, "map start") || !strcmp(event, "checkpoint revert"))
	{
		chasm_watch.reset = TRUE;
		chasm_watch.count = 0;
	}
	if (!strcmp(event, "map start"))
	{
		chasm_bridge.said = FALSE;
		chasm_bridge.reacted = FALSE;
		chasm_bridge.say_started_at = 0;
	}
	/* (the host's alone: a client's game state is the host's) */
	if (!config_boolean("chasm.enabled") || !chasm_client_available() || game_engine_running() ||
		game_connection() == _game_connection_network_client)
	{
		return;
	}
	if (!strcmp(event, "checkpoint saved"))
		kind = "checkpoint";
	else if (!strcmp(event, "checkpoint revert"))
		kind = "load";
	else
		return;
	/* (a revert: what the characters were saying is over) */
	if (!strcmp(kind, "load"))
	{
		chasm_client_cancel();
		chasm_send_line(_chasm_line_stop, NONE, "");
		chasm_bridge.state = _chasm_state_idle;
		chasm_bridge.subtitle_count = 0;
	}
	map = chasm_tag_base_name(global_scenario_index);
	_snprintf(body, sizeof(body),
		"{\"event\":\"%s\",\"gameId\":\"" CHASM_GAME_ID "\",\"saveId\":\"%s-checkpoint\",\"saveName\":\"%s checkpoint\"}",
		kind, map, chasm_mission_name(map));
	body[sizeof(body) - 1] = 0;
	chasm_client_post("/api/headless/v1/save-sync/events", body);
}
