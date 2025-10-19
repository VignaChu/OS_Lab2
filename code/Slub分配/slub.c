#include <defs.h>
#include <stdio.h>
#include <string.h>
#include "../debug/assert.h"
#include "mmu.h"
#include "memlayout.h"
#include "pmm.h"
#include "slub.h"

/* ========= 工具：Page <-> KVA ========= */
static inline void *page_to_kva(struct Page *pg) {
    return (void *)(page2pa(pg) + va_pa_offset);
}
static inline struct Page *kva_to_page(void *kva) {
    return pa2page(PADDR(kva));
}

/* ========= 常量与小工具 ========= */
#ifndef PGSIZE
#define PGSIZE 4096
#endif
#ifndef ROUNDDOWN
#define ROUNDDOWN(a, n) ((uintptr_t)(a) & ~((uintptr_t)(n) - 1))
#endif
#ifndef ROUNDUP
#define ROUNDUP(a, n) ((((uintptr_t)(a) + (n) - 1)) & ~((uintptr_t)(n) - 1))
#endif

#define SLUB_ALIGN      8u
#define SLUB_NIL        0xFFFFFFFFu
#define SLAB_MAGIC      0x51ab51abU
#define BIG_MAGIC       0xB16B00B5U
#define BIG_FOOT_MAGIC  0xF00DB1DEU   

static inline size_t align_up(size_t x, size_t a) {
    return (x + a - 1) / a * a;
}

/* ========= 元数据 ========= */
struct kmem_cache;

struct slub_slab {
    struct kmem_cache *cache;
    struct slub_slab  *next;
    uint16_t total;
    uint16_t inuse;
    uint32_t free_head;     /* free-list 存在对象首 U32 */
    uint32_t magic;         /* SLAB_MAGIC */
};

struct kmem_cache {
    size_t obj_size;        /* 请求大小（外部可见） */
    size_t obj_stride;      /* 实际步长（含对齐）   */
    size_t objs_per_slab;
    struct slub_slab *partial;  /* 有空位 */
    struct slub_slab *full;     /* 满 */
    struct slub_slab *empty;    /* 暂不用：释放到 0 直接还页，避免内存涨 */
};

/* 固定 size-classes（8..2048） */
static const size_t size_classes[] = {8,16,32,64,128,256,512,1024,2048,0};
#define N_CACHES 9
static struct kmem_cache caches[N_CACHES];

/* ========= slab 辅助 ========= */
static inline struct slub_slab *kva_to_slab(void *kva_page_base) {
    return (struct slub_slab *)kva_page_base;
}

static inline void *slab_obj_base(struct slub_slab *slab) {
    uintptr_t base = (uintptr_t)slab;
    uintptr_t obj0 = ROUNDUP(base + sizeof(struct slub_slab), SLUB_ALIGN);
    return (void *)obj0;
}

static inline void *slab_index_to_ptr(struct slub_slab *slab, uint32_t idx) {
    return (void *)((uintptr_t)slab_obj_base(slab) +
                    (uintptr_t)slab->cache->obj_stride * idx);
}

static inline uint32_t slab_ptr_to_index(struct slub_slab *slab, void *p) {
    return (uint32_t)(((uintptr_t)p - (uintptr_t)slab_obj_base(slab)) /
                      slab->cache->obj_stride);
}

/* 兜底：重建某 slab 的 free-list（当发现损坏时使用） */
static void slab_rebuild_freelist(struct slub_slab *slab) {
    uintptr_t obj0 = (uintptr_t)slab_obj_base(slab);
    uint32_t n = (uint32_t)slab->total;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t *slot = (uint32_t *)(obj0 + slab->cache->obj_stride * i);
        *slot = (i + 1 < n) ? (i + 1) : SLUB_NIL;
    }
    slab->free_head = 0;
    slab->inuse = 0;
}

/* ========= slab create/destroy ========= */
static struct slub_slab *slab_create(struct kmem_cache *c) {
    struct Page *pg = alloc_pages(1);
    if (!pg) return NULL;

    void *base = page_to_kva(pg);
    struct slub_slab *slab = kva_to_slab(base);
    memset(slab, 0, sizeof(*slab));
    slab->cache = c;
    slab->magic = SLAB_MAGIC;

    uintptr_t obj0 = (uintptr_t)slab_obj_base(slab);
    size_t usable = PGSIZE - (obj0 - (uintptr_t)slab);
    size_t nobj   = usable / c->obj_stride;
    if (nobj == 0) { free_pages(pg, 1); return NULL; }

    slab->total = (uint16_t)nobj;
    slab->inuse = 0;
    slab->next  = NULL;

    for (uint32_t i = 0; i < nobj; ++i) {
        uint32_t *slot = (uint32_t *)(obj0 + c->obj_stride * i);
        *slot = (i + 1 < nobj) ? (i + 1) : SLUB_NIL;
    }
    slab->free_head = 0;
    if (c->objs_per_slab == 0) c->objs_per_slab = nobj;

    cprintf("[slub] create: class=%u stride=%u obj_off=0x%x usable=%u nobj=%u\n",
        (unsigned)c->obj_size, (unsigned)c->obj_stride,
        (unsigned)(obj0 - (uintptr_t)slab),
        (unsigned)usable, (unsigned)nobj);

    return slab;
}

static void slab_destroy(struct slub_slab *slab) {
    /* 页首的 slab 头一定在页对齐处 */
    assert(slab->magic == SLAB_MAGIC);
    free_pages(kva_to_page((void *)slab), 1);
}

/* ========= cache 链表操作 ========= */
static void cache_push_partial(struct kmem_cache *c, struct slub_slab *s) {
    s->next = c->partial; c->partial = s;
}
static void cache_push_full(struct kmem_cache *c, struct slub_slab *s) {
    s->next = c->full; c->full = s;
}
static struct slub_slab *cache_pop_partial(struct kmem_cache *c) {
    struct slub_slab *s = c->partial;
    if (s) { c->partial = s->next; s->next = NULL; }
    return s;
}
static void cache_unlink(struct kmem_cache *c, struct slub_slab *slab) {
    struct slub_slab **pp = &c->partial;
    while (*pp) {
        if (*pp == slab) { *pp = slab->next; slab->next = NULL; return; }
        pp = &(*pp)->next;
    }
    pp = &c->full;
    while (*pp) {
        if (*pp == slab) { *pp = slab->next; slab->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

/* 取一个有空位的 slab（优先 partial，没则新建） */
static struct slub_slab *cache_pop_slab_with_space(struct kmem_cache *c) {
    struct slub_slab *s = cache_pop_partial(c);
    if (s) return s;
    return slab_create(c);
}

/* ========= class 选择 ========= */
static int class_index(size_t n) {
    for (int i = 0; i < N_CACHES; ++i)
        if (n <= size_classes[i]) return i;
    return -1;
}

/* ========= 大块（>2KB）走页 =========
 * 设计：双头标记，既在页首放 big_hdr，也在“返回指针前面”再放一份 hdr，
 * 避免被页首误分类或用户指针被篡改时难以恢复。free 时优先检查“p-1 头”。
 */
struct big_hdr {
    uint32_t magic;     /* BIG_MAGIC   */
    uint32_t npages;    /* 连续页数     */
    uint32_t guard;     /* BIG_FOOT_MAGIC */
    uint32_t _pad;      /* 对齐 */
};

static void *big_alloc(size_t n) {
    size_t need = n + sizeof(struct big_hdr) * 2;   /* 双头 */
    size_t np   = (need + PGSIZE - 1) / PGSIZE;

    struct Page *pg = alloc_pages(np);
    if (!pg) return NULL;

    void *base = page_to_kva(pg);

    /* 页首头 */
    struct big_hdr *h0 = (struct big_hdr *)base;
    h0->magic  = BIG_MAGIC;
    h0->npages = (uint32_t)np;
    h0->guard  = BIG_FOOT_MAGIC;
    h0->_pad   = 0;

    /* 返回给用户的指针 */
    uint8_t *ret = (uint8_t *)base + sizeof(struct big_hdr);

    /* 返回指针前的“镜像头”，用于更稳健的 free 判定 */
    struct big_hdr *h1 = (struct big_hdr *)(ret - sizeof(struct big_hdr));
    *h1 = *h0;

    return ret;
}

static void big_free_by_hdr(struct big_hdr *h) {
    if (h->magic != BIG_MAGIC || h->guard != BIG_FOOT_MAGIC) {
        cprintf("[slub] E: big_free header broken: magic=0x%x guard=0x%x\n",
                h->magic, h->guard);
        assert(0);
    }
    /* 将 magic 清零以防双 free */
    uint32_t np = h->npages;
    h->magic = 0; h->guard = 0;
    void *base = (void *)ROUNDDOWN((uintptr_t)h, PGSIZE);  
    free_pages(kva_to_page(base), np);
}

/* ========= 初始化 ========= */
void slub_init(void) {
    for (int i = 0; i < N_CACHES; ++i) {
        size_t s      = size_classes[i];
        size_t stride = align_up(s > sizeof(uint32_t) ? s : sizeof(uint32_t),
                                 SLUB_ALIGN);
        caches[i].obj_size      = s;
        caches[i].obj_stride    = stride;
        caches[i].objs_per_slab = 0;
        caches[i].partial = caches[i].full = caches[i].empty = NULL;
    }
    cprintf("[slub] init %d caches (8..2048)\n", N_CACHES);
}

/* ========= 分配 ========= */
void *slub_alloc(size_t n) {
    if (n == 0) n = 1;
    int idx = class_index(n);
    if (idx < 0) return big_alloc(n);

    struct kmem_cache *c = &caches[idx];
    struct slub_slab *slab = cache_pop_slab_with_space(c);
    if (!slab) return NULL;

    /* 必要时重建 free-list（防止被人为覆盖） */
    if (slab->free_head == SLUB_NIL) {
        cprintf("[slub] warn: free_head==NIL, rebuild freelist (class=%u)\n",
                (unsigned)c->obj_size);
        slab_rebuild_freelist(slab);
    }
    assert(slab->free_head != SLUB_NIL);

    uint32_t idxobj = slab->free_head;
    void *obj = slab_index_to_ptr(slab, idxobj);

    uint32_t *slot = (uint32_t *)obj;
    slab->free_head = *slot;
    slab->inuse++;

    if (slab->inuse < slab->total) cache_push_partial(c, slab);
    else                           cache_push_full(c, slab);
    return obj;
}

/* ========= 释放 ========= */
void slub_free(void *p) {
    if (!p) return;

    /* 先尝试“大块”的“p-1 镜像头” */
    struct big_hdr *h1 = (struct big_hdr *)((uint8_t *)p - sizeof(struct big_hdr));
    if (h1->magic == BIG_MAGIC && h1->guard == BIG_FOOT_MAGIC) {
        big_free_by_hdr(h1);
        return;
    }

    /* 再看页首：可能是小对象 slab 或 大块页首头 */
    void *base = (void *)ROUNDDOWN((uintptr_t)p, PGSIZE);

    /* 小对象 slab */
    struct slub_slab *as_slab = (struct slub_slab *)base;
    if (as_slab->magic == SLAB_MAGIC) {
        struct slub_slab *slab = as_slab;
        struct kmem_cache *c   = slab->cache;

        cache_unlink(c, slab);

        uint32_t idxobj = slab_ptr_to_index(slab, p);
        assert(idxobj < slab->total);

        uint32_t *slot = (uint32_t *)p;
        *slot = slab->free_head;
        slab->free_head = idxobj;
        assert(slab->inuse > 0);
        slab->inuse--;

        if (slab->inuse == 0) slab_destroy(slab);
        else                  cache_push_partial(c, slab);
        return;
    }

    /* 页首是大块头（兼容路径） */
    struct big_hdr *h0 = (struct big_hdr *)base;
    if (h0->magic == BIG_MAGIC && h0->guard == BIG_FOOT_MAGIC) {
        big_free_by_hdr(h0);
        return;
    }

    cprintf("[slub] E: slub_free classify fail p=%p base=%p\n", p, base);
    assert(0);
}

/* ========= 适配 kmalloc/kfree ========= */
void *kmalloc(size_t n) { return slub_alloc(n); }
void *kzalloc(size_t n) { void *p = slub_alloc(n); if (p) memset(p, 0, n); return p; }
void  kfree(void *p)    { slub_free(p); }

/* ========= 统计 / 自检 ========= */
static int count_list(struct slub_slab *s){ int n=0; while(s){ n++; s=s->next; } return n; }

void slub_dump_stats(int verbose) {
    cprintf("[slub] stats\n");
    for (int i = 0; i < N_CACHES; ++i) {
        struct kmem_cache *c = &caches[i];
        int n_partial = count_list(c->partial);
        int n_full    = count_list(c->full);

        uint64_t inuse=0, total=0;
        for(struct slub_slab *s=c->full; s; s=s->next){ inuse+=s->total; total+=s->total; }
        for(struct slub_slab *s=c->partial; s; s=s->next){ inuse+=s->inuse; total+=s->total; }

        uint64_t bytes_req = inuse * c->obj_size;
        uint64_t bytes_cap = (uint64_t)(n_full+n_partial) * (c->objs_per_slab * c->obj_stride);
        uint64_t internal_frag = bytes_cap>bytes_req? (bytes_cap - bytes_req):0;

        cprintf("  class=%4u stride=%4u slab(partial=%d, full=%d) objs inuse=%llu/%llu, internal_frag=%lluB\n",
            (unsigned)c->obj_size, (unsigned)c->obj_stride, n_partial, n_full,
            (unsigned long long)inuse, (unsigned long long)total,
            (unsigned long long)internal_frag);

        if(verbose){
            for(struct slub_slab *s=c->partial; s; s=s->next){
                cprintf("    [partial] inuse=%u total=%u free_head=%u\n", s->inuse, s->total, s->free_head);
            }
        }
    }
    cprintf("[slub] stats end\n");
}

/* Floyd 防环，避免自检卡死 */
static int list_has_cycle(struct slub_slab *head){
    struct slub_slab *slow=head, *fast=head;
    while (fast && fast->next){
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) return 1;
    }
    return 0;
}

/* 一致性检查：带防环与长度护栏 */
int slub_check_invariants(int fatal){
    int bad=0;
    const int GUARD_MAX=100000;
    for (int i=0;i<N_CACHES;++i){
        struct kmem_cache *c=&caches[i];

        if (list_has_cycle(c->partial)){
            cprintf("[slub] E: cycle in PARTIAL list (class=%u)\n",(unsigned)c->obj_size);
            if (fatal) assert(0); return 0;
        }
        if (list_has_cycle(c->full)){
            cprintf("[slub] E: cycle in FULL list (class=%u)\n",(unsigned)c->obj_size);
            if (fatal) assert(0); return 0;
        }

        int guard=0;
        for(struct slub_slab *s=c->partial; s; s=s->next){
            if(++guard>GUARD_MAX){ cprintf("[slub] E: partial too long (class=%u)\n",(unsigned)c->obj_size); if(fatal) assert(0); bad=1; break; }
            if(!(s->inuse<=s->total)){ cprintf("[slub] E: inuse>total (class=%u)\n",(unsigned)c->obj_size); if(fatal) assert(0); bad=1; }
            uint32_t seen=0, idx=s->free_head;
            while(idx!=SLUB_NIL){
                if(idx>=s->total){ cprintf("[slub] E: bad idx=%u total=%u\n",idx,s->total); if(fatal) assert(0); bad=1; break; }
                void *p=slab_index_to_ptr(s, idx);
                idx=*(uint32_t*)p;
                if(++seen> s->total){ cprintf("[slub] E: free list loop\n"); if(fatal) assert(0); bad=1; break; }
            }
        }
        guard=0;
        for(struct slub_slab *s=c->full; s; s=s->next){
            if(++guard>GUARD_MAX){ cprintf("[slub] E: full too long (class=%u)\n",(unsigned)c->obj_size); if(fatal) assert(0); bad=1; break; }
            if(!(s->inuse==s->total)){ cprintf("[slub] E: full but inuse!=total (class=%u)\n",(unsigned)c->obj_size); if(fatal) assert(0); bad=1; }
        }
    }
    if(!bad) cprintf("[slub] invariants ok\n");
    return !bad;
}
