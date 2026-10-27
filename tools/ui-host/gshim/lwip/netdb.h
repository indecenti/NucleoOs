// ui-host gallery shim (lwIP): just enough of BSD sockets to COMPILE network apps (declarations only;
// nothing connects in a render). Not Winsock: its macros (s_host, s_addr...) collide with app names.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/time.h>
typedef uint32_t socklen_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct sockaddr_in { uint8_t sin_len, sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct sockaddr_storage { uint8_t s2_len, ss_family; char pad[126]; };
struct addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; socklen_t ai_addrlen; struct sockaddr *ai_addr; char *ai_canonname; struct addrinfo *ai_next; };
struct hostent { char *h_name; char **h_aliases; int h_addrtype, h_length; char **h_addr_list; };
typedef struct { uint32_t bits[2]; } fd_set;
#define FD_ZERO(s) ((s)->bits[0] = (s)->bits[1] = 0)
#define FD_SET(n, s) ((s)->bits[(n) / 32] |= 1u << ((n) % 32))
#define FD_CLR(n, s) ((s)->bits[(n) / 32] &= ~(1u << ((n) % 32)))
#define FD_ISSET(n, s) (((s)->bits[(n) / 32] >> ((n) % 32)) & 1u)
#define AF_INET 2
#define AF_UNSPEC 0
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SOL_SOCKET 0xfff
#define SO_ERROR 0x1007
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_REUSEADDR 0x0004
#define SO_BROADCAST 0x0020
#define SO_KEEPALIVE 0x0008
#define TCP_NODELAY 0x01
#define MSG_DONTWAIT 0x08
#define MSG_PEEK 0x01
#define O_NONBLOCK 1
#define F_GETFL 3
#define F_SETFL 4
#define SHUT_RDWR 2
#define INADDR_ANY 0u
#define INADDR_NONE 0xffffffffu
#ifndef EINPROGRESS
#define EINPROGRESS 119
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifdef __cplusplus
extern "C" {
#endif
int socket(int dom, int t, int p);
int connect(int s, const struct sockaddr *a, socklen_t n);
int bind(int s, const struct sockaddr *a, socklen_t n);
int listen(int s, int n);
int accept(int s, struct sockaddr *a, socklen_t *n);
int send(int s, const void *b, size_t n, int f);
int recv(int s, void *b, size_t n, int f);
int sendto(int s, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t al);
int recvfrom(int s, void *b, size_t n, int f, struct sockaddr *a, socklen_t *al);
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t);
int getsockopt(int s, int l, int o, void *v, socklen_t *n);
int setsockopt(int s, int l, int o, const void *v, socklen_t n);
int shutdown(int s, int how);
int fcntl(int fd, int cmd, ...);
int getaddrinfo(const char *h, const char *s, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *a);
struct hostent *gethostbyname(const char *h);
int inet_aton(const char *cp, struct in_addr *inp);
in_addr_t inet_addr(const char *cp);
char *inet_ntoa(struct in_addr a);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t n);
int inet_pton(int af, const char *src, void *dst);
uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);
#ifdef __cplusplus
}
#endif
