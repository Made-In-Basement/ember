/* A very small C library for Ember NX32 programs.  Every standard header
   in this include directory maps onto this one file. */
#ifndef NANOLIBC_H
#define NANOLIBC_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

typedef long off_t;
typedef int ssize_t;
typedef unsigned int mode_t;
typedef long time_t;

#define EOF (-1)
#define RAND_MAX 0x7fffffff
#define MAXINT ((int)0x7fffffff)
#define MININT ((int)0x80000000)
#define MAXLONG MAXINT
#define MINLONG MININT
#define MAXSHORT ((short)0x7fff)
#define MINSHORT ((short)0x8000)

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_BINARY 0
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRGRP 040
#define S_IROTH 04
#define ENOENT 2
#define EINVAL 22
extern int errno;
struct stat { long st_size; };

/* string / memory */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);

/* ctype */
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int isprint(int c); int isxdigit(int c);
int toupper(int c); int tolower(int c);

/* stdlib */
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void  free(void *p);
int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **end, int base);
int   abs(int x);
long  labs(long x);
void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
char *getenv(const char *name);
int   rand(void);
void  srand(unsigned seed);
void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
#define alloca(n) __builtin_alloca(n)

/* stdio */
typedef struct nano_file FILE;
extern FILE *stdin, *stdout, *stderr;
FILE *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
int    feof(FILE *f);
int    fgetc(FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
int    fflush(FILE *f);
int    puts(const char *s);
int    putchar(int c);
#define getc(f) fgetc(f)
#define putc(c, f) fputc(c, f)
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);

/* unistd / fcntl */
/* open()/close() are not declared here: Doom has enum members of those names */
int  read(int fd, void *buf, size_t n);
int  write(int fd, const void *buf, size_t n);
long lseek(int fd, long off, int whence);
int  access(const char *path, int mode);
int  mkdir(const char *path, mode_t mode);
int  unlink(const char *path);
int  fstat(int fd, struct stat *st);
unsigned sleep(unsigned s);
int  usleep(unsigned us);

/* math (x87) */
double fabs(double x);
double floor(double x);
double sqrt(double x);
double pow(double x, double y);
/* the float forms C99 code reaches for, and the two the x87 does itself */
float fabsf(float x);
float floorf(float x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float fmodf(float x, float y);
/* isfinite without <math.h>: a NaN is unequal to itself, and an infinity
   minus itself is a NaN rather than zero. */
#ifndef isfinite
#define isfinite(x) ((x) == (x) && (x) - (x) == 0)
#endif

#endif
