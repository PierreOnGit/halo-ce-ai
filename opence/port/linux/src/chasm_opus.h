/*
CHASM_OPUS.H

Opus for the AI characters' co-op (chasm_opus.c): 20 ms frames at 48 kHz,
mono. tests/client_harness has a stand-in of these.
*/

#ifndef CHASM_OPUS_H
#define CHASM_OPUS_H

#define CHASM_OPUS_FRAME_SAMPLES 960
/* the longest packet: one length in a byte */
#define CHASM_OPUS_MAXIMUM_PACKET 255

struct chasm_opus_encoder;
struct chasm_opus_decoder;

/* bitrate: bits a second */
struct chasm_opus_encoder *chasm_opus_encoder_new(int bitrate);
/* a frame (CHASM_OPUS_FRAME_SAMPLES) encoded; its packet's length, 0 for none */
int chasm_opus_encode(struct chasm_opus_encoder *encoder, const float *frame, unsigned char *packet, int maximum);
void chasm_opus_encoder_delete(struct chasm_opus_encoder *encoder);
struct chasm_opus_decoder *chasm_opus_decoder_new(void);
/* a packet decoded into a frame; its samples, 0 if it was not a frame */
int chasm_opus_decode(struct chasm_opus_decoder *decoder, const unsigned char *packet, int length, float *frame);
void chasm_opus_decoder_delete(struct chasm_opus_decoder *decoder);

#endif
