/*
HARNESS.C

Runs OpenCE's chasm_client.c (opence/port/linux/src) outside the game, as
chasm_bridge.c drives it: the bridge folder staged and the heartbeat
written, push to talk held over a microphone that says a tone, what was
heard, a character's turn to it, the voice mixed as dsound_sdl.c mixes it,
and a checkpoint told. Each event is printed; the exit status is 0 if the
whole turn came back (heard, a subtitle with voice, the action, the end).

    harness <data folder holding chasm-profile/> [turn members JSON]

chasm.url is CHASM_URL (default http://127.0.0.1:7341); the bridge folder
is CHASM_BRIDGE_ROOT, as chasm's own.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "voice_audio.h"
#include "../../opence/port/linux/src/chasm_client.h"

#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <math.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *data_root;

/* ---------- the platform's stand-ins */

void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fprintf(stderr, "  [log] ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

const char *platform_data_root(void)
{
	return data_root;
}

int config_boolean(const char *name)
{
	if (!strcmp(name, "chasm.microphone_warm"))
		return 0;
	return 1;
}

double config_real(const char *name)
{
	(void)name;
	return 12.0;
}

const char *config_string(const char *name)
{
	if (!strcmp(name, "chasm.url"))
		return getenv("CHASM_URL") ? getenv("CHASM_URL") : "http://127.0.0.1:7341";
	return "";
}

char *config_file_read(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	long length;
	char *text;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = (char *)malloc((size_t)length + 1);
	if (text && fread(text, 1, (size_t)length, file) != (size_t)length)
		length = 0;
	if (text)
		text[length] = 0;
	fclose(file);
	/* (as port_config.c's, which always writes it) */
	*size = (size_t)length;
	return text;
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct addrinfo hints, *results;
	posix_ulong address = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(host, NULL, &hints, &results) != 0)
		return 0;
	if (results && results->ai_addr)
		address = ((struct sockaddr_in *)results->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(results);
	return address;
}

int posix_socket(int family, int type, int protocol) { return socket(family, type, protocol); }
int posix_socket_close(int socket) { return close(socket); }
int posix_socket_connect(int socket, const void *address, int length) { return connect(socket, address, (socklen_t)length); }
int posix_socket_send(int socket, const void *buffer, int length, int flags) { return (int)send(socket, buffer, (size_t)length, flags | MSG_NOSIGNAL); }
int posix_socket_recv(int socket, void *buffer, int length, int flags) { return (int)recv(socket, buffer, (size_t)length, flags); }
posix_ulong posix_process_id(void) { return (posix_ulong)getpid(); }

int posix_socket_set_nodelay(int socket)
{
	int on = 1;

	return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	struct pollfd poller;
	int result;

	(void)write; (void)write_count; (void)error; (void)error_count;
	poller.fd = read[0];
	poller.events = POLLIN;
	poller.revents = 0;
	result = poll(&poller, 1, infinite ? -1 : (int)(timeout_seconds * 1000 + timeout_microseconds / 1000));
	*read_count = result > 0 && (poller.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
	return result;
}

/* the microphone: a voice-like tone, a frame each 20 ms of real time */
static int microphone_open;
static Uint64 microphone_opened_at;
static unsigned long microphone_frames;

int voice_audio_microphone(int open)
{
	if (open && !microphone_open)
	{
		microphone_opened_at = SDL_GetTicks();
		microphone_frames = 0;
	}
	microphone_open = open;
	return open;
}

int voice_audio_read_frame(float *frame)
{
	int index;

	if (!microphone_open || (SDL_GetTicks() - microphone_opened_at) / 20 <= microphone_frames)
		return 0;
	for (index = 0; index < VOICE_FRAME_SAMPLES; index++)
	{
		double t = (double)(microphone_frames * VOICE_FRAME_SAMPLES + (unsigned long)index) / 48000.0;

		frame[index] = (float)(0.3 * sin(2.0 * 3.14159265 * 220.0 * t) * (0.6 + 0.4 * sin(2.0 * 3.14159265 * 3.0 * t)));
	}
	microphone_frames++;
	return 1;
}

/* ---------- the test */

static const char *event_names[] = { "none", "heard", "subtitle", "action", "done", "error" };

struct results
{
	int heard, subtitles, actions, done, errors;
	char action[96];
	long mixed;
};

/* frames: a game's frame of updates, the mix pulled as the audio thread
would (10 ms at 48 kHz) */
static int mode = _chasm_mode_host;

static void run(int milliseconds, int until_idle, struct results *results, int *last_event)
{
	Uint64 start = SDL_GetTicks();

	while (SDL_GetTicks() - start < (Uint64)milliseconds)
	{
		struct chasm_event event;
		float mix[480 * 2];
		int index;

		chasm_client_update(mode);
		while (chasm_client_next_event(&event))
		{
			printf("  %6.2fs event %-8s turn %d npc \"%s\" %s%s%s\n", (double)SDL_GetTicks() / 1000.0,
				event_names[event.type], event.turn, event.npc_key,
				event.action_id[0] ? event.action_id : "", event.action_id[0] ? " " : "", event.text);
			*last_event = event.type;
			switch (event.type)
			{
			case _chasm_event_heard: results->heard++; break;
			case _chasm_event_subtitle: results->subtitles++; break;
			case _chasm_event_action:
				results->actions++;
				/* (the first: the order's, not a later reaction's) */
				if (!results->action[0])
					snprintf(results->action, sizeof(results->action), "%s", event.action_id);
				break;
			case _chasm_event_done: results->done++; break;
			case _chasm_event_error: results->errors++; break;
			}
		}
		memset(mix, 0, sizeof(mix));
		chasm_client_mix(mix, 480);
		for (index = 0; index < 480 * 2; index++)
		{
			if (mix[index] != 0.0f)
			{
				results->mixed++;
				break;
			}
		}
		/* (a line's subtitle comes as its voice starts: after the turn's end,
		for the last ones) */
		if (until_idle && results->done + results->errors > 0 && !chasm_client_busy())
		{
			printf("  %6.2fs idle\n", (double)SDL_GetTicks() / 1000.0);
			return;
		}
		SDL_Delay(10);
	}
}

int main(int argc, char **argv)
{
	struct results results;
	int last_event = 0;
	int turn;
	/* the host's relays of the turn's voices, kept for the client's part */
	static struct chasm_relay relays[64];
	int relay_count = 0, relay_frames = 0;
	int relayed_heard = 0, relayed_subtitles = 0, reacted = 0;
	const char *members = argc > 2 ? argv[2] :
		"\"request_id\":\"harness_1\",\"npc_key\":\"Sergeant Johnson\",\"npc_name\":\"Sergeant Johnson\","
		"\"player_text\":\"Johnson, follow me.\",\"want_tts\":true,"
		"\"location\":{\"cell\":\"a30\",\"worldspace\":\"Halo\",\"region\":\"Halo\",\"major\":\"Halo\",\"minor\":\"a30\"},"
		"\"metadata\":{\"targeting\":{\"nearby_npcs\":["
		"{\"npc_key\":\"Sergeant Johnson\",\"npc_name\":\"Sergeant Johnson\",\"characterId\":\"Sergeant Johnson\",\"distance_m\":3.20,\"under_crosshair\":true,\"hostile\":false},"
		"{\"npc_key\":\"Bisenti\",\"npc_name\":\"Bisenti\",\"characterId\":\"Bisenti\",\"distance_m\":6.10,\"under_crosshair\":false,\"hostile\":false}]},"
		"\"combat_with\":[],\"in_combat\":false,\"npc_state\":{\"teammate\":true,\"in_combat\":false},"
		"\"macros\":{\"player_name\":\"Master Chief\",\"major_location\":\"Halo\",\"health\":\"100%\",\"weapon\":\"assault rifle\",\"difficulty\":\"Heroic\"},"
		"\"gamestate\":{\"player_name\":\"Master Chief\",\"major_location\":\"Halo\",\"health\":\"100%\",\"weapon\":\"assault rifle\",\"difficulty\":\"Heroic\"}}";
	const char *reaction =
		"\"request_id\":\"harness_reaction\",\"npc_key\":\"Bisenti\",\"npc_name\":\"Bisenti\",\"player_text\":\"\","
		"\"want_tts\":true,\"location\":{\"major\":\"Halo\"},"
		"\"metadata\":{\"reaction\":{\"narration\":\"World: Mendoza was just killed, right beside you.\"},"
		"\"targeting\":{\"nearby_npcs\":["
		"{\"npc_key\":\"Bisenti\",\"npc_name\":\"Bisenti\",\"characterId\":\"Bisenti\",\"distance_m\":2.0,\"under_crosshair\":true}]}}";

	if (argc < 2)
	{
		fprintf(stderr, "usage: harness <data folder> [turn members]\n");
		return 2;
	}
	data_root = argv[1];
	SDL_Init(0);
	memset(&results, 0, sizeof(results));
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("1. the bridge folder staged, the heartbeat written (5 s for chasm to connect)\n");
	run(5000, 0, &results, &last_event);

	printf("2. push to talk held 1.5 s\n");
	chasm_client_listen(1);
	run(1500, 0, &results, &last_event);
	chasm_client_listen(0);
	printf("  recorded %d ms\n", chasm_client_recorded_ms());

	printf("3. speech recognition\n");
	turn = chasm_client_recognize();
	printf("  turn %d\n", turn);
	last_event = 0;
	{
		Uint64 start = SDL_GetTicks();

		while (!results.heard && !results.errors && SDL_GetTicks() - start < 60000)
			run(100, 0, &results, &last_event);
	}

	printf("4. the character's turn (its voices kept for co-op clients too)\n");
	chasm_client_set_relaying(1);
	turn = chasm_client_send_turn(members);
	printf("  turn %d\n", turn);
	last_event = 0;
	{
		Uint64 start = SDL_GetTicks();

		while (SDL_GetTicks() - start < 180000)
		{
			run(20, 0, &results, &last_event);
			while (relay_count < 64 && chasm_client_next_relay(&relays[relay_count]))
			{
				relay_frames += relays[relay_count].count;
				if (relays[relay_count].first)
					printf("  relay: \"%s\" (%s)\n", relays[relay_count].caption, relays[relay_count].npc_key);
				relay_count++;
			}
			if (results.done + results.errors > 0 && !chasm_client_busy())
				break;
		}
	}
	chasm_client_set_relaying(0);
	printf("  mixed %ld chunks of voice (10 ms each); %d relays, %d frames\n", results.mixed, relay_count, relay_frames);

	printf("5. a co-op client's words: recorded as Opus, recognized by the host\n");
	mode = _chasm_mode_client;
	chasm_client_listen(1);
	run(1500, 0, &results, &last_event);
	chasm_client_listen(0);
	{
		/* (as the bridge sends them: a dozen frames a message, which the host
		puts back end to end) */
		static unsigned char lengths[2000], packed[2000 * 255];
		unsigned char chunk_lengths[12], chunk_bytes[12 * 255];
		int count = 0, size = 0, taken;
		int heard_before = results.heard;

		while ((taken = chasm_client_take_packets(chunk_lengths, chunk_bytes, 12, (int)sizeof(chunk_bytes))) > 0)
		{
			int index, bytes = 0;

			for (index = 0; index < taken; index++)
				bytes += chunk_lengths[index];
			memcpy(lengths + count, chunk_lengths, (size_t)taken);
			memcpy(packed + size, chunk_bytes, (size_t)bytes);
			count += taken;
			size += bytes;
		}
		printf("  %d frames (%d bytes) from the client\n", count, size);
		/* (the host's, which this machine is again first: another mode stops
		what was running) */
		mode = _chasm_mode_host;
		run(50, 0, &results, &last_event);
		turn = chasm_client_recognize_packets(lengths, packed, count);
		printf("  turn %d\n", turn);
		{
			Uint64 start = SDL_GetTicks();

			while (results.heard == heard_before && !results.errors && SDL_GetTicks() - start < 60000)
				run(100, 0, &results, &last_event);
		}
		relayed_heard = results.heard > heard_before;
	}

	printf("6. the relays played on a co-op client\n");
	{
		int subtitles_before = results.subtitles;
		long mixed_before = results.mixed;
		int index;

		mode = _chasm_mode_client;
		run(100, 0, &results, &last_event);
		for (index = 0; index < relay_count; index++)
		{
			relays[index].turn += 1000;
			chasm_client_play_relay(&relays[index]);
		}
		run(4000, 0, &results, &last_event);
		relayed_subtitles = results.subtitles - subtitles_before;
		printf("  subtitles %d, voice %ld\n", relayed_subtitles, results.mixed - mixed_before);
		mode = _chasm_mode_host;
		run(100, 0, &results, &last_event);
	}

	printf("7. a reaction turn (nobody spoke: a friend was killed)\n");
	{
		int done_before = results.done;

		turn = chasm_client_send_turn(reaction);
		printf("  turn %d\n", turn);
		run(180000, 1, &results, &last_event);
		reacted = results.done > done_before;
		/* (the harness's run stops at the first done: the voice played out) */
		run(1500, 0, &results, &last_event);
	}

	printf("8. a checkpoint told\n");
	chasm_client_post("/api/headless/v1/save-sync/events",
		"{\"event\":\"checkpoint\",\"gameId\":\"halo-combat-evolved\",\"saveId\":\"a30-checkpoint\",\"saveName\":\"Halo checkpoint\"}");
	run(1500, 0, &results, &last_event);

	printf("co-op: client heard %d, relays %d, relayed subtitles %d; reaction %d\n", relayed_heard, relay_count,
		relayed_subtitles, reacted);
	printf("heard %d, subtitles %d, actions %d (%s), done %d, errors %d, voice %ld\n", results.heard,
		results.subtitles, results.actions, results.action, results.done, results.errors, results.mixed);
	return results.heard && results.subtitles && results.actions && results.done && !results.errors &&
		results.mixed > 0 && relayed_heard && relay_count > 0 && relayed_subtitles > 0 && reacted ? 0 : 1;
}
