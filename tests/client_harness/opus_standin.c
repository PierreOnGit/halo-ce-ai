/*
OPUS_STANDIN.C

tests/client_harness's stand-in for chasm_opus.c, which the game builds
with OpenCE's Opus: a frame (20 ms at 48 kHz) kept as 240 bytes, each the
average of four samples in 8 bits. Crude, and enough to carry the co-op's
recordings and relays through chasm_client.c in the harness.
*/

#include "../../opence/port/linux/src/chasm_opus.h"

#include <stdlib.h>

struct chasm_opus_encoder { int bitrate; };
struct chasm_opus_decoder { int unused; };

#define STANDIN_BYTES (CHASM_OPUS_FRAME_SAMPLES / 4)

struct chasm_opus_encoder *chasm_opus_encoder_new(int bitrate)
{
	struct chasm_opus_encoder *encoder = (struct chasm_opus_encoder *)malloc(sizeof(*encoder));

	if (encoder)
		encoder->bitrate = bitrate;
	return encoder;
}

int chasm_opus_encode(struct chasm_opus_encoder *encoder, const float *frame, unsigned char *packet, int maximum)
{
	int index;

	if (!encoder || maximum < STANDIN_BYTES)
		return 0;
	for (index = 0; index < STANDIN_BYTES; index++)
	{
		float value = (frame[index * 4] + frame[index * 4 + 1] + frame[index * 4 + 2] + frame[index * 4 + 3]) / 4.0f;

		value = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
		packet[index] = (unsigned char)(int)(value * 127.0f + 128.0f);
	}
	return STANDIN_BYTES;
}

void chasm_opus_encoder_delete(struct chasm_opus_encoder *encoder)
{
	free(encoder);
}

struct chasm_opus_decoder *chasm_opus_decoder_new(void)
{
	return (struct chasm_opus_decoder *)malloc(sizeof(struct chasm_opus_decoder));
}

int chasm_opus_decode(struct chasm_opus_decoder *decoder, const unsigned char *packet, int length, float *frame)
{
	int index;

	if (!decoder || length != STANDIN_BYTES)
		return 0;
	for (index = 0; index < CHASM_OPUS_FRAME_SAMPLES; index++)
		frame[index] = ((float)packet[index / 4] - 128.0f) / 127.0f;
	return CHASM_OPUS_FRAME_SAMPLES;
}

void chasm_opus_decoder_delete(struct chasm_opus_decoder *decoder)
{
	free(decoder);
}
