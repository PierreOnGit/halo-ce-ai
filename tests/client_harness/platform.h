/* tests/client_harness: the few names of OpenCE's platform layer that
chasm_client.c uses, for running it outside the game */
#ifndef HARNESS_PLATFORM_H
#define HARNESS_PLATFORM_H
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *platform_data_root(void);
#endif
