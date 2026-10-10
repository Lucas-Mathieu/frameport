/* Guest address space: guest address == host address, inside the host's low 4 GiB, which is reserved up front
 * (PROT_NONE) so the host never places its own mappings there. A page map tracks what the guest has mapped. */
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime.h"

#define NPAGES (1u << 20)
static uint8_t pagemap[NPAGES / 8];
static uint8_t readmap[NPAGES / 8]; /* pages the guest can read (the translator fetches code from them) */
static uint8_t hostmap[NPAGES / 8]; /* bridge mode: pages the host process uses below 4 GiB (never the guest's) */
static uint32_t brk_start, brk_cur;
static int reserved;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static inline int page_guest(uint32_t p) { return (pagemap[p >> 3] >> (p & 7)) & 1; }
static inline int page_used(uint32_t p) { return ((pagemap[p >> 3] | hostmap[p >> 3]) >> (p & 7)) & 1; }

static void mark_bits(uint8_t *map, uint32_t addr, uint32_t len, int on)
{
    for (uint32_t p = addr / GUEST_PAGE, e = (uint32_t)(((uint64_t)addr + len + GUEST_PAGE - 1) / GUEST_PAGE); p < e; p++) {
        if (on) map[p >> 3] |= 1u << (p & 7);
        else map[p >> 3] &= ~(1u << (p & 7));
    }
}

static void mark(uint32_t addr, uint32_t len, int used)
{
    mark_bits(pagemap, addr, len, used);
    if (!used) mark_bits(readmap, addr, len, 0);
}

static int range_free(uint32_t addr, uint64_t len)
{
    if (addr < GUEST_LO || addr + len > 0x100000000ull) return 0;
    for (uint32_t p = addr / GUEST_PAGE, e = (uint32_t)((addr + len + GUEST_PAGE - 1) / GUEST_PAGE); p < e; p++)
        if (page_used(p)) return 0;
    return 1;
}

int mem_range_mapped(uint32_t addr, uint32_t len)
{
    for (uint32_t p = addr / GUEST_PAGE, e = (uint32_t)(((uint64_t)addr + len + GUEST_PAGE - 1) / GUEST_PAGE); p < e; p++)
        if (!page_guest(p)) return 0;
    return 1;
}

int mem_readable(uint32_t addr, uint32_t len)
{
    for (uint32_t p = addr / GUEST_PAGE, e = (uint32_t)(((uint64_t)addr + len + GUEST_PAGE - 1) / GUEST_PAGE); p < e; p++)
        if (!((readmap[p >> 3] >> (p & 7)) & 1)) return 0;
    return 1;
}

/* Bridge mode: the guest shares the address space with a host process (ART) that keeps its own mappings below
 * 4 GiB (Java heap, boot image, JIT caches). Those pages are read from /proc/self/maps and never handed out. */
static void scan_host_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return;
    memset(hostmap, 0, sizeof hostmap);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) != 2 || a >= 0x100000000ull) continue;
        if (b > 0x100000000ull) b = 0x100000000ull;
        for (uint64_t p = a / GUEST_PAGE; p < (b + GUEST_PAGE - 1) / GUEST_PAGE; p++)
            if (!page_guest((uint32_t)p)) hostmap[p >> 3] |= 1u << (p & 7);
    }
    fclose(f);
}

int mem_init(void)
{
    if (sysconf(_SC_PAGESIZE) != GUEST_PAGE) {
        /* 16K/64K host pages would need sub-page emulation of guest mappings */
        fprintf(stderr, "a32: host page size %ld is not supported (need 4096)\n", sysconf(_SC_PAGESIZE));
        return -1;
    }
    if (opt_bridge) {
        reserved = 0;
        mark(0, GUEST_LO, 1);
        scan_host_maps();
        return 0;
    }
    void *p = mmap((void *)(uintptr_t)GUEST_LO, 0x100000000ull - GUEST_LO, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || p != (void *)(uintptr_t)GUEST_LO) {
        if (p != MAP_FAILED) munmap(p, 0x100000000ull - GUEST_LO);
        reserved = 0; /* fall back to MAP_FIXED_NOREPLACE per mapping */
        if (opt_verbose) fprintf(stderr, "a32: could not reserve the low 4 GiB (%s)\n", strerror(errno));
    } else {
        reserved = 1;
    }
    /* never hand out page 0..GUEST_LO */
    mark(0, GUEST_LO, 1);
    return 0;
}

static int host_prot(int prot)
{
    /* guest code is only read by the translator, never executed by the host */
    int p = prot & (PROT_READ | PROT_WRITE);
    if (prot & PROT_EXEC) p |= PROT_READ;
    return p;
}

static uint32_t find_free(uint32_t len, uint32_t below)
{
    uint32_t npg = (len + GUEST_PAGE - 1) / GUEST_PAGE, run = 0;
    for (uint32_t p = below / GUEST_PAGE; p-- > GUEST_LO / GUEST_PAGE;) {
        if (page_used(p)) {
            run = 0;
            continue;
        }
        if (++run == npg) return p * GUEST_PAGE;
    }
    return 0;
}

/* Maps at exactly addr. Without the up-front reservation, only pages the guest already owns may be replaced. */
static int64_t map_fixed(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint64_t off)
{
    int hf = (flags & ~(MAP_FIXED | MAP_FIXED_NOREPLACE | MAP_GROWSDOWN)) | MAP_FIXED;
    if (!reserved && !mem_range_mapped(addr, len)) {
        /* claim the pages the guest doesn't own yet without replacing anything (they may be the host's) */
        uint32_t p = addr / GUEST_PAGE, e = (uint32_t)(((uint64_t)addr + len) / GUEST_PAGE);
        while (p < e) {
            if (page_guest(p)) {
                p++;
                continue;
            }
            uint32_t q = p;
            while (q < e && !page_guest(q)) q++;
            void *ph = mmap((void *)(uintptr_t)(p * GUEST_PAGE), (size_t)(q - p) * GUEST_PAGE, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
            if (ph == MAP_FAILED || ph != (void *)(uintptr_t)(p * GUEST_PAGE)) {
                int e2 = ph == MAP_FAILED ? errno : EEXIST;
                if (ph != MAP_FAILED) munmap(ph, (size_t)(q - p) * GUEST_PAGE);
                return -e2;
            }
            mark(p * GUEST_PAGE, (q - p) * GUEST_PAGE, 1); /* now ours (replaced below) */
            p = q;
        }
    }
    void *p = mmap((void *)(uintptr_t)addr, len, host_prot(prot), hf, fd, (off_t)off);
    if (p == MAP_FAILED) return -errno;
    if ((uintptr_t)p != addr) {
        munmap(p, len);
        return -ENOMEM;
    }
    mark(addr, len, 1);
    mark_bits(readmap, addr, len, (prot & (PROT_READ | PROT_EXEC)) != 0);
    return addr;
}

int64_t mem_mmap(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint64_t off)
{
    if (len == 0) return -EINVAL;
    uint64_t alen = ((uint64_t)len + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
    if (alen >= 0x100000000ull) return -ENOMEM;
    if (addr & (GUEST_PAGE - 1) && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) return -EINVAL;
    pthread_mutex_lock(&lock);
    int64_t r;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if ((uint64_t)addr + alen > 0x100000000ull || addr < GUEST_LO) {
            r = -ENOMEM;
        } else if ((flags & MAP_FIXED_NOREPLACE) && !range_free(addr, alen)) {
            r = -EEXIST;
        } else {
            if (!range_free(addr, alen)) code_invalidate(addr, (uint32_t)alen);
            r = map_fixed(addr, (uint32_t)alen, prot, flags & ~MAP_FIXED_NOREPLACE, fd, off);
        }
    } else {
        r = -ENOMEM;
        for (int tries = 0; tries < 4; tries++) {
            uint32_t a = 0;
            if (addr && range_free(addr & ~(GUEST_PAGE - 1), alen)) a = addr & ~(GUEST_PAGE - 1);
            if (!a) a = find_free((uint32_t)alen, GUEST_MMAP_TOP);
            if (!a) a = find_free((uint32_t)alen, GUEST_STACK_TOP - GUEST_STACK_SIZE);
            if (!a) break;
            r = map_fixed(a, (uint32_t)alen, prot, flags | MAP_FIXED, fd, off);
            /* bridge mode: the host mapped something there since the last scan */
            if (r != -EEXIST || !opt_bridge) break;
            scan_host_maps();
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}

static void release(uint32_t addr, uint32_t len)
{
    if (opt_bridge) {
        /* only the guest's own pages: anything else in the range belongs to the host */
        uint32_t p = addr / GUEST_PAGE, e = (uint32_t)(((uint64_t)addr + len) / GUEST_PAGE);
        while (p < e) {
            if (!page_guest(p)) {
                p++;
                continue;
            }
            uint32_t q = p;
            while (q < e && page_guest(q)) q++;
            munmap((void *)(uintptr_t)(p * GUEST_PAGE), (size_t)(q - p) * GUEST_PAGE);
            mark(p * GUEST_PAGE, (q - p) * GUEST_PAGE, 0);
            p = q;
        }
        return;
    }
    if (reserved) mmap((void *)(uintptr_t)addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    else munmap((void *)(uintptr_t)addr, len);
    mark(addr, len, 0);
}

int mem_munmap(uint32_t addr, uint32_t len)
{
    if (addr & (GUEST_PAGE - 1) || len == 0) return -EINVAL;
    uint64_t alen = ((uint64_t)len + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
    if ((uint64_t)addr + alen > 0x100000000ull) return -EINVAL;
    pthread_mutex_lock(&lock);
    code_invalidate(addr, (uint32_t)alen);
    if (addr >= GUEST_LO) release(addr, (uint32_t)alen);
    pthread_mutex_unlock(&lock);
    return 0;
}

int mem_mprotect(uint32_t addr, uint32_t len, int prot)
{
    if (addr & (GUEST_PAGE - 1)) return -EINVAL;
    uint64_t alen = ((uint64_t)len + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
    if (!alen) return 0;
    if (!mem_range_mapped(addr, (uint32_t)alen)) return -ENOMEM;
    if (mprotect((void *)(uintptr_t)addr, alen, host_prot(prot))) return -errno;
    mark_bits(readmap, addr, (uint32_t)alen, (prot & (PROT_READ | PROT_EXEC)) != 0);
    /* code may have been rewritten (JIT engines map RW, write, then map RX) */
    code_invalidate(addr, (uint32_t)alen);
    return 0;
}

int64_t mem_mremap(uint32_t old, uint32_t oldlen, uint32_t newlen, int flags, uint32_t newaddr)
{
    if (old & (GUEST_PAGE - 1)) return -EINVAL;
    uint32_t ol = (oldlen + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    uint64_t nl = ((uint64_t)newlen + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
    if (!nl || nl >= 0x100000000ull) return -EINVAL;
    pthread_mutex_lock(&lock);
    int64_t r;
    code_invalidate(old, ol);
    if (nl <= ol) {
        if (nl < ol) release(old + (uint32_t)nl, ol - (uint32_t)nl);
        r = old;
    } else if (!(flags & MREMAP_FIXED) && range_free(old + ol, nl - ol)) {
        /* grow in place: the reservation behind the mapping has to go first */
        if (reserved) munmap((void *)(uintptr_t)(old + ol), nl - ol);
        void *p = mremap((void *)(uintptr_t)old, ol, nl, 0);
        if (p == MAP_FAILED) {
            r = -errno;
            if (reserved) mmap((void *)(uintptr_t)(old + ol), nl - ol, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        } else {
            int rd = mem_readable(old, ol);
            mark(old, (uint32_t)nl, 1);
            mark_bits(readmap, old, (uint32_t)nl, rd);
            r = old;
        }
    } else if (flags & (MREMAP_MAYMOVE | MREMAP_FIXED)) {
        uint32_t dst = (flags & MREMAP_FIXED) ? newaddr : find_free((uint32_t)nl, GUEST_MMAP_TOP);
        if (!dst || ((flags & MREMAP_FIXED) && (dst < GUEST_LO || dst + nl > 0x100000000ull))) {
            r = -ENOMEM;
        } else {
            void *p = mremap((void *)(uintptr_t)old, ol, nl, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)(uintptr_t)dst);
            if (p == MAP_FAILED) {
                r = -errno;
            } else {
                int rd = mem_readable(old, ol);
                mark(old, ol, 0);
                if (reserved) mmap((void *)(uintptr_t)old, ol, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
                mark(dst, (uint32_t)nl, 1);
                mark_bits(readmap, dst, (uint32_t)nl, rd);
                r = dst;
            }
        }
    } else {
        r = -ENOMEM;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

void mem_set_brk(uint32_t start)
{
    brk_start = brk_cur = (start + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
}

uint32_t mem_brk(uint32_t newbrk)
{
    pthread_mutex_lock(&lock);
    if (newbrk < brk_start) goto out;
    uint32_t top = (brk_cur + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    uint32_t want = (uint32_t)(((uint64_t)newbrk + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1));
    if (want > top) {
        if (!range_free(top, want - top)) goto out;
        if (map_fixed(top, want - top, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) < 0)
            goto out;
    } else if (want < top) {
        release(want, top - want);
    }
    brk_cur = newbrk;
out:
    pthread_mutex_unlock(&lock);
    return brk_cur;
}
