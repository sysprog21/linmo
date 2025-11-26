#pragma once

/* Compact C standard library
 *
 * Provides essential C library functions for embedded systems including
 * string manipulation, memory management, character classification,
 * and basic I/O operations.
 */

#include <hal.h>
#include <stdarg.h> /* For va_list in vsnprintf declaration */

/* Basic Type Definitions */
#ifndef NULL
#define NULL ((void *) 0) /* Standard NULL definition */
#endif

typedef _Bool bool;
#define false ((bool) 0)
#define true ((bool) 1)

/* Utility Macros */
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

/* Character Classification Macros */
#define isprint(c) (' ' <= (c) && (c) <= '~') /* Printable character */
#define isspace(c) \
    ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r') /* Whitespace */
#define isdigit(c) ('0' <= (c) && (c) <= '9') /* Decimal digit */
#define islower(c) ('a' <= (c) && (c) <= 'z') /* Lowercase letter */
#define isupper(c) ('A' <= (c) && (c) <= 'Z') /* Uppercase letter */
#define isalpha(c) (islower(c) || isupper(c)) /* Alphabetic character */
#define isalnum(c) (isalpha(c) || isdigit(c)) /* Alphanumeric character */

/* Endianness Conversion Macros */
#if __BYTE_ORDER == __BIG_ENDIAN
/* Big-endian: no conversion needed */
#define htons(n) (n)
#define ntohs(n) (n)
#define htonl(n) (n)
#define ntohl(n) (n)

#else /* __LITTLE_ENDIAN */
/* Little-endian: byte swapping required */

/* 16-bit byte swap */
#define htons(n) \
    (((((uint16_t) (n) & 0x00FF)) << 8) | (((uint16_t) (n) & 0xFF00) >> 8))
#define ntohs(n) \
    (((((uint16_t) (n) & 0x00FF)) << 8) | (((uint16_t) (n) & 0xFF00) >> 8))

/* 32-bit byte swap */
#define htonl(n)                               \
    (((((uint32_t) (n) & 0x000000FF)) << 24) | \
     ((((uint32_t) (n) & 0x0000FF00)) << 8) |  \
     ((((uint32_t) (n) & 0x00FF0000)) >> 8) |  \
     ((((uint32_t) (n) & 0xFF000000)) >> 24))

#define ntohl(n)                               \
    (((((uint32_t) (n) & 0x000000FF)) << 24) | \
     ((((uint32_t) (n) & 0x0000FF00)) << 8) |  \
     ((((uint32_t) (n) & 0x00FF0000)) >> 8) |  \
     ((((uint32_t) (n) & 0xFF000000)) >> 24))

/* 64-bit byte swap - relies on 32-bit htonl/ntohl */
#define htonll(x)                                                         \
    ((1 == htonl(1))                                                      \
         ? (x)                                                            \
         : (((uint64_t) htonl((uint32_t) ((x) & 0xFFFFFFFFULL))) << 32) | \
               htonl((uint32_t) (((x) >> 32) & 0xFFFFFFFFULL)))

#define ntohll(x)                                                         \
    ((1 == ntohl(1))                                                      \
         ? (x)                                                            \
         : (((uint64_t) ntohl((uint32_t) ((x) & 0xFFFFFFFFULL))) << 32) | \
               ntohl((uint32_t) (((x) >> 32) & 0xFFFFFFFFULL)))

#endif /* __BYTE_ORDER */

/* String Manipulation Functions */

/* String copying and concatenation */
char *strcpy(char *s1, const char *s2);
char *strncpy(char *s1, const char *s2, int32_t n);
char *strcat(char *s1, const char *s2);
char *strncat(char *s1, const char *s2, int32_t n);

/* String comparison */
int32_t strcmp(const char *s1, const char *s2);
int32_t strncmp(const char *s1, const char *s2, int32_t n);

/* String searching */
char *strstr(const char *s1, const char *s2);
char *strchr(const char *s1, int32_t c);
char *strpbrk(const char *s1, const char *s2);

/* String length */
size_t strlen(const char *s1);

/* String tokenization */
char *strsep(char **pp, const char *delim);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **holder);

/* Memory Manipulation Functions */

/* Memory copying and moving */
void *memcpy(void *dst, const void *src, uint32_t n);
void *memmove(void *dst, const void *src, uint32_t n);

/* Memory comparison and initialization */
int32_t memcmp(const void *cs, const void *ct, uint32_t n);
void *memset(void *s, int32_t c, uint32_t n);

/* Mathematical Functions */
int32_t abs(int32_t n); /* Absolute value */

/* String to Number Conversion */
int32_t strtol(const char *s, char **end, int32_t base);
int32_t atoi(const char *s);
void itoa(int32_t i, char *s, int32_t base); /* Integer to ASCII conversion */

/* Random Number Generation */

#ifndef RAND_MAX
#define RAND_MAX 32767U /* Default max value for random() */
#endif

/* Simple random number generator */
int32_t random(void);
void srand(uint32_t seed);

/* POSIX-style re-entrant random number generator */
struct random_data {
    uint32_t state; /* State must never be zero */
};
int random_r(struct random_data *buf, int32_t *result);

/* Input/Output Functions */

/* Character and string output */
int32_t puts(const char *str);
int _putchar(int c); /* Low-level character output (used by logger) */

/* Character and string input */
int getchar(void);
char *gets(char *s);
char *fgets(char *s, int n, void *f);
char *getline(char *s);

/* Formatted output */
int32_t printf(const char *fmt, ...);
int32_t snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list args);

/* User mode safe formatted output.
 * Similar to printf but safe for U-mode tasks - formats in user space
 * then uses syscall to output, avoiding privilege violations.
 */
int32_t umode_printf(const char *fmt, ...);
