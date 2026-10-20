#ifndef HARNESS_VOICE_AUDIO_H
#define HARNESS_VOICE_AUDIO_H
#define VOICE_FRAME_SAMPLES 960
int voice_audio_microphone(int open);
int voice_audio_read_frame(float *frame);
#endif
