#ifndef UTILS_H
#define UTILS_H

#include "common.h"

/* Create a UDP socket */
int create_udp_socket(void);

/* Set socket to non-blocking mode */
int set_nonblocking(int fd);

/* Send a message with header */
int send_msg(int fd, const struct sockaddr_in *addr, uint16_t type, uint32_t seq, const void *body, uint32_t body_len);

/* Receive a message, parse header and body */
int recv_msg(int fd, struct sockaddr_in *addr, struct msg_header *hdr, void *body, uint32_t *body_len);

/* Print address for debugging */
void print_addr(const struct sockaddr_in *addr);

/* Convert IP string to network byte order */
uint32_t ip_str_to_uint32(const char *ip_str);

/* Convert network byte order to IP string */
void ip_uint32_to_str(uint32_t ip, char *buf, size_t len);

/* Get current timestamp in seconds */
time_t get_timestamp(void);

/* Sleep for milliseconds */
void sleep_ms(int ms);

#endif /* UTILS_H */