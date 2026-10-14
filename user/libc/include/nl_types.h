#pragma once

/* System has a single built-in C/POSIX locale and no message catalogs.  Keep
 * the POSIX surface available so libc++'s messages facet can compile; the
 * runtime returns the caller's default message. */
typedef int nl_catd;

#define NL_CAT_LOCALE 1

#ifdef __cplusplus
extern "C" {
#endif
nl_catd catopen(const char *name, int oflag);
char *catgets(nl_catd catalog, int set_id, int message_id,
              const char *default_message);
int catclose(nl_catd catalog);
#ifdef __cplusplus
}
#endif
