#include <defs.h>
#include <stdio.h>
#include <string.h>
#include "../mm/slub.h"
#include "../mm/pmm.h"

static void fill(void *p, size_t n, uint8_t v){ memset(p, v, n); }

/* —— 把大数组放到 BSS，避免内核栈爆 —— */
static void *g_a[2000];
static void *g_b[1000];

/* T1: 基础正确性（覆盖所有 class） */
static void test_basic(void){
    cprintf("[T1] basic begin\n");
    size_t classes[] = {8,16,32,64,128,256,512,1024,2048};
    for(int i=0;i<9;++i){
        size_t n = classes[i];
        const int CNT = 3 * 64;
        void *ptr[CNT];
        for(int k=0;k<CNT;++k){
            ptr[k]=kmalloc(n);
            assert(ptr[k] && (((uintptr_t)ptr[k] & 7u)==0));
            fill(ptr[k], n, 0xA5);
        }
        for(int k=0;k<CNT;++k) kfree(ptr[k]);
    }
    slub_check_invariants(1);
    cprintf("[T1] basic ok\n");
}

/* T2: 大块路径（>2KB 走页） */
static void test_big(void){
    cprintf("[T2] big begin\n");
    size_t sizes[] = { 2049, 3000, 4096, 6000, 8191, 16384 };
    void *p[16]={0};
    for(int i=0;i<6;++i){
        p[i]=kmalloc(sizes[i]);
        assert(p[i]);
        ((uint8_t*)p[i])[0]=0x5A;
    }
    for(int i=0;i<6;++i) kfree(p[i]);
    slub_check_invariants(1);
    cprintf("[T2] big ok\n");
}

/* T3: 内碎片快照（128 vs 129）*/
static void test_fragmentation_snapshot(void){
    cprintf("[T3] frag snapshot begin\n");
    slub_dump_stats(0);                // 初始

    /* 分配 2000 个 128B（落到 128-class） */
    for(int i=0;i<2000;++i){
        g_a[i]=kmalloc(128);
        assert(g_a[i]!=NULL);
    }
    slub_dump_stats(0);                // 纯 128B 占用

    /* 释放一半（形成 partial） */
    for(int i=0;i<2000; i+=2) { kfree(g_a[i]); g_a[i]=NULL; }
    slub_dump_stats(1);                // 查看 partial 细节

    /* 再分配 1000 个 129B（跨到 256-class） */
    for(int i=0;i<1000;++i){
        g_b[i]=kmalloc(129);
        assert(g_b[i]!=NULL);
    }
    slub_dump_stats(0);                // 128+256 混合

    /* 全部回收并校验 */
    for(int i=0;i<2000;++i) if(g_a[i]) kfree(g_a[i]), g_a[i]=NULL;
    for(int i=0;i<1000;++i)            kfree(g_b[i]), g_b[i]=NULL;

    slub_check_invariants(1);
    cprintf("[T3] frag snapshot ok\n");
}

/* T4: 可视化“碎片→再填充→回收”
   选 3 个典型大小，各做：alloc N -> 释放 1/3 -> 再 alloc N/3 -> 全回收 -> 打印统计 */
static void stage_pattern(size_t sz, int N){
    cprintf("  [T4] pattern size=%u begin\n",(unsigned)sz);
    void *v[300];                      // 300*8=2400B，放栈没问题
    int n = (N<=300?N:300);

    for(int i=0;i<n;++i){ v[i]=kmalloc(sz); assert(v[i]); }
    slub_dump_stats(0);

    /* 释放每 3 个里的第 1 个，制造空洞（partial） */
    for(int i=0;i<n;i+=3){ kfree(v[i]); v[i]=NULL; }
    slub_dump_stats(1);

    /* 把空洞再填上 N/3 个，观察 partial→full 变化 */
    int need = n/3, filled=0;
    for(int i=0;i<n && filled<need;++i){
        if(v[i]==NULL){ v[i]=kmalloc(sz); assert(v[i]); ++filled; }
    }
    slub_dump_stats(0);

    /* 全回收 */
    for(int i=0;i<n;++i) if(v[i]) kfree(v[i]);
    slub_check_invariants(1);
    cprintf("  [T4] pattern size=%u ok\n",(unsigned)sz);
}

static void test_pattern_showcase(void){
    cprintf("[T4] pattern showcase begin\n");
    stage_pattern(32,  180);
    stage_pattern(128, 210);
    stage_pattern(256, 150);
    cprintf("[T4] pattern showcase ok\n");
}

void run_slub_tests(void){
    test_basic();                  // T1
    test_big();                    // T2
    test_fragmentation_snapshot(); // T3
    test_pattern_showcase();       // T4
    cprintf("[slub] all tests done\n");
}
