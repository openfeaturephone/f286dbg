/* app_rt.h - tiny runtime for position-independent SD binaries. */
#ifndef APP_RT_H
#define APP_RT_H

#include <stdint.h>
#include "f286_api.h"

/* Header: magic, then the entry offset. The linker script places .f286hdr at 0 and
 * .text.entry right after it, so the entry is always at offset 8 (build.py checks this). */
#define F286_APP_HEADER(fn) \
    __attribute__((section(".f286hdr"), used)) const uint32_t f286_header[2] = { F286_APP_MAGIC, 8 }; \
    void fn(const f286_api_t *, void *, uint32_t) __attribute__((section(".text.entry")))

/* Writable state without RW relocations: the block lives in the (writable) loaded
 * image and is reached PC-relatively like any read-only object. The asm hides its
 * address from the optimiser so stores are not folded away as writes to const. */
#define APP_STATE(type) \
    __attribute__((section(".rodata.state"), aligned(8))) const type app_state_blob; \
    static inline type *STATE(void) { type *p; __asm__("" : "=r"(p) : "0"(&app_state_blob)); return p; }

static inline void zero(void *p, uint32_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }
static inline uint16_t wlen(const uint16_t *s) { uint16_t n = 0; while (s[n]) n++; return n; }

#endif
