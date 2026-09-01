#pragma once

#include <liteos/libc.h>

#define BUFSIZ 4096U
#define FOPEN_MAX 32
#define FILENAME_MAX 256
#define L_tmpnam 64

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define getc(stream) fgetc(stream)
#define getchar() fgetc(stdin)
#define putc(value, stream) fputc((value), (stream))
#define putchar(value) fputc((value), stdout)

typedef off_t fpos_t;

int sprintf(char *buffer, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list arguments);
int vasprintf(char **buffer, const char *format, va_list arguments);
int asprintf(char **buffer, const char *format, ...);
int vdprintf(int descriptor, const char *format, va_list arguments);
int dprintf(int descriptor, const char *format, ...);
int vscanf(const char *format, va_list arguments);
int scanf(const char *format, ...);
int vfscanf(FILE *stream, const char *format, va_list arguments);
int fscanf(FILE *stream, const char *format, ...);
int vsscanf(const char *text, const char *format, va_list arguments);
int sscanf(const char *text, const char *format, ...);
int fwide(FILE *stream, int mode);
wint_t fgetwc(FILE *stream);
wint_t fputwc(wchar_t value, FILE *stream);
wchar_t *fgetws(wchar_t *buffer, int capacity, FILE *stream);
int fputws(const wchar_t *text, FILE *stream);
wint_t getwc(FILE *stream);
wint_t putwc(wchar_t value, FILE *stream);
wint_t getwchar(void);
wint_t putwchar(wchar_t value);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *position);
int fsetpos(FILE *stream, const fpos_t *position);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int ungetc(int value, FILE *stream);
int fileno(FILE *stream);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
void perror(const char *prefix);
int remove(const char *path);
int rename(const char *old_path, const char *new_path);
FILE *tmpfile(void);
char *tmpnam(char *buffer);
FILE *fopen64(const char *path, const char *mode);
FILE *freopen64(const char *path, const char *mode, FILE *stream);
ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *stream);
ssize_t getline(char **line, size_t *capacity, FILE *stream);
int getc_unlocked(FILE *stream);
int getchar_unlocked(void);
int putc_unlocked(int value, FILE *stream);
int putchar_unlocked(int value);
char *fgets_unlocked(char *buffer, int capacity, FILE *stream);
int fputs_unlocked(const char *text, FILE *stream);
int fputc_unlocked(int value, FILE *stream);
size_t fread_unlocked(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite_unlocked(const void *buffer, size_t size, size_t count,
                       FILE *stream);
int fflush_unlocked(FILE *stream);
