#ifndef HARNESS_PORT_CONFIG_H
#define HARNESS_PORT_CONFIG_H
#include <stddef.h>
int config_boolean(const char *name);
double config_real(const char *name);
const char *config_string(const char *name);
char *config_file_read(const char *path, size_t *size);
#endif
