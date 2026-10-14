#pragma once

/* Linux-compatible TCP socket option names used by portable clients.  The
 * System socket layer can reject options it does not implement with
 * ENOPROTOOPT; keeping the declarations here lets the common networking code
 * compile without importing the host libc's ABI. */
#define SOL_TCP 6
#define TCP_NODELAY 1
#define TCP_MAXSEG 2
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define TCP_SYNCNT 7
#define TCP_LINGER2 8
#define TCP_DEFER_ACCEPT 9
#define TCP_WINDOW_CLAMP 10
#define TCP_QUICKACK 12
#define TCP_CONGESTION 13
#define TCP_CORK 3
#define TCP_USER_TIMEOUT 18
