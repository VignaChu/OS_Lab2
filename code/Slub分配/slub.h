#pragma once
#include <defs.h>
#include <string.h>

/* freestanding: 自补定宽整数（以防头文件裁剪） */
#ifndef __SLUB_STDINT_FALLBACK__
#define __SLUB_STDINT_FALLBACK__
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
#endif

/* 对外接口 */
void  slub_init(void);
void* slub_alloc(size_t n);
void  slub_free(void *p);

static inline void *slub_zalloc(size_t n) {
    void *p = slub_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* 统计与自检 */
void slub_dump_stats(int verbose);
int  slub_check_invariants(int fatal);

/* 让全局 kmalloc/kfree 指到 SLUB */
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void  kfree(void *p);
