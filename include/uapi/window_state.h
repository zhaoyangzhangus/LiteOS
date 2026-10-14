#pragma once

/* Linux window command extension: width= minimized (0/1), other payload
 * fields zero. Kept separate from the general Linux syscall-number header. */
#define LINUX_WINDOW_OPERATION_MINIMIZE 6U
/* Exclusive pointer capture: width=1 acquire, width=0 release; all other
 * command fields zero. Kept here to avoid a general syscall ABI rebuild. */
#define LINUX_WINDOW_OPERATION_CAPTURE 7U
/* Fixed 32-byte NUL-terminated title at address; length=32. */
#define LINUX_WINDOW_OPERATION_TITLE 8U
/* width=1 compositor frame, width=0 client frame; other payload fields zero. */
#define LINUX_WINDOW_OPERATION_DECORATIONS 9U
/* Output x/y=client screen origin, width/height=client size,
 * stride state bits: bit 0 maximized, bit 1 fullscreen.
 * All input payload fields must be zero. */
#define LINUX_WINDOW_OPERATION_GEOMETRY 10U
/* width=1 maximize, width=0 restore; other payload fields zero. */
#define LINUX_WINDOW_OPERATION_MAXIMIZE 11U
/* width=1 enter fullscreen, width=0 restore; other payload fields zero. */
#define LINUX_WINDOW_OPERATION_FULLSCREEN 12U
/* Begin an interactive move (width=0) or resize (width=edge bits:
 * left=1, right=2, top=4, bottom=8). Requires a delivered, still-held left
 * press on an EXPLICIT_MOVE_RESIZE window. Other payload fields zero. */
#define LINUX_WINDOW_OPERATION_INTERACTIVE 13U
