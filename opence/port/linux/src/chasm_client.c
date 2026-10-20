/*
CHASM_CLIENT.C

The platform's side of the AI characters (port/linux/game/chasm_bridge.c
decides who is spoken to, and what they do): a chasm backend
(https://github.com/chasmlol/chasm, chasm.url) gives the game's characters a
mind and a voice. A turn goes as chasm's bridge contract has it
(docs/building-a-bridge-mod.md): the player's recorded words to
/api/headless/v1/speech/recognize, then the character's turn to
/api/game/v1/turn, whose answer streams back as lines of JSON - the reply's
sentences, each a WAV of the character's voice, the action the character
chose, and the reply whole. The requests are made on a thread of this file's,
one at a time; the game's thread takes what they gave back as events
(chasm_client_next_event).

The voices are decoded as they come, resampled to the mix's 48 kHz, and
played one after the other into the game's output (dsound_sdl.c's mix, as
voice chat's are), from where the game's side says the character stands; a
line's subtitle is given when its voice starts.

The microphone is voice chat's (voice_audio.c), used only outside network
games: while push to talk is held its 20 ms frames are kept, at 16 kHz,
for the next recognition. It is kept open in a game (chasm.microphone_warm),
so that the first word is not lost while it opens; nothing is kept or sent
but while the key is held.

chasm finds the game by files in its bridge folder (%LOCALAPPDATA%\chasm\bridge,
CHASM_BRIDGE_ROOT, or chasm.bridge_folder): the heartbeat, rewritten a few
times a second with this process's id; the profile of characters, copied
there from chasm-profile/ in the data folder for chasm to import; and the
bridge's settings (nvbridge.config.json), written if there are none, which
point chasm's actions at this game's action book.

In a co-op game only the host talks to chasm. A client's words are recorded
as Opus frames (chasm_opus.c) that port/linux/game/chasm_bridge.c sends to
the host, which recognizes them as its own (chasm_client_recognize_packets);
and the host keeps each voice it plays as Opus relays too, which the bridge
sends to the clients, which play them as they come (chasm_client_play_relay).

Android and the web have no chasm to talk to: everything here does nothing
there.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "sdl_platform.h"
#include "voice_audio.h"
#include "chasm_client.h"
#include "chasm_opus.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int chasm_json_escape(const char *text, char *buffer, int size)
{
	static const char digits[] = "0123456789abcdef";
	int length = 0;

	if (size <= 0)
		return 0;
	for (; *text; text++)
	{
		unsigned char character = (unsigned char)*text;
		char escaped[8];
		int count = 0;

		if (character == '"' || character == '\\')
		{
			escaped[count++] = '\\';
			escaped[count++] = (char)character;
		}
		else if (character == '\n')
		{
			escaped[count++] = '\\';
			escaped[count++] = 'n';
		}
		else if (character < 0x20)
		{
			escaped[count++] = '\\';
			escaped[count++] = 'u';
			escaped[count++] = '0';
			escaped[count++] = '0';
			escaped[count++] = digits[character >> 4];
			escaped[count++] = digits[character & 15];
		}
		else
		{
			escaped[count++] = (char)character;
		}
		if (length + count >= size)
			break;
		memcpy(buffer + length, escaped, (size_t)count);
		length += count;
	}
	buffer[length] = 0;
	return length;
}

#if defined(HALO_WEB) || defined(HALO_ANDROID)

void chasm_client_update(int mode) { (void)mode; }
int chasm_client_available(void) { return 0; }
int chasm_client_listen(int listen) { (void)listen; return 0; }
int chasm_client_recorded_ms(void) { return 0; }
int chasm_client_recognize(void) { return 0; }
int chasm_client_recognize_packets(const unsigned char *lengths, const unsigned char *bytes, int count)
{ (void)lengths; (void)bytes; (void)count; return 0; }
int chasm_client_take_packets(unsigned char *lengths, unsigned char *bytes, int maximum, int size)
{ (void)lengths; (void)bytes; (void)maximum; (void)size; return 0; }
void chasm_client_set_relaying(int relaying) { (void)relaying; }
int chasm_client_next_relay(struct chasm_relay *relay) { (void)relay; return 0; }
void chasm_client_play_relay(const struct chasm_relay *relay) { (void)relay; }
int chasm_client_send_turn(const char *members) { (void)members; return 0; }
void chasm_client_post(const char *path, const char *body) { (void)path; (void)body; }
void chasm_client_cancel(void) {}
int chasm_client_busy(void) { return 0; }
int chasm_client_next_event(struct chasm_event *event) { (void)event; return 0; }
float chasm_client_voice_level(void) { return 0.0f; }
void chasm_client_voice_place(float gain, float pan) { (void)gain; (void)pan; }
void chasm_client_mix(float *output, unsigned long frames) { (void)output; (void)frames; }

#else

#define CHASM_RATE 48000
/* what is recorded: 16 kHz (each three of the microphone's samples
averaged), the most chasm's recognition is given, and the shortest kept */
#define CHASM_RECORD_RATE 16000
#define CHASM_MAXIMUM_RECORDING_MS 30000
#define CHASM_MAXIMUM_RECORDING (CHASM_RECORD_RATE / 1000 * CHASM_MAXIMUM_RECORDING_MS)
/* events not yet taken, at most (the oldest are dropped) */
#define CHASM_EVENTS 64
#define CHASM_HEARTBEAT_MS 250
/* a request given up when its server says nothing for this long (ms):
chasm's own timeout for a turn is three minutes */
#define CHASM_SILENCE_MS 180000
#define CHASM_GAME_ID "halo-combat-evolved"

#ifdef HALO_WINDOWS
#define CHASM_SEPARATOR "\\"
#else
#define CHASM_SEPARATOR "/"
#endif

enum
{
	_chasm_job_recognize,
	_chasm_job_turn,
	_chasm_job_post,
};

struct chasm_job
{
	int kind;
	int turn;
	char host[128];
	unsigned short port;
	char path[96];
	char *body;
	struct chasm_job *next;
};

/* a stretch of a voice, at 48 kHz */
struct chasm_voice
{
	int turn;
	float *samples;
	int count, read;
	BOOL started, announced;
	char npc_key[64];
	char caption[1024];
	struct chasm_voice *next;
};

/* (chasm_lock) the jobs waiting, the events, the turns */
static pthread_mutex_t chasm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t chasm_wake = PTHREAD_COND_INITIALIZER;
static BOOL chasm_thread_started;
static struct chasm_job *chasm_jobs;
static struct chasm_event chasm_events[CHASM_EVENTS];
static int chasm_event_first, chasm_event_count;
static int chasm_next_turn = 1;
/* turns up to this one are abandoned */
static int chasm_cancelled_turn;
/* the turn the thread is on (0: none) */
static int chasm_running_turn;

/* (chasm_voice_lock: the mixer's too) the voices to play */
static pthread_mutex_t chasm_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static struct chasm_voice *chasm_voices;
static float chasm_voice_gain = 1.0f, chasm_voice_pan = 0.0f;
static float chasm_voice_left, chasm_voice_right;
static volatile float chasm_level;

/* (the game's thread) the recording */
static short *chasm_recording;
static int chasm_recorded;
static BOOL chasm_listening;
static BOOL chasm_microphone_open;
static float chasm_record_sum;
static int chasm_record_phase;

/* (the game's thread) the bridge folder's files */
static char chasm_bridge_folder[1024];
static BOOL chasm_bridge_staged;
static Uint64 chasm_heartbeat_at;
static BOOL chasm_enabled_before;
static int chasm_mode;

/* (the game's thread) a co-op client's recording, Opus, until taken */
#define CHASM_PENDING_PACKETS 100
static struct chasm_opus_encoder *chasm_record_encoder;
static unsigned char chasm_pending_lengths[CHASM_PENDING_PACKETS];
static unsigned char chasm_pending_bytes[CHASM_PENDING_PACKETS][CHASM_OPUS_MAXIMUM_PACKET];
static int chasm_pending_count;

/* (chasm_voice_lock) the host's voices kept for co-op clients, as Opus: each
line's frames, taken a relay at a time (chasm_client_next_relay) */
struct chasm_relay_source
{
	int turn;
	char npc_key[64];
	char caption[CHASM_RELAY_CAPTION_SIZE];
	int count, taken;
	unsigned char *lengths;
	unsigned char *bytes;
	int offset;
	struct chasm_relay_source *next;
};
static BOOL chasm_relaying;
static struct chasm_relay_source *chasm_relays;
/* (the thread's) the relays' encoder */
static struct chasm_opus_encoder *chasm_relay_encoder;
/* (the game's thread, a client) the relays' decoder, and the newest turn
played */
static struct chasm_opus_decoder *chasm_relay_decoder;
static int chasm_relay_turn;

static Uint64 chasm_now(void)
{
	return SDL_GetTicks();
}

/* ---------- JSON, read */

static const char *json_space(const char *text)
{
	while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
		text++;
	return text;
}

/* past a string (text at its opening quote) */
static const char *json_skip_string(const char *text)
{
	text++;
	while (*text && *text != '"')
	{
		if (*text == '\\' && text[1])
			text++;
		text++;
	}
	return *text ? text + 1 : text;
}

/* past any value */
static const char *json_skip_value(const char *text)
{
	int depth = 0;

	text = json_space(text);
	if (*text == '"')
		return json_skip_string(text);
	if (*text != '{' && *text != '[')
	{
		while (*text && *text != ',' && *text != '}' && *text != ']')
			text++;
		return text;
	}
	while (*text)
	{
		if (*text == '"')
		{
			text = json_skip_string(text);
			continue;
		}
		if (*text == '{' || *text == '[')
			depth++;
		else if (*text == '}' || *text == ']')
		{
			if (--depth == 0)
				return text + 1;
		}
		text++;
	}
	return text;
}

/* the value of an object's member, by its path ("audio.data"): its first
character, or NULL */
static const char *json_member(const char *object, const char *path)
{
	const char *dot = strchr(path, '.');
	size_t length = dot ? (size_t)(dot - path) : strlen(path);

	object = json_space(object);
	if (*object != '{')
		return NULL;
	object = json_space(object + 1);
	while (*object == '"')
	{
		const char *name = object + 1;
		const char *value;
		BOOL match;

		object = json_skip_string(object);
		match = (size_t)(object - 1 - name) == length && !memcmp(name, path, length);
		object = json_space(object);
		if (*object != ':')
			return NULL;
		value = json_space(object + 1);
		if (match)
			return dot ? json_member(value, dot + 1) : value;
		object = json_space(json_skip_value(value));
		if (*object != ',')
			return NULL;
		object = json_space(object + 1);
	}
	return NULL;
}

static int json_hex(const char *text)
{
	int value = 0;
	int index;

	for (index = 0; index < 4; index++)
	{
		char digit = text[index];

		value <<= 4;
		if (digit >= '0' && digit <= '9')
			value |= digit - '0';
		else if (digit >= 'a' && digit <= 'f')
			value |= digit - 'a' + 10;
		else if (digit >= 'A' && digit <= 'F')
			value |= digit - 'A' + 10;
		else
			return -1;
	}
	return value;
}

/* a string member's text (UTF-8), unescaped; its length, -1 if it is not
a string */
static int json_string(const char *object, const char *path, char *buffer, int size)
{
	const char *text = json_member(object, path);
	int length = 0;

	if (size > 0)
		buffer[0] = 0;
	if (!text || *text != '"' || size <= 0)
		return -1;
	for (text++; *text && *text != '"'; text++)
	{
		char bytes[4];
		int count = 1;

		bytes[0] = *text;
		if (*text == '\\' && text[1])
		{
			text++;
			switch (*text)
			{
			case 'n': bytes[0] = '\n'; break;
			case 't': bytes[0] = '\t'; break;
			case 'r': bytes[0] = '\r'; break;
			case 'b': case 'f': bytes[0] = ' '; break;
			case 'u':
			{
				int code = json_hex(text + 1);

				if (code < 0)
					break;
				text += 4;
				/* (a surrogate pair's second half joined to the first) */
				if (code >= 0xd800 && code < 0xdc00 && text[1] == '\\' && text[2] == 'u')
				{
					int low = json_hex(text + 3);

					if (low >= 0xdc00 && low < 0xe000)
					{
						code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
						text += 6;
					}
				}
				if (code < 0x80)
					bytes[0] = (char)code;
				else if (code < 0x800)
				{
					bytes[0] = (char)(0xc0 | (code >> 6));
					bytes[1] = (char)(0x80 | (code & 0x3f));
					count = 2;
				}
				else if (code < 0x10000)
				{
					bytes[0] = (char)(0xe0 | (code >> 12));
					bytes[1] = (char)(0x80 | ((code >> 6) & 0x3f));
					bytes[2] = (char)(0x80 | (code & 0x3f));
					count = 3;
				}
				else
				{
					bytes[0] = (char)(0xf0 | (code >> 18));
					bytes[1] = (char)(0x80 | ((code >> 12) & 0x3f));
					bytes[2] = (char)(0x80 | ((code >> 6) & 0x3f));
					bytes[3] = (char)(0x80 | (code & 0x3f));
					count = 4;
				}
				break;
			}
			default: bytes[0] = *text; break;
			}
		}
		if (length + count >= size)
			break;
		memcpy(buffer + length, bytes, (size_t)count);
		length += count;
	}
	buffer[length] = 0;
	return length;
}

/* a number member (or a string holding one: chasm sends its confidences
as "0.81"); 1 if there was one */
static int json_number(const char *object, const char *path, double *value)
{
	const char *text = json_member(object, path);
	char *end;

	if (!text)
		return 0;
	if (*text == '"')
		text++;
	*value = strtod(text, &end);
	return end != text;
}

static int json_true(const char *object, const char *path)
{
	const char *text = json_member(object, path);

	return text && (!strncmp(text, "true", 4) || !strncmp(text, "\"true\"", 6) || !strncmp(text, "1", 1));
}

/* ---------- base64 */

static const char base64_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_value(char digit)
{
	if (digit >= 'A' && digit <= 'Z')
		return digit - 'A';
	if (digit >= 'a' && digit <= 'z')
		return digit - 'a' + 26;
	if (digit >= '0' && digit <= '9')
		return digit - '0' + 52;
	if (digit == '+' || digit == '-')
		return 62;
	if (digit == '/' || digit == '_')
		return 63;
	return -1;
}

/* the bytes of base64 text, up to its closing quote; a malloc'd buffer (its
length in size), or NULL */
static unsigned char *base64_decode(const char *text, int *size)
{
	size_t length = strcspn(text, "\"");
	unsigned char *bytes = (unsigned char *)malloc(length / 4 * 3 + 3);
	unsigned long bits = 0;
	int count = 0, written = 0;
	size_t index;

	if (!bytes)
		return NULL;
	for (index = 0; index < length; index++)
	{
		int value = base64_value(text[index]);

		/* (padding, the escaped slashes, and anything else skipped) */
		if (value < 0)
			continue;
		bits = (bits << 6) | (unsigned long)value;
		if (++count == 4)
		{
			bytes[written++] = (unsigned char)(bits >> 16);
			bytes[written++] = (unsigned char)(bits >> 8);
			bytes[written++] = (unsigned char)bits;
			bits = 0;
			count = 0;
		}
	}
	if (count == 3)
	{
		bytes[written++] = (unsigned char)(bits >> 10);
		bytes[written++] = (unsigned char)(bits >> 2);
	}
	else if (count == 2)
	{
		bytes[written++] = (unsigned char)(bits >> 4);
	}
	*size = written;
	return bytes;
}

/* bytes as base64, NUL terminated: malloc'd, or NULL */
static char *base64_encode(const unsigned char *bytes, int size)
{
	char *text = (char *)malloc((size_t)(size + 2) / 3 * 4 + 1);
	int index, written = 0;

	if (!text)
		return NULL;
	for (index = 0; index + 2 < size; index += 3)
	{
		unsigned long bits = ((unsigned long)bytes[index] << 16) | ((unsigned long)bytes[index + 1] << 8) |
			bytes[index + 2];

		text[written++] = base64_digits[(bits >> 18) & 63];
		text[written++] = base64_digits[(bits >> 12) & 63];
		text[written++] = base64_digits[(bits >> 6) & 63];
		text[written++] = base64_digits[bits & 63];
	}
	if (index < size)
	{
		unsigned long bits = (unsigned long)bytes[index] << 16;

		if (index + 1 < size)
			bits |= (unsigned long)bytes[index + 1] << 8;
		text[written++] = base64_digits[(bits >> 18) & 63];
		text[written++] = base64_digits[(bits >> 12) & 63];
		text[written++] = index + 1 < size ? base64_digits[(bits >> 6) & 63] : '=';
		text[written++] = '=';
	}
	text[written] = 0;
	return text;
}

/* ---------- WAV */

static unsigned long wav_long(const unsigned char *bytes)
{
	return bytes[0] | ((unsigned long)bytes[1] << 8) | ((unsigned long)bytes[2] << 16) |
		((unsigned long)bytes[3] << 24);
}

static unsigned short wav_short(const unsigned char *bytes)
{
	return (unsigned short)(bytes[0] | (bytes[1] << 8));
}

/* a WAV's sound (16-bit or float PCM, any rate, mono or stereo), mixed to
mono and resampled to 48 kHz: malloc'd samples (their number in count), or
NULL */
static float *wav_decode(const unsigned char *bytes, int size, int *count)
{
	unsigned short format = 0, channels = 0, bits = 0;
	unsigned long rate = 0;
	const unsigned char *data = NULL;
	unsigned long data_size = 0;
	unsigned long frames, frame;
	float *source, *samples;
	int offset = 12;
	double step, position;
	int index, total;

	if (size < 12 || memcmp(bytes, "RIFF", 4) || memcmp(bytes + 8, "WAVE", 4))
		return NULL;
	while (offset + 8 <= size)
	{
		unsigned long chunk_size = wav_long(bytes + offset + 4);
		const unsigned char *chunk = bytes + offset + 8;

		/* (a stream's WAV may say its data runs to the end, or past it) */
		if (chunk_size > (unsigned long)(size - offset - 8))
			chunk_size = (unsigned long)(size - offset - 8);
		if (!memcmp(bytes + offset, "fmt ", 4) && chunk_size >= 16)
		{
			format = wav_short(chunk);
			channels = wav_short(chunk + 2);
			rate = wav_long(chunk + 4);
			bits = wav_short(chunk + 14);
			/* (WAVE_FORMAT_EXTENSIBLE: its subformat's first two bytes) */
			if (format == 0xfffe && chunk_size >= 26)
				format = wav_short(chunk + 24);
		}
		else if (!memcmp(bytes + offset, "data", 4))
		{
			data = chunk;
			data_size = chunk_size;
			break;
		}
		offset += 8 + (int)chunk_size + (int)(chunk_size & 1);
	}
	if (!data || !channels || channels > 8 || rate < 4000 || rate > 192000 ||
		!((format == 1 && bits == 16) || (format == 3 && bits == 32)))
	{
		return NULL;
	}
	frames = data_size / (channels * (bits / 8));
	if (!frames)
		return NULL;
	source = (float *)malloc(frames * sizeof(float));
	if (!source)
		return NULL;
	for (frame = 0; frame < frames; frame++)
	{
		float sum = 0.0f;
		int channel;

		for (channel = 0; channel < channels; channel++)
		{
			const unsigned char *sample = data + (frame * channels + (unsigned long)channel) * (bits / 8);

			if (format == 1)
				sum += (float)(short)wav_short(sample) / 32768.0f;
			else
			{
				unsigned long word = wav_long(sample);
				float value;

				memcpy(&value, &word, sizeof(value));
				sum += value;
			}
		}
		source[frame] = sum / (float)channels;
	}
	/* (linear interpolation: the voices are speech, and the mix's limiter
	is after them) */
	step = (double)rate / CHASM_RATE;
	total = (int)((double)frames / step);
	samples = (float *)malloc((size_t)(total > 0 ? total : 1) * sizeof(float));
	if (!samples)
	{
		free(source);
		return NULL;
	}
	for (index = 0, position = 0.0; index < total; index++, position += step)
	{
		unsigned long whole = (unsigned long)position;
		float fraction = (float)(position - (double)whole);
		float next = whole + 1 < frames ? source[whole + 1] : source[whole];

		samples[index] = source[whole] + (next - source[whole]) * fraction;
	}
	free(source);
	*count = total;
	return samples;
}

/* the recording as a WAV (16 kHz, 16-bit, mono): malloc'd, or NULL */
static unsigned char *wav_encode(const short *samples, int count, int *size)
{
	int data_size = count * 2;
	unsigned char *bytes = (unsigned char *)malloc((size_t)(44 + data_size));
	unsigned char *at;
	int index;

	if (!bytes)
		return NULL;
	at = bytes;
#define PUT_LONG(value) do { unsigned long put = (unsigned long)(value); at[0] = (unsigned char)put; \
	at[1] = (unsigned char)(put >> 8); at[2] = (unsigned char)(put >> 16); at[3] = (unsigned char)(put >> 24); \
	at += 4; } while (0)
#define PUT_SHORT(value) do { unsigned put = (unsigned)(value); at[0] = (unsigned char)put; \
	at[1] = (unsigned char)(put >> 8); at += 2; } while (0)
	memcpy(at, "RIFF", 4); at += 4;
	PUT_LONG(36 + data_size);
	memcpy(at, "WAVEfmt ", 8); at += 8;
	PUT_LONG(16);
	PUT_SHORT(1);
	PUT_SHORT(1);
	PUT_LONG(CHASM_RECORD_RATE);
	PUT_LONG(CHASM_RECORD_RATE * 2);
	PUT_SHORT(2);
	PUT_SHORT(16);
	memcpy(at, "data", 4); at += 4;
	PUT_LONG(data_size);
	for (index = 0; index < count; index++)
		PUT_SHORT((unsigned short)samples[index]);
#undef PUT_LONG
#undef PUT_SHORT
	*size = 44 + data_size;
	return bytes;
}

/* ---------- events */

/* (chasm_lock) */
static void chasm_push_locked(const struct chasm_event *event)
{
	if (chasm_event_count == CHASM_EVENTS)
	{
		chasm_event_first = (chasm_event_first + 1) % CHASM_EVENTS;
		chasm_event_count--;
	}
	chasm_events[(chasm_event_first + chasm_event_count) % CHASM_EVENTS] = *event;
	chasm_event_count++;
}

static void chasm_push(int type, int turn, const char *npc_key, const char *text)
{
	struct chasm_event event;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.turn = turn;
	if (npc_key)
		snprintf(event.npc_key, sizeof(event.npc_key), "%s", npc_key);
	if (text)
		snprintf(event.text, sizeof(event.text), "%s", text);
	pthread_mutex_lock(&chasm_lock);
	chasm_push_locked(&event);
	pthread_mutex_unlock(&chasm_lock);
}

static BOOL chasm_turn_cancelled(int turn)
{
	BOOL cancelled;

	pthread_mutex_lock(&chasm_lock);
	cancelled = turn <= chasm_cancelled_turn;
	pthread_mutex_unlock(&chasm_lock);
	return cancelled;
}

int chasm_client_next_event(struct chasm_event *event)
{
	int found = 0;

	pthread_mutex_lock(&chasm_lock);
	if (chasm_event_count)
	{
		*event = chasm_events[chasm_event_first];
		chasm_event_first = (chasm_event_first + 1) % CHASM_EVENTS;
		chasm_event_count--;
		found = 1;
	}
	pthread_mutex_unlock(&chasm_lock);
	return found;
}

/* ---------- the voices */

/* a line's voice as Opus frames, for co-op clients (on the thread: its
encoder's); NULL if it could not be */
static struct chasm_relay_source *chasm_relay_encode(int turn, const char *npc_key, const char *caption,
	const float *samples, int count)
{
	struct chasm_relay_source *source = (struct chasm_relay_source *)calloc(1, sizeof(*source));
	int frames = (count + CHASM_OPUS_FRAME_SAMPLES - 1) / CHASM_OPUS_FRAME_SAMPLES;
	int frame, written = 0;

	if (!chasm_relay_encoder)
		chasm_relay_encoder = chasm_opus_encoder_new(32000);
	if (!source || !chasm_relay_encoder || frames <= 0)
	{
		free(source);
		return NULL;
	}
	source->lengths = (unsigned char *)malloc((size_t)frames);
	source->bytes = (unsigned char *)malloc((size_t)frames * CHASM_OPUS_MAXIMUM_PACKET);
	if (!source->lengths || !source->bytes)
	{
		free(source->lengths);
		free(source->bytes);
		free(source);
		return NULL;
	}
	for (frame = 0; frame < frames; frame++)
	{
		float padded[CHASM_OPUS_FRAME_SAMPLES];
		int first = frame * CHASM_OPUS_FRAME_SAMPLES;
		int take = count - first < CHASM_OPUS_FRAME_SAMPLES ? count - first : CHASM_OPUS_FRAME_SAMPLES;
		int length;

		memset(padded, 0, sizeof(padded));
		memcpy(padded, samples + first, (size_t)take * sizeof(float));
		length = chasm_opus_encode(chasm_relay_encoder, padded, source->bytes + written, CHASM_OPUS_MAXIMUM_PACKET);
		source->lengths[source->count++] = (unsigned char)length;
		written += length;
	}
	source->turn = turn;
	snprintf(source->npc_key, sizeof(source->npc_key), "%s", npc_key);
	snprintf(source->caption, sizeof(source->caption), "%s", caption);
	return source;
}

static void chasm_relay_free(struct chasm_relay_source *source)
{
	free(source->lengths);
	free(source->bytes);
	free(source);
}

static void chasm_voice_add(int turn, const char *npc_key, const char *caption, float *samples, int count)
{
	struct chasm_voice *voice = (struct chasm_voice *)calloc(1, sizeof(*voice));
	struct chasm_relay_source *relay = NULL;
	struct chasm_voice **last;
	struct chasm_relay_source **last_relay;
	BOOL relaying;

	if (!voice)
	{
		free(samples);
		return;
	}
	voice->turn = turn;
	voice->samples = samples;
	voice->count = count;
	snprintf(voice->npc_key, sizeof(voice->npc_key), "%s", npc_key);
	snprintf(voice->caption, sizeof(voice->caption), "%s", caption);
	pthread_mutex_lock(&chasm_voice_lock);
	relaying = chasm_relaying;
	pthread_mutex_unlock(&chasm_voice_lock);
	/* (encoded outside the lock, which the mixer waits on) */
	if (relaying)
		relay = chasm_relay_encode(turn, npc_key, caption, samples, count);
	pthread_mutex_lock(&chasm_voice_lock);
	for (last = &chasm_voices; *last; last = &(*last)->next)
		;
	*last = voice;
	if (relay)
	{
		for (last_relay = &chasm_relays; *last_relay; last_relay = &(*last_relay)->next)
			;
		*last_relay = relay;
	}
	pthread_mutex_unlock(&chasm_voice_lock);
}

/* (chasm_voice_lock) the voices of abandoned turns, and those played and
announced, freed; and the relays of abandoned turns */
static void chasm_voices_prune_locked(int cancelled_turn)
{
	struct chasm_voice **link = &chasm_voices;
	struct chasm_relay_source **relay_link = &chasm_relays;

	while (*link)
	{
		struct chasm_voice *voice = *link;

		if (voice->turn <= cancelled_turn || (voice->read >= voice->count && voice->announced))
		{
			*link = voice->next;
			free(voice->samples);
			free(voice);
			continue;
		}
		link = &voice->next;
	}
	while (*relay_link)
	{
		struct chasm_relay_source *relay = *relay_link;

		if (relay->turn <= cancelled_turn)
		{
			*relay_link = relay->next;
			chasm_relay_free(relay);
			continue;
		}
		relay_link = &relay->next;
	}
}

void chasm_client_set_relaying(int relaying)
{
	pthread_mutex_lock(&chasm_voice_lock);
	chasm_relaying = relaying != 0;
	/* (nobody to send them to: those kept dropped) */
	while (!chasm_relaying && chasm_relays)
	{
		struct chasm_relay_source *relay = chasm_relays;

		chasm_relays = relay->next;
		chasm_relay_free(relay);
	}
	pthread_mutex_unlock(&chasm_voice_lock);
}

int chasm_client_next_relay(struct chasm_relay *relay)
{
	struct chasm_relay_source *source;
	int bytes = 0;

	pthread_mutex_lock(&chasm_voice_lock);
	source = chasm_relays;
	if (!source)
	{
		pthread_mutex_unlock(&chasm_voice_lock);
		return 0;
	}
	relay->turn = source->turn;
	relay->first = source->taken == 0;
	snprintf(relay->npc_key, sizeof(relay->npc_key), "%s", source->npc_key);
	snprintf(relay->caption, sizeof(relay->caption), "%s", relay->first ? source->caption : "");
	relay->count = 0;
	while (source->taken < source->count && relay->count < CHASM_RELAY_FRAMES &&
		bytes + source->lengths[source->taken] <= CHASM_RELAY_BYTES)
	{
		int length = source->lengths[source->taken];

		memcpy(relay->bytes + bytes, source->bytes + source->offset, (size_t)length);
		relay->lengths[relay->count++] = (unsigned char)length;
		bytes += length;
		source->offset += length;
		source->taken++;
	}
	if (source->taken >= source->count)
	{
		chasm_relays = source->next;
		chasm_relay_free(source);
	}
	pthread_mutex_unlock(&chasm_voice_lock);
	return 1;
}

void chasm_client_play_relay(const struct chasm_relay *relay)
{
	float *samples;
	int index, offset = 0, count = 0;
	struct chasm_voice *voice = NULL;

	if (!chasm_relay_decoder)
		chasm_relay_decoder = chasm_opus_decoder_new();
	if (!chasm_relay_decoder || relay->count <= 0 || relay->turn < chasm_relay_turn)
		return;
	samples = (float *)malloc((size_t)relay->count * CHASM_OPUS_FRAME_SAMPLES * sizeof(float));
	if (!samples)
		return;
	for (index = 0; index < relay->count; index++)
	{
		count += chasm_opus_decode(chasm_relay_decoder, relay->bytes + offset, relay->lengths[index], samples + count);
		offset += relay->lengths[index];
	}
	pthread_mutex_lock(&chasm_voice_lock);
	/* (a newer turn's: the older's stop) */
	if (relay->turn > chasm_relay_turn)
	{
		chasm_voices_prune_locked(relay->turn - 1);
		chasm_relay_turn = relay->turn;
	}
	/* (a line's later frames go on the end of its voice, which may be
	playing) */
	if (!relay->first)
	{
		struct chasm_voice *last;

		for (last = chasm_voices; last; last = last->next)
		{
			if (last->turn == relay->turn)
				voice = last;
		}
	}
	if (voice)
	{
		float *grown = (float *)realloc(voice->samples, (size_t)(voice->count + count) * sizeof(float));

		if (grown)
		{
			memcpy(grown + voice->count, samples, (size_t)count * sizeof(float));
			voice->samples = grown;
			voice->count += count;
		}
		pthread_mutex_unlock(&chasm_voice_lock);
		free(samples);
		return;
	}
	pthread_mutex_unlock(&chasm_voice_lock);
	if (count > 0)
		chasm_voice_add(relay->turn, relay->npc_key, relay->caption, samples, count);
	else
		free(samples);
}

float chasm_client_voice_level(void)
{
	return chasm_level;
}

void chasm_client_voice_place(float gain, float pan)
{
	pthread_mutex_lock(&chasm_voice_lock);
	chasm_voice_gain = gain < 0.0f ? 0.0f : gain > 2.0f ? 2.0f : gain;
	chasm_voice_pan = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
	pthread_mutex_unlock(&chasm_voice_lock);
}

void chasm_client_mix(float *output, unsigned long frames)
{
	struct chasm_voice *voice;
	unsigned long frame = 0;
	float angle, left, right, step_left, step_right;
	double sum = 0.0;
	unsigned long summed = 0;

	pthread_mutex_lock(&chasm_voice_lock);
	/* (equal power: both sides at the gain when centred; the gains glide
	to their new values over the chunk) */
	angle = (chasm_voice_pan + 1.0f) * 0.25f * 3.14159265f;
	left = chasm_voice_gain * cosf(angle) * 1.41421356f;
	right = chasm_voice_gain * sinf(angle) * 1.41421356f;
	step_left = frames ? (left - chasm_voice_left) / (float)frames : 0.0f;
	step_right = frames ? (right - chasm_voice_right) / (float)frames : 0.0f;
	for (voice = chasm_voices; voice && frame < frames; voice = voice->next)
	{
		if (voice->read >= voice->count)
			continue;
		voice->started = TRUE;
		while (frame < frames && voice->read < voice->count)
		{
			float sample = voice->samples[voice->read++];

			chasm_voice_left += step_left;
			chasm_voice_right += step_right;
			output[frame * 2] += sample * chasm_voice_left;
			output[frame * 2 + 1] += sample * chasm_voice_right;
			sum += (double)sample * sample;
			summed++;
			frame++;
		}
	}
	chasm_voice_left = left;
	chasm_voice_right = right;
	chasm_level = summed ? (float)sqrt(sum / (double)summed) : 0.0f;
	pthread_mutex_unlock(&chasm_voice_lock);
}

/* ---------- HTTP */

/* chasm.url's host and port ("http://127.0.0.1:7341"); 1 if it is one */
static int chasm_parse_url(const char *url, char *host, size_t host_size, unsigned short *port)
{
	const char *start, *end, *colon;
	long number = 80;

	if (!strncmp(url, "http://", 7))
		start = url + 7;
	else if (strstr(url, "://"))
		return 0;
	else
		start = url;
	end = start + strcspn(start, "/");
	colon = memchr(start, ':', (size_t)(end - start));
	if (colon)
	{
		number = strtol(colon + 1, NULL, 10);
		end = colon;
	}
	if (end == start || (size_t)(end - start) >= host_size || number <= 0 || number > 65535)
		return 0;
	memcpy(host, start, (size_t)(end - start));
	host[end - start] = 0;
	*port = (unsigned short)number;
	return 1;
}

struct chasm_connection
{
	int socket;
	int turn;
	/* the response's bytes not yet taken */
	char buffer[16384];
	int start, end;
	BOOL closed;
};

/* more of the response read into the buffer: 1, or 0 when it ended, failed,
was abandoned or went silent too long */
static int chasm_receive(struct chasm_connection *connection)
{
	Uint64 waited_from = chasm_now();

	if (connection->start > 0)
	{
		memmove(connection->buffer, connection->buffer + connection->start,
			(size_t)(connection->end - connection->start));
		connection->end -= connection->start;
		connection->start = 0;
	}
	if (connection->closed || connection->end == (int)sizeof(connection->buffer))
		return 0;
	for (;;)
	{
		int read_list[1], read_count = 1;
		int result;

		if (connection->turn && chasm_turn_cancelled(connection->turn))
			return 0;
		read_list[0] = connection->socket;
		/* (a tenth of a second at a time, to see an abandoned turn) */
		result = posix_socket_select(read_list, &read_count, NULL, NULL, NULL, NULL, 0, 100000, 0);
		if (result < 0)
			return 0;
		if (result == 0 || !read_count)
		{
			if (chasm_now() - waited_from > CHASM_SILENCE_MS)
				return 0;
			continue;
		}
		result = posix_socket_recv(connection->socket, connection->buffer + connection->end,
			(int)sizeof(connection->buffer) - connection->end, 0);
		if (result <= 0)
		{
			connection->closed = TRUE;
			return 0;
		}
		connection->end += result;
		return 1;
	}
}

/* a line of the response's head, without its CRLF; 0 if it ended */
static int chasm_read_line(struct chasm_connection *connection, char *line, int size)
{
	for (;;)
	{
		char *newline = memchr(connection->buffer + connection->start, '\n',
			(size_t)(connection->end - connection->start));

		if (newline)
		{
			int length = (int)(newline - (connection->buffer + connection->start));

			if (length > 0 && newline[-1] == '\r')
				length--;
			if (length >= size)
				length = size - 1;
			memcpy(line, connection->buffer + connection->start, (size_t)length);
			line[length] = 0;
			connection->start = (int)(newline + 1 - connection->buffer);
			return 1;
		}
		if (!chasm_receive(connection))
			return 0;
	}
}

/* up to size of the body's bytes; how many, 0 at its end */
static int chasm_read_bytes(struct chasm_connection *connection, char *bytes, int size)
{
	int count;

	if (connection->start == connection->end && !chasm_receive(connection))
		return 0;
	count = connection->end - connection->start;
	if (count > size)
		count = size;
	memcpy(bytes, connection->buffer + connection->start, (size_t)count);
	connection->start += count;
	return count;
}

/* what is done with a response's body, a line (of NDJSON) at a time */
typedef void (*chasm_line_handler)(struct chasm_job *job, char *line, void *context);

/* a growing line */
struct chasm_line
{
	char *text;
	int length, size;
};

static void chasm_line_feed(struct chasm_line *line, const char *bytes, int count, struct chasm_job *job,
	chasm_line_handler handler, void *context)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (bytes[index] == '\n')
		{
			if (line->text)
			{
				line->text[line->length] = 0;
				if (line->length)
					handler(job, line->text, context);
			}
			line->length = 0;
			continue;
		}
		if (line->length + 2 > line->size)
		{
			int size = line->size ? line->size * 2 : 4096;
			char *text = (char *)realloc(line->text, (size_t)size);

			if (!text)
				continue;
			line->text = text;
			line->size = size;
		}
		line->text[line->length++] = bytes[index];
	}
}

/* POSTs a job's body, and gives its response's body to the handler a line at
a time; the HTTP status, or 0 if there was none (no server, or abandoned).
error holds the body of a status other than 200 */
static int chasm_request(struct chasm_job *job, chasm_line_handler handler, void *context, char *error,
	int error_size)
{
	struct chasm_connection *connection;
	struct sockaddr_in address;
	unsigned long ip;
	char head[512];
	char line[1024];
	int status = 0;
	long content_length = -1;
	BOOL chunked = FALSE;
	struct chasm_line body_line;
	int body_length = job->body ? (int)strlen(job->body) : 0;
	int sent;

	error[0] = 0;
	memset(&body_line, 0, sizeof(body_line));
	ip = (unsigned long)posix_resolve_ipv4(job->host);
	if (!ip)
	{
		snprintf(error, (size_t)error_size, "cannot find %s", job->host);
		return 0;
	}
	connection = (struct chasm_connection *)calloc(1, sizeof(*connection));
	if (!connection)
		return 0;
	connection->turn = job->kind == _chasm_job_post ? 0 : job->turn;
	connection->socket = posix_socket(AF_INET, SOCK_STREAM, 0);
	if (connection->socket < 0)
	{
		free(connection);
		return 0;
	}
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = (unsigned short)((job->port >> 8) | ((job->port & 0xff) << 8));
	address.sin_addr.s_addr = ip;
	if (posix_socket_connect(connection->socket, &address, sizeof(address)) < 0)
	{
		snprintf(error, (size_t)error_size, "chasm is not running at %s:%u", job->host, (unsigned)job->port);
		posix_socket_close(connection->socket);
		free(connection);
		return 0;
	}
	posix_socket_set_nodelay(connection->socket);
	snprintf(head, sizeof(head),
		"POST %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: OpenCE-chasm-bridge\r\nContent-Type: application/json\r\n"
		"Accept: application/x-ndjson, application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
		job->path, job->host, (unsigned)job->port, body_length);
	sent = posix_socket_send(connection->socket, head, (int)strlen(head), 0) == (int)strlen(head);
	{
		int offset = 0;

		while (sent && offset < body_length)
		{
			int result = posix_socket_send(connection->socket, job->body + offset, body_length - offset, 0);

			if (result <= 0)
				sent = FALSE;
			else
				offset += result;
		}
	}
	/* the head: its status, and how the body comes */
	if (sent && chasm_read_line(connection, line, sizeof(line)) && !strncmp(line, "HTTP/", 5))
	{
		const char *space = strchr(line, ' ');

		status = space ? atoi(space + 1) : 0;
		while (chasm_read_line(connection, line, sizeof(line)) && line[0])
		{
			if (!SDL_strncasecmp(line, "content-length:", 15))
				content_length = strtol(line + 15, NULL, 10);
			else if (!SDL_strncasecmp(line, "transfer-encoding:", 18) && SDL_strcasestr(line + 18, "chunked"))
				chunked = TRUE;
		}
	}
	else if (!error[0])
	{
		snprintf(error, (size_t)error_size, "no answer from chasm");
	}
	/* the body */
	if (status)
	{
		char bytes[4096];
		int error_length = 0;
		long left = content_length;

		for (;;)
		{
			int count;

			if (chunked)
			{
				/* (a chunk's size, then its bytes) */
				if (left <= 0)
				{
					if (left == 0 && !chasm_read_line(connection, line, sizeof(line)))
						break;
					if (!chasm_read_line(connection, line, sizeof(line)))
						break;
					left = strtol(line, NULL, 16);
					if (left <= 0)
						break;
				}
				count = chasm_read_bytes(connection, bytes, left < (long)sizeof(bytes) ? (int)left : (int)sizeof(bytes));
				if (count <= 0)
					break;
				/* (at 0, the CRLF after it is read before the next one's size) */
				left -= count;
			}
			else
			{
				int want = sizeof(bytes);

				if (content_length >= 0)
				{
					if (left <= 0)
						break;
					if (left < want)
						want = (int)left;
				}
				count = chasm_read_bytes(connection, bytes, want);
				if (count <= 0)
					break;
				left -= count;
			}
			if (status == 200)
			{
				chasm_line_feed(&body_line, bytes, count, job, handler, context);
			}
			else if (error_length < error_size - 1)
			{
				int take = count < error_size - 1 - error_length ? count : error_size - 1 - error_length;

				memcpy(error + error_length, bytes, (size_t)take);
				error_length += take;
				error[error_length] = 0;
			}
		}
		/* (a last line without its newline) */
		if (status == 200)
			chasm_line_feed(&body_line, "\n", 1, job, handler, context);
	}
	free(body_line.text);
	posix_socket_close(connection->socket);
	free(connection);
	return status;
}

/* ---------- the requests */

struct chasm_turn_state
{
	int voices;
	BOOL finished;
	BOOL failed;
	char npc_key[64];
};

static void chasm_recognized_line(struct chasm_job *job, char *line, void *context)
{
	char *text = (char *)context;

	(void)job;
	json_string(line, "text", text, 1024);
}

static void chasm_turn_line(struct chasm_job *job, char *line, void *context)
{
	struct chasm_turn_state *state = (struct chasm_turn_state *)context;
	char type[32];
	char text[1024];
	struct chasm_event event;

	if (chasm_turn_cancelled(job->turn) || json_string(line, "type", type, sizeof(type)) < 0)
		return;
	if (!strcmp(type, "audio.chunk"))
	{
		const char *data = json_member(line, "audio.data");
		char npc_key[64];
		unsigned char *bytes;
		int size = 0;

		json_string(line, "text", text, sizeof(text));
		if (json_string(line, "npcKey", npc_key, sizeof(npc_key)) > 0)
			snprintf(state->npc_key, sizeof(state->npc_key), "%s", npc_key);
		if (!data || *data != '"')
			return;
		bytes = base64_decode(data + 1, &size);
		if (bytes)
		{
			int count = 0;
			float *samples = wav_decode(bytes, size, &count);

			free(bytes);
			if (samples && count > 0)
			{
				chasm_voice_add(job->turn, state->npc_key, text, samples, count);
				state->voices++;
			}
			else
			{
				free(samples);
				platform_log("chasm: a voice that is not a WAV this plays (%d bytes)", size);
			}
		}
	}
	else if (!strcmp(type, "action"))
	{
		double confidence = 0.0;

		memset(&event, 0, sizeof(event));
		if (!json_true(line, "shouldTrigger"))
			return;
		event.type = _chasm_event_action;
		event.turn = job->turn;
		json_string(line, "actionId", event.action_id, sizeof(event.action_id));
		if (!event.action_id[0])
			json_string(line, "action", event.action_id, sizeof(event.action_id));
		if (json_string(line, "actor.npcKey", event.npc_key, sizeof(event.npc_key)) <= 0)
			snprintf(event.npc_key, sizeof(event.npc_key), "%s", state->npc_key);
		json_number(line, "confidence", &confidence);
		event.confidence = (float)confidence;
		json_string(line, "reason", event.text, sizeof(event.text));
		platform_log("chasm: %s chose %s (%.2f): %s", event.npc_key, event.action_id, confidence, event.text);
		pthread_mutex_lock(&chasm_lock);
		chasm_push_locked(&event);
		pthread_mutex_unlock(&chasm_lock);
	}
	else if (!strcmp(type, "reply"))
	{
		char error[512];
		char npc_key[64];

		json_string(line, "text", text, sizeof(text));
		json_string(line, "error", error, sizeof(error));
		if (json_string(line, "npcKey", npc_key, sizeof(npc_key)) > 0)
			snprintf(state->npc_key, sizeof(state->npc_key), "%s", npc_key);
		if (error[0])
		{
			state->failed = TRUE;
			chasm_push(_chasm_event_error, job->turn, state->npc_key, error);
			return;
		}
		platform_log("chasm: %s: %s", state->npc_key, text);
		/* (no voice: the line said without one) */
		if (!state->voices && text[0])
			chasm_voice_add(job->turn, state->npc_key, text, (float *)calloc(1, sizeof(float)), 1);
		chasm_push(_chasm_event_done, job->turn, state->npc_key, text);
		state->finished = TRUE;
	}
	else if (!strcmp(type, "turn.error"))
	{
		json_string(line, "error", text, sizeof(text));
		state->failed = TRUE;
		chasm_push(_chasm_event_error, job->turn, state->npc_key, text);
	}
	else if (!strcmp(type, "turn.completed"))
	{
		if (!state->finished && !state->failed)
			chasm_push(_chasm_event_done, job->turn, state->npc_key, "");
		state->finished = TRUE;
	}
}

static void chasm_ignored_line(struct chasm_job *job, char *line, void *context)
{
	(void)job;
	(void)line;
	(void)context;
}

/* chasm's error for a status, from its JSON body ({"error": ...}) or the
body itself */
static void chasm_error_text(int status, const char *body, char *text, int size)
{
	char message[768];

	if (json_string(body, "error", message, sizeof(message)) <= 0 &&
		json_string(body, "error.message", message, sizeof(message)) <= 0 &&
		json_string(body, "message", message, sizeof(message)) <= 0)
	{
		snprintf(message, sizeof(message), "%.700s", body);
	}
	snprintf(text, (size_t)size, "chasm said %d: %s", status, message);
}

static void chasm_run_job(struct chasm_job *job)
{
	char error[1024];
	int status;

	if (job->kind == _chasm_job_recognize)
	{
		char text[1024];

		text[0] = 0;
		status = chasm_request(job, chasm_recognized_line, text, error, sizeof(error));
		if (chasm_turn_cancelled(job->turn))
			return;
		if (status == 200)
		{
			platform_log("chasm: heard \"%s\"", text);
			chasm_push(_chasm_event_heard, job->turn, NULL, text);
		}
		else
		{
			if (status)
				chasm_error_text(status, error, error, sizeof(error));
			platform_log("chasm: speech recognition failed: %s", error);
			chasm_push(_chasm_event_error, job->turn, NULL, error);
		}
	}
	else if (job->kind == _chasm_job_turn)
	{
		struct chasm_turn_state state;

		memset(&state, 0, sizeof(state));
		json_string(job->body, "npc_key", state.npc_key, sizeof(state.npc_key));
		status = chasm_request(job, chasm_turn_line, &state, error, sizeof(error));
		if (chasm_turn_cancelled(job->turn))
			return;
		if (status != 200)
		{
			if (status)
				chasm_error_text(status, error, error, sizeof(error));
			platform_log("chasm: the turn failed: %s", error);
			chasm_push(_chasm_event_error, job->turn, state.npc_key, error);
		}
		else if (!state.finished && !state.failed)
		{
			chasm_push(_chasm_event_error, job->turn, state.npc_key, "the turn ended without a reply");
		}
	}
	else
	{
		status = chasm_request(job, chasm_ignored_line, NULL, error, sizeof(error));
		if (status != 200)
			platform_log("chasm: POST %s: %d %s", job->path, status, error);
	}
}

static void *chasm_thread(void *parameter)
{
	(void)parameter;
	for (;;)
	{
		struct chasm_job *job;

		pthread_mutex_lock(&chasm_lock);
		while (!chasm_jobs)
			pthread_cond_wait(&chasm_wake, &chasm_lock);
		job = chasm_jobs;
		chasm_jobs = job->next;
		/* (a turn abandoned before it started is not made) */
		if (job->kind != _chasm_job_post && job->turn <= chasm_cancelled_turn)
		{
			pthread_mutex_unlock(&chasm_lock);
			free(job->body);
			free(job);
			continue;
		}
		chasm_running_turn = job->kind == _chasm_job_post ? 0 : job->turn;
		pthread_mutex_unlock(&chasm_lock);
		chasm_run_job(job);
		pthread_mutex_lock(&chasm_lock);
		chasm_running_turn = 0;
		pthread_mutex_unlock(&chasm_lock);
		free(job->body);
		free(job);
	}
	return NULL;
}

/* a job queued for the thread (started the first time); its body is taken */
static int chasm_queue(int kind, const char *path, char *body)
{
	struct chasm_job *job = (struct chasm_job *)calloc(1, sizeof(*job));
	struct chasm_job **last;
	int turn = 0;

	if (!job || !chasm_parse_url(config_string("chasm.url"), job->host, sizeof(job->host), &job->port))
	{
		platform_log("chasm: chasm.url is not an http:// address: %s", config_string("chasm.url"));
		free(job);
		free(body);
		return 0;
	}
	job->kind = kind;
	job->body = body;
	snprintf(job->path, sizeof(job->path), "%s", path);
	pthread_mutex_lock(&chasm_lock);
	if (!chasm_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, chasm_thread, NULL) != 0)
		{
			pthread_mutex_unlock(&chasm_lock);
			free(job->body);
			free(job);
			return 0;
		}
		pthread_detach(thread);
		chasm_thread_started = TRUE;
	}
	if (kind != _chasm_job_post)
	{
		/* (a new turn abandons the one before) */
		turn = chasm_next_turn++;
		chasm_cancelled_turn = turn - 1;
		job->turn = turn;
	}
	for (last = &chasm_jobs; *last; last = &(*last)->next)
		;
	*last = job;
	pthread_cond_signal(&chasm_wake);
	pthread_mutex_unlock(&chasm_lock);
	if (kind != _chasm_job_post)
	{
		pthread_mutex_lock(&chasm_voice_lock);
		chasm_voices_prune_locked(turn - 1);
		pthread_mutex_unlock(&chasm_voice_lock);
	}
	return turn ? turn : 1;
}

int chasm_client_available(void)
{
	char host[128];
	unsigned short port;

	return chasm_parse_url(config_string("chasm.url"), host, sizeof(host), &port);
}

/* 16 kHz samples given to chasm's speech recognition */
static int chasm_recognize_samples(const short *samples, int count)
{
	unsigned char *wav;
	char *audio, *body;
	int size = 0;
	size_t length;

	if (!samples || count <= 0)
		return 0;
	wav = wav_encode(samples, count, &size);
	audio = wav ? base64_encode(wav, size) : NULL;
	free(wav);
	if (!audio)
		return 0;
	length = strlen(audio) + 160;
	body = (char *)malloc(length);
	if (!body)
	{
		free(audio);
		return 0;
	}
	snprintf(body, length,
		"{\"audio\":{\"data\":\"%s\",\"encoding\":\"base64\",\"format\":\"wav\",\"mimeType\":\"audio/wav\"},"
		"\"language\":\"en\"}", audio);
	free(audio);
	return chasm_queue(_chasm_job_recognize, "/api/headless/v1/speech/recognize", body);
}

int chasm_client_recognize(void)
{
	return chasm_recognize_samples(chasm_recording, chasm_recorded);
}

int chasm_client_recognize_packets(const unsigned char *lengths, const unsigned char *bytes, int count)
{
	struct chasm_opus_decoder *decoder = chasm_opus_decoder_new();
	short *samples = (short *)malloc((size_t)(count > 0 ? count : 1) * CHASM_OPUS_FRAME_SAMPLES / 3 * sizeof(short));
	int index, offset = 0, recorded = 0, turn = 0;

	if (decoder && samples)
	{
		for (index = 0; index < count; index++)
		{
			float frame[CHASM_OPUS_FRAME_SAMPLES];
			int decoded = chasm_opus_decode(decoder, bytes + offset, lengths[index], frame);
			int sample;

			offset += lengths[index];
			/* (to 16 kHz, each three averaged, as the microphone's are) */
			for (sample = 0; sample + 2 < decoded; sample += 3)
			{
				float value = (frame[sample] + frame[sample + 1] + frame[sample + 2]) / 3.0f;

				value = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
				samples[recorded++] = (short)(value * 32767.0f);
			}
		}
		turn = chasm_recognize_samples(samples, recorded);
	}
	free(samples);
	chasm_opus_decoder_delete(decoder);
	return turn;
}

int chasm_client_send_turn(const char *members)
{
	size_t length = strlen(members) + 3;
	char *body = (char *)malloc(length);

	if (!body)
		return 0;
	snprintf(body, length, "{%s}", members);
	return chasm_queue(_chasm_job_turn, "/api/game/v1/turn", body);
}

void chasm_client_post(const char *path, const char *body)
{
	size_t length = strlen(body) + 1;
	char *copy = (char *)malloc(length);

	if (!copy)
		return;
	memcpy(copy, body, length);
	chasm_queue(_chasm_job_post, path, copy);
}

void chasm_client_cancel(void)
{
	pthread_mutex_lock(&chasm_lock);
	chasm_cancelled_turn = chasm_next_turn - 1;
	pthread_mutex_unlock(&chasm_lock);
	pthread_mutex_lock(&chasm_voice_lock);
	/* (a co-op client's are the host's turns: all of them, and the rest of
	the one playing) */
	if (chasm_mode == _chasm_mode_client)
	{
		chasm_voices_prune_locked(chasm_relay_turn);
		chasm_relay_turn++;
	}
	else
	{
		chasm_voices_prune_locked(chasm_next_turn - 1);
	}
	pthread_mutex_unlock(&chasm_voice_lock);
}

int chasm_client_busy(void)
{
	BOOL busy;

	pthread_mutex_lock(&chasm_lock);
	busy = chasm_running_turn != 0 || (chasm_jobs && chasm_jobs->kind != _chasm_job_post);
	pthread_mutex_unlock(&chasm_lock);
	pthread_mutex_lock(&chasm_voice_lock);
	busy = busy || chasm_voices != NULL;
	pthread_mutex_unlock(&chasm_voice_lock);
	return busy;
}

/* ---------- the microphone */

int chasm_client_listen(int listen)
{
	if (listen && !chasm_listening)
	{
		chasm_recorded = 0;
		chasm_pending_count = 0;
		chasm_record_sum = 0.0f;
		chasm_record_phase = 0;
		if (!chasm_recording)
			chasm_recording = (short *)malloc(CHASM_MAXIMUM_RECORDING * sizeof(short));
	}
	chasm_listening = listen && chasm_recording;
	return chasm_microphone_open;
}

int chasm_client_recorded_ms(void)
{
	return chasm_recorded / (CHASM_RECORD_RATE / 1000);
}

int chasm_client_take_packets(unsigned char *lengths, unsigned char *bytes, int maximum, int size)
{
	int count = 0, written = 0, index;

	while (count < chasm_pending_count && count < maximum && written + chasm_pending_lengths[count] <= size)
	{
		memcpy(bytes + written, chasm_pending_bytes[count], chasm_pending_lengths[count]);
		lengths[count] = chasm_pending_lengths[count];
		written += chasm_pending_lengths[count];
		count++;
	}
	for (index = count; index < chasm_pending_count; index++)
	{
		chasm_pending_lengths[index - count] = chasm_pending_lengths[index];
		memcpy(chasm_pending_bytes[index - count], chasm_pending_bytes[index], chasm_pending_lengths[index]);
	}
	chasm_pending_count -= count;
	return count;
}

/* the microphone's frames: kept while listening, else dropped */
static void chasm_microphone_update(int enabled)
{
	float frame[VOICE_FRAME_SAMPLES];
	BOOL wanted = enabled && (chasm_listening || config_boolean("chasm.microphone_warm"));

	/* (asked every frame while wanted, as voice chat does: it opens on a
	thread of its own, and again if it failed) */
	if (wanted || chasm_microphone_open)
		chasm_microphone_open = voice_audio_microphone(wanted) != 0;
	while (wanted && voice_audio_read_frame(frame))
	{
		int index;

		if (!chasm_listening)
			continue;
		/* (a co-op client's, for the host: Opus) */
		if (chasm_mode == _chasm_mode_client && chasm_recorded < CHASM_MAXIMUM_RECORDING &&
			chasm_pending_count < CHASM_PENDING_PACKETS)
		{
			int length;

			if (!chasm_record_encoder)
				chasm_record_encoder = chasm_opus_encoder_new(24000);
			length = chasm_opus_encode(chasm_record_encoder, frame, chasm_pending_bytes[chasm_pending_count],
				CHASM_OPUS_MAXIMUM_PACKET);
			if (length > 0)
				chasm_pending_lengths[chasm_pending_count++] = (unsigned char)length;
		}
		for (index = 0; index < VOICE_FRAME_SAMPLES; index++)
		{
			chasm_record_sum += frame[index];
			if (++chasm_record_phase < CHASM_RATE / CHASM_RECORD_RATE)
				continue;
			if (chasm_recorded < CHASM_MAXIMUM_RECORDING)
			{
				float sample = chasm_record_sum / (float)(CHASM_RATE / CHASM_RECORD_RATE);

				sample = sample < -1.0f ? -1.0f : sample > 1.0f ? 1.0f : sample;
				chasm_recording[chasm_recorded++] = (short)(sample * 32767.0f);
			}
			chasm_record_sum = 0.0f;
			chasm_record_phase = 0;
		}
	}
}

/* ---------- the bridge folder */

static const char *chasm_bridge_root(void)
{
	if (!chasm_bridge_folder[0])
	{
		const char *setting = config_string("chasm.bridge_folder");
		const char *environment = SDL_getenv("CHASM_BRIDGE_ROOT");

		if (setting[0])
			snprintf(chasm_bridge_folder, sizeof(chasm_bridge_folder), "%s", setting);
		else if (environment && environment[0])
			snprintf(chasm_bridge_folder, sizeof(chasm_bridge_folder), "%s", environment);
		else
		{
			/* (as chasm finds it: %LOCALAPPDATA%, else %APPDATA%, else the
			temporary folder; then chasm/bridge) */
			const char *base = SDL_getenv("LOCALAPPDATA");

			if (!base || !base[0])
				base = SDL_getenv("APPDATA");
			if (!base || !base[0])
				base = SDL_getenv("TMPDIR");
			if (!base || !base[0])
				base = "/tmp";
			snprintf(chasm_bridge_folder, sizeof(chasm_bridge_folder), "%s" CHASM_SEPARATOR "chasm" CHASM_SEPARATOR "bridge",
				base);
		}
	}
	return chasm_bridge_folder;
}

static BOOL chasm_write_file(const char *path, const char *text)
{
	SDL_IOStream *stream = SDL_IOFromFile(path, "wb");
	size_t length = strlen(text);
	BOOL written;

	if (!stream)
		return FALSE;
	written = SDL_WriteIO(stream, text, length) == length;
	return SDL_CloseIO(stream) && written;
}

struct chasm_copy
{
	char from[1024];
	char to[1024];
	int files;
	BOOL failed;
};

static SDL_EnumerationResult SDLCALL chasm_copy_entry(void *context, const char *directory, const char *name)
{
	struct chasm_copy *copy = (struct chasm_copy *)context;
	char from[1024], to[1024];
	size_t from_length = strlen(copy->from), to_length = strlen(copy->to);
	SDL_PathInfo information;

	(void)directory;
	snprintf(from, sizeof(from), "%s" CHASM_SEPARATOR "%s", copy->from, name);
	snprintf(to, sizeof(to), "%s" CHASM_SEPARATOR "%s", copy->to, name);
	if (!SDL_GetPathInfo(from, &information))
		return SDL_ENUM_CONTINUE;
	if (information.type == SDL_PATHTYPE_DIRECTORY)
	{
		/* (into the folder, and back out) */
		if (!SDL_CreateDirectory(to))
		{
			copy->failed = TRUE;
			return SDL_ENUM_CONTINUE;
		}
		snprintf(copy->from, sizeof(copy->from), "%s", from);
		snprintf(copy->to, sizeof(copy->to), "%s", to);
		SDL_EnumerateDirectory(from, chasm_copy_entry, copy);
		copy->from[from_length] = 0;
		copy->to[to_length] = 0;
	}
	else if (information.type == SDL_PATHTYPE_FILE)
	{
		if (SDL_CopyFile(from, to))
			copy->files++;
		else
			copy->failed = TRUE;
	}
	return SDL_ENUM_CONTINUE;
}

/* the profile (chasm-profile/<game> in the data folder) copied into the
bridge folder for chasm to import, and the bridge's settings written if
there are none (or they are this game's) */
static void chasm_stage_bridge(void)
{
	const char *root = chasm_bridge_root();
	struct chasm_copy copy;
	char path[1024];
	char *existing;
	size_t existing_size;
	char text[2048];

	chasm_bridge_staged = TRUE;
	snprintf(path, sizeof(path), "%s" CHASM_SEPARATOR "chasm-profile" CHASM_SEPARATOR CHASM_GAME_ID, root);
	if (!SDL_CreateDirectory(path))
	{
		platform_log("chasm: cannot make the bridge folder %s (%s)", path, SDL_GetError());
		return;
	}
	memset(&copy, 0, sizeof(copy));
	snprintf(copy.from, sizeof(copy.from), "%s" CHASM_SEPARATOR "chasm-profile" CHASM_SEPARATOR CHASM_GAME_ID,
		platform_data_root());
	snprintf(copy.to, sizeof(copy.to), "%s", path);
	if (SDL_GetPathInfo(copy.from, NULL))
	{
		SDL_EnumerateDirectory(copy.from, chasm_copy_entry, &copy);
		platform_log("chasm: the profile's %d files copied to %s%s", copy.files, path,
			copy.failed ? " (some could not be)" : "");
	}
	else
	{
		platform_log("chasm: no profile at %s: chasm uses the one it has", copy.from);
	}
	snprintf(path, sizeof(path), "%s" CHASM_SEPARATOR "nvbridge.config.json", root);
	existing = config_file_read(path, &existing_size);
	if (existing && !strstr(existing, "\"" CHASM_GAME_ID "\""))
	{
		platform_log("chasm: %s is another game's: its actions are not this game's until chasm's Settings > "
			"Bridge points at settings of this game's", path);
	}
	else
	{
		char url[256];

		chasm_json_escape(config_string("chasm.url"), url, sizeof(url));
		snprintf(text, sizeof(text),
			"{\n"
			"  \"comment\": \"Written by OpenCE's chasm bridge (" CHASM_GAME_ID "); point chasm's Settings > Bridge > "
			"helper config at this file so that its turns use Halo's action book.\",\n"
			"  \"apiBase\": \"%s/api/headless/v1\",\n"
			"  \"liveChatId\": \"halo-ce\",\n"
			"  \"groupId\": \"halo-ce\",\n"
			"  \"participantId\": \"player\",\n"
			"  \"characterId\": \"Marine\",\n"
			"  \"characterName\": \"Marine\",\n"
			"  \"adminCharacterId\": \"Cortana\",\n"
			"  \"adminCharacterName\": \"Cortana\",\n"
			"  \"nativeMaxDistanceMeters\": %.1f,\n"
			"  \"gameStateRadiusMeters\": 40.0,\n"
			"  \"enableActionBooks\": true,\n"
			"  \"actionBookTargetGame\": \"" CHASM_GAME_ID "\",\n"
			"  \"actionBookIds\": [\"Halo Combat Evolved Action Book\"],\n"
			"  \"nativeActionConfidence\": 0.6\n"
			"}\n",
			url, config_real("chasm.distance"));
		if (!existing || strcmp(existing, text))
		{
			if (chasm_write_file(path, text))
				platform_log("chasm: bridge settings written to %s", path);
			else
				platform_log("chasm: cannot write %s", path);
		}
	}
	free(existing);
}

static void chasm_write_heartbeat(void)
{
	char path[1024];
	char text[256];

	snprintf(path, sizeof(path), "%s" CHASM_SEPARATOR "runtime_heartbeat.json", chasm_bridge_root());
	snprintf(text, sizeof(text), "{\"pid\":%lu,\"game\":\"" CHASM_GAME_ID "\",\"plugin\":\"opence-chasm-bridge\",\"ticks\":%lu}\n",
		(unsigned long)posix_process_id(), (unsigned long)chasm_now());
	chasm_write_file(path, text);
}

void chasm_client_update(int mode)
{
	Uint64 now = chasm_now();

	/* (another mode: what the last one left stopped) */
	if (mode != chasm_mode && chasm_enabled_before)
		chasm_client_cancel();
	chasm_mode = mode;
	chasm_microphone_update(mode != _chasm_mode_off);
	if (mode == _chasm_mode_off)
	{
		chasm_listening = FALSE;
		chasm_enabled_before = FALSE;
		chasm_client_set_relaying(FALSE);
		return;
	}
	chasm_enabled_before = TRUE;
	/* (the host alone is chasm's game) */
	if (mode == _chasm_mode_host)
	{
		if (!chasm_bridge_staged)
			chasm_stage_bridge();
		if (now - chasm_heartbeat_at >= CHASM_HEARTBEAT_MS)
		{
			chasm_heartbeat_at = now;
			chasm_write_heartbeat();
		}
	}
	/* the lines whose voices started given as subtitles; those played
	freed */
	{
		struct chasm_event events[8];
		int count = 0, index;
		struct chasm_voice *voice;
		int cancelled;

		pthread_mutex_lock(&chasm_lock);
		cancelled = chasm_cancelled_turn;
		pthread_mutex_unlock(&chasm_lock);
		pthread_mutex_lock(&chasm_voice_lock);
		for (voice = chasm_voices; voice && count < 8; voice = voice->next)
		{
			if (!voice->started || voice->announced || voice->turn <= cancelled)
				continue;
			voice->announced = TRUE;
			memset(&events[count], 0, sizeof(events[count]));
			events[count].type = _chasm_event_subtitle;
			events[count].turn = voice->turn;
			snprintf(events[count].npc_key, sizeof(events[count].npc_key), "%s", voice->npc_key);
			snprintf(events[count].text, sizeof(events[count].text), "%s", voice->caption);
			count++;
		}
		chasm_voices_prune_locked(cancelled);
		pthread_mutex_unlock(&chasm_voice_lock);
		for (index = 0; index < count; index++)
		{
			if (!events[index].text[0])
				continue;
			pthread_mutex_lock(&chasm_lock);
			chasm_push_locked(&events[index]);
			pthread_mutex_unlock(&chasm_lock);
		}
	}
}

#endif
