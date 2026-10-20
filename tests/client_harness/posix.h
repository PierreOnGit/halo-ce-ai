#ifndef HARNESS_POSIX_H
#define HARNESS_POSIX_H
typedef long posix_long;
typedef unsigned long posix_ulong;
posix_ulong posix_resolve_ipv4(const char *host);
int posix_socket(int family, int type, int protocol);
int posix_socket_close(int socket);
int posix_socket_connect(int socket, const void *address, int address_length);
int posix_socket_send(int socket, const void *buffer, int length, int flags);
int posix_socket_recv(int socket, void *buffer, int length, int flags);
int posix_socket_set_nodelay(int socket);
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite);
posix_ulong posix_process_id(void);
#endif
