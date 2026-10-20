/*
CHASM_OPUS.C

Opus for the AI characters' co-op (chasm_client.c): a client's recorded
words, and the characters' voices the host sends on, travel the game's
connection as 20 ms frames at 48 kHz, mono, encoded as voice chat's are
(port/third_party/opus, voice_audio.c), each machine with coders of its own
so voice chat's are never disturbed.
*/

#include "chasm_opus.h"

#if defined(HALO_WEB) || defined(HALO_ANDROID)

struct chasm_opus_encoder *chasm_opus_encoder_new(int bitrate) { (void)bitrate; return 0; }
int chasm_opus_encode(struct chasm_opus_encoder *encoder, const float *frame, unsigned char *packet, int maximum)
{ (void)encoder; (void)frame; (void)packet; (void)maximum; return 0; }
void chasm_opus_encoder_delete(struct chasm_opus_encoder *encoder) { (void)encoder; }
struct chasm_opus_decoder *chasm_opus_decoder_new(void) { return 0; }
int chasm_opus_decode(struct chasm_opus_decoder *decoder, const unsigned char *packet, int length, float *frame)
{ (void)decoder; (void)packet; (void)length; (void)frame; return 0; }
void chasm_opus_decoder_delete(struct chasm_opus_decoder *decoder) { (void)decoder; }

#else

#include "../../third_party/opus/include/opus.h"

#include <stdlib.h>

#define CHASM_OPUS_RATE 48000

struct chasm_opus_encoder *chasm_opus_encoder_new(int bitrate)
{
	int error = 0;
	OpusEncoder *encoder = opus_encoder_create(CHASM_OPUS_RATE, 1, OPUS_APPLICATION_VOIP, &error);

	if (!encoder)
		return 0;
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
	opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	return (struct chasm_opus_encoder *)encoder;
}

int chasm_opus_encode(struct chasm_opus_encoder *encoder, const float *frame, unsigned char *packet, int maximum)
{
	int length;

	if (!encoder)
		return 0;
	length = opus_encode_float((OpusEncoder *)encoder, frame, CHASM_OPUS_FRAME_SAMPLES, packet, maximum);
	return length > 0 ? length : 0;
}

void chasm_opus_encoder_delete(struct chasm_opus_encoder *encoder)
{
	if (encoder)
		opus_encoder_destroy((OpusEncoder *)encoder);
}

struct chasm_opus_decoder *chasm_opus_decoder_new(void)
{
	int error = 0;

	return (struct chasm_opus_decoder *)opus_decoder_create(CHASM_OPUS_RATE, 1, &error);
}

int chasm_opus_decode(struct chasm_opus_decoder *decoder, const unsigned char *packet, int length, float *frame)
{
	int samples;

	if (!decoder || length <= 0)
		return 0;
	/* (every packet is a frame of 20 ms: a damaged one is dropped) */
	if (opus_packet_get_nb_samples(packet, length, CHASM_OPUS_RATE) != CHASM_OPUS_FRAME_SAMPLES)
		return 0;
	samples = opus_decode_float((OpusDecoder *)decoder, packet, length, frame, CHASM_OPUS_FRAME_SAMPLES, 0);
	return samples > 0 ? samples : 0;
}

void chasm_opus_decoder_delete(struct chasm_opus_decoder *decoder)
{
	if (decoder)
		opus_decoder_destroy((OpusDecoder *)decoder);
}

#endif
