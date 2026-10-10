/* Guest dynamic loader: loads 32-bit ARM shared libraries (the game's) into the runtime, which is a static
 * executable. Symbols resolve against the library itself, its DT_NEEDED libraries, every library loaded before,
 * and the runtime's exports (libc, libm, liblog, ... = "system" libraries). Implements the bionic dl* API for
 * guest code. */
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "grt.h"

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELSZ 0x60000010
#define DT_RELR_ 36
#define DT_RELRSZ_ 35
#define R_ARM_NONE 0
#define R_ARM_ABS32 2
#define R_ARM_REL32 3
#define R_ARM_TLS_DTPMOD32 17
#define R_ARM_TLS_DTPOFF32 18
#define R_ARM_TLS_TPOFF32 19
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#define R_ARM_IRELATIVE 160

typedef struct lib {
    char name[128];   /* as requested (basename) */
    char path[512];
    uint32_t base;         /* load bias */
    uint32_t start, size;  /* mapped range */
    const Elf32_Phdr *phdr;
    int phnum;
    Elf32_Dyn *dyn;
    const Elf32_Sym *symtab;
    const char *strtab;
    const uint32_t *hash;      /* DT_HASH */
    const uint32_t *gnu_hash;  /* DT_GNU_HASH */
    uint32_t nsyms;
    uint32_t exidx, exidx_count;
    struct lib **needed;
    int nneeded;
    int refs;
    int initialised;
    struct lib *next;
} lib;

/* "system" libraries: no file, symbols come from the runtime's export table */
static const char *const SYSTEM_LIBS[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libstdc++.so", "libz.so", "libEGL.so",
    "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libvulkan.so", "libjnigraphics.so",
    "libaaudio.so", "libmediandk.so", "libnativewindow.so", "libsync.so", "libOpenMAXAL.so", "libcamera2ndk.so",
    "libamidi.so", "libbinder_ndk.so", "libneuralnetworks.so",
    "libvrapi.so", /* VrApi 1.x: translated by the host (host/vrapi.c); a game's own copy is the Oculus loader */
};
static lib system_lib = {.name = "libc.so", .refs = 1, .initialised = 1};

static lib *libs, *libs_tail;
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static __thread char err_buf[512];
static __thread int err_set;
static const char *lib_dir = ".";

void loader_set_dir(const char *dir) { lib_dir = dir; }

static void vset_error(int log, const char *fmt, va_list ap)
{
    vsnprintf(err_buf, sizeof err_buf, fmt, ap);
    err_set = 1;
    if (log) grt_log(5, "a32rt", "%s", err_buf);
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vset_error(1, fmt, ap);
    va_end(ap);
}

/* failures callers expect (dlsym probes, Mono looking for AOT images "X.dll.so"): not logged */
static void set_error_quiet(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vset_error(0, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------------------------------ symbols */

static uint32_t elf_hash(const char *n)
{
    uint32_t h = 0, g;
    while (*n) {
        h = (h << 4) + (uint8_t)*n++;
        g = h & 0xf0000000u;
        h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char *n)
{
    uint32_t h = 5381;
    while (*n) h = h * 33 + (uint8_t)*n++;
    return h;
}

static int sym_defined(const Elf32_Sym *s)
{
    int bind = ELF32_ST_BIND(s->st_info);
    return s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK || bind == 10 /* GNU_UNIQUE */);
}

/* a defined, exported symbol of l, or NULL */
static const Elf32_Sym *lib_lookup(const lib *l, const char *name)
{
    if (l == &system_lib) return NULL;
    if (l->gnu_hash) {
        const uint32_t *g = l->gnu_hash;
        uint32_t nbuckets = g[0], symoffset = g[1], bloom_size = g[2], bloom_shift = g[3];
        const uint32_t *bloom = g + 4, *buckets = bloom + bloom_size, *chain = buckets + nbuckets;
        uint32_t h = gnu_hash(name);
        uint32_t word = bloom[(h / 32) % bloom_size];
        if (!((word >> (h % 32)) & (word >> ((h >> bloom_shift) % 32)) & 1)) return NULL;
        uint32_t k = buckets[h % nbuckets];
        if (k < symoffset) return NULL;
        for (;; k++) {
            uint32_t h2 = chain[k - symoffset];
            if ((h | 1) == (h2 | 1)) {
                const Elf32_Sym *s = &l->symtab[k];
                if (sym_defined(s) && !strcmp(l->strtab + s->st_name, name)) return s;
            }
            if (h2 & 1) break;
        }
        return NULL;
    }
    if (l->hash) {
        uint32_t nbucket = l->hash[0];
        const uint32_t *bucket = l->hash + 2, *chain = bucket + nbucket;
        for (uint32_t k = bucket[elf_hash(name) % nbucket]; k; k = chain[k]) {
            const Elf32_Sym *s = &l->symtab[k];
            if (sym_defined(s) && !strcmp(l->strtab + s->st_name, name)) return s;
        }
    }
    return NULL;
}

/* address of name in l (or the runtime's exports for system libraries), 0 if absent */
static uint32_t lib_sym_addr(const lib *l, const char *name)
{
    if (l == &system_lib) return grt_export(name);
    const Elf32_Sym *s = lib_lookup(l, name);
    return s ? l->base + s->st_value : 0;
}

/* bionic's order: the library itself, its DT_NEEDED (breadth first), then everything else that is loaded */
static uint32_t resolve(const lib *l, const char *name)
{
    const lib *q[64];
    int n = 0, h = 0;
    q[n++] = l;
    while (h < n) {
        const lib *c = q[h++];
        uint32_t a = lib_sym_addr(c, name);
        if (a) return a;
        for (int k = 0; k < c->nneeded && n < 64; k++) {
            int seen = 0;
            for (int j = 0; j < n; j++) seen |= q[j] == c->needed[k];
            if (!seen) q[n++] = c->needed[k];
        }
    }
    for (const lib *c = libs; c; c = c->next) {
        uint32_t a = lib_sym_addr(c, name);
        if (a) return a;
    }
    return grt_export(name);
}

/* code that reports the name of an unresolved symbol when called and returns 0: movw/movt r0, name; movw/movt r7;
 * svc 0; bx lr */
static uint32_t unresolved_stub(const char *name)
{
    static uint32_t *page, used;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (!page || used + 6 > 1024) {
        page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        used = 0;
    }
    uint32_t *p = page + used, a = (uint32_t)(uintptr_t)strdup(name), hc = A32_HYPERCALL | HC_UNRESOLVED;
    p[0] = 0xe3000000u | ((a & 0xf000) << 4) | (a & 0xfff);                  /* movw r0, #lo */
    p[1] = 0xe3400000u | ((a >> 12) & 0xf0000) | ((a >> 16) & 0xfff);        /* movt r0, #hi */
    p[2] = 0xe3007000u | ((hc & 0xf000) << 4) | (hc & 0xfff);                /* movw r7, #lo */
    p[3] = 0xe3407000u | ((hc >> 12) & 0xf0000) | ((hc >> 16) & 0xfff);      /* movt r7, #hi */
    p[4] = 0xef000000u;                                                      /* svc 0 */
    p[5] = 0xe12fff1eu;                                                      /* bx lr */
    used += 6;
    __builtin___clear_cache((char *)p, (char *)(p + 6));
    pthread_mutex_unlock(&m);
    return (uint32_t)(uintptr_t)p;
}

/* ------------------------------------------------------------------------------------------------ relocation */

static int reloc_one(lib *l, uint32_t offset, uint32_t info)
{
    uint32_t type = ELF32_R_TYPE(info), symi = ELF32_R_SYM(info);
    uint32_t *where = (uint32_t *)(uintptr_t)(l->base + offset);
    uint32_t s = 0;
    if (symi) {
        const Elf32_Sym *sym = &l->symtab[symi];
        const char *name = l->strtab + sym->st_name;
        if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL) {
            s = l->base + sym->st_value;
        } else {
            s = resolve(l, name);
            if (!s) {
                if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                    s = 0;
                } else if (type == R_ARM_JUMP_SLOT || (ELF32_ST_TYPE(sym->st_info) == STT_FUNC)) {
                    grt_log(5, "a32rt", "%s: unresolved function %s", l->name, name);
                    s = unresolved_stub(name);
                } else if (type == R_ARM_GLOB_DAT || type == R_ARM_ABS32) {
                    /* untyped (functions referenced through the GOT): report when called */
                    grt_log(5, "a32rt", "%s: unresolved symbol %s", l->name, name);
                    s = unresolved_stub(name);
                } else {
                    set_error("%s: cannot resolve %s", l->name, name);
                    return -1;
                }
            }
        }
    }
    switch (type) {
    case R_ARM_NONE: break;
    case R_ARM_ABS32: *where += s; break;
    case R_ARM_REL32: *where += s - (uint32_t)(uintptr_t)where; break;
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT: *where = s; break;
    case R_ARM_RELATIVE: *where += l->base; break;
    case R_ARM_IRELATIVE: *where = ((uint32_t (*)(void))(uintptr_t)(l->base + *where))(); break;
    case R_ARM_TLS_DTPMOD32:
    case R_ARM_TLS_DTPOFF32:
    case R_ARM_TLS_TPOFF32:
        set_error("%s: ELF TLS relocations are not supported yet (type %u)", l->name, type);
        return -1;
    default:
        set_error("%s: unsupported relocation type %u", l->name, type);
        return -1;
    }
    return 0;
}

static int reloc_rel(lib *l, uint32_t addr, uint32_t size)
{
    const Elf32_Rel *r = (const Elf32_Rel *)(uintptr_t)(l->base + addr);
    for (uint32_t k = 0; k < size / sizeof *r; k++)
        if (reloc_one(l, r[k].r_offset, r[k].r_info)) return -1;
    return 0;
}

static int32_t sleb(const uint8_t **p)
{
    int32_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        v |= (int32_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40)) v |= -(1 << shift);
    return v;
}

/* Android's packed relocations ("APS2", bionic linker_reloc_iterators.h) */
static int reloc_android(lib *l, uint32_t addr, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)(l->base + addr);
    if (size < 4 || memcmp(p, "APS2", 4)) {
        set_error("%s: unknown packed relocation format", l->name);
        return -1;
    }
    p += 4;
    int32_t count = sleb(&p);
    uint32_t offset = (uint32_t)sleb(&p), info = 0;
    while (count > 0) {
        int32_t gsize = sleb(&p), flags = sleb(&p), delta = 0;
        if (flags & 2) delta = sleb(&p);
        if (flags & 1) info = (uint32_t)sleb(&p);
        if (flags & 8) {
            set_error("%s: packed relocations with addends on a REL target", l->name);
            return -1;
        }
        for (int32_t k = 0; k < gsize; k++) {
            offset += (flags & 2) ? (uint32_t)delta : (uint32_t)sleb(&p);
            if (!(flags & 1)) info = (uint32_t)sleb(&p);
            if (reloc_one(l, offset, info)) return -1;
        }
        count -= gsize;
    }
    return 0;
}

static void reloc_relr(lib *l, uint32_t addr, uint32_t size)
{
    const uint32_t *r = (const uint32_t *)(uintptr_t)(l->base + addr);
    uint32_t *where = NULL;
    for (uint32_t k = 0; k < size / 4; k++) {
        uint32_t e = r[k];
        if (!(e & 1)) {
            where = (uint32_t *)(uintptr_t)(l->base + e);
            *where++ += l->base;
        } else {
            for (uint32_t j = 0; (e >>= 1) != 0; j++)
                if (e & 1) where[j] += l->base;
            where += 31;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ loading */

static lib *load_lib(const char *name);

static int is_system(const char *base)
{
    for (size_t k = 0; k < sizeof SYSTEM_LIBS / sizeof *SYSTEM_LIBS; k++)
        if (!strcmp(base, SYSTEM_LIBS[k])) return 1;
    return 0;
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* The guest copy of libX.so is stored as libX.a32.so next to the host libraries (the package manager extracts
 * lib*.so files only, and libX.so itself is the host stub). */
static int open_lib(const char *name, char *path, size_t n)
{
    int fd;
    size_t len = strlen(name);
    if (strchr(name, '/')) {
        /* a path into the app's library folder names the host stub: its guest code is next to it */
        if (len > 3 && !strcmp(name + len - 3, ".so")) {
            snprintf(path, n, "%.*s.a32.so", (int)(len - 3), name);
            if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) return fd;
        }
        snprintf(path, n, "%s", name);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) return fd;
        name = basename_of(name);
        len = strlen(name);
    }
    if (len > 3 && !strcmp(name + len - 3, ".so")) {
        snprintf(path, n, "%s/%.*s.a32.so", lib_dir, (int)(len - 3), name);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) return fd;
    }
    snprintf(path, n, "%s/%s", lib_dir, name);
    return open(path, O_RDONLY | O_CLOEXEC);
}

static int map_lib(lib *l, int fd)
{
    Elf32_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_machine != EM_ARM || eh.e_type != ET_DYN || eh.e_phnum > 64) {
        set_error("%s: not a 32-bit ARM shared library", l->path);
        return -1;
    }
    Elf32_Phdr ph[64];
    if (pread(fd, ph, eh.e_phnum * sizeof *ph, eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof *ph)) return -1;
    uint32_t lo = 0xffffffffu, hi = 0;
    for (int k = 0; k < eh.e_phnum; k++)
        if (ph[k].p_type == PT_LOAD) {
            if (ph[k].p_vaddr < lo) lo = ph[k].p_vaddr;
            if (ph[k].p_vaddr + ph[k].p_memsz > hi) hi = ph[k].p_vaddr + ph[k].p_memsz;
        }
    lo &= ~4095u;
    hi = (hi + 4095) & ~4095u;
    void *res = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        set_error("%s: no room for %u bytes", l->path, hi - lo);
        return -1;
    }
    l->start = (uint32_t)(uintptr_t)res;
    l->base = l->start - lo;
    l->size = hi - lo;
    for (int k = 0; k < eh.e_phnum; k++) {
        const Elf32_Phdr *p = &ph[k];
        if (p->p_type == PT_DYNAMIC) l->dyn = (Elf32_Dyn *)(uintptr_t)(l->base + p->p_vaddr);
        if (p->p_type == PT_ARM_EXIDX) {
            l->exidx = l->base + p->p_vaddr;
            l->exidx_count = p->p_memsz / 8;
        }
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        uint32_t va = l->base + p->p_vaddr, start = va & ~4095u;
        uint32_t fend = va + p->p_filesz, mend = va + p->p_memsz, fpend = (fend + 4095) & ~4095u;
        /* writable while relocating; final protections are applied afterwards */
        if (p->p_filesz) {
            void *m = mmap((void *)(uintptr_t)start, fpend - start, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_FIXED, fd, p->p_offset - (va - start));
            if (m == MAP_FAILED) {
                set_error("%s: mapping a segment failed (%s)", l->path, strerror(errno));
                return -1;
            }
            if (mend > fend && fpend > fend) memset((void *)(uintptr_t)fend, 0, fpend - fend);
        } else {
            fpend = start;
        }
        uint32_t mpend = (mend + 4095) & ~4095u;
        if (mpend > fpend &&
            mmap((void *)(uintptr_t)fpend, mpend - fpend, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
            set_error("%s: mapping .bss failed", l->path);
            return -1;
        }
        if (!l->phdr && eh.e_phoff >= p->p_offset && eh.e_phoff < p->p_offset + p->p_filesz)
            l->phdr = (const Elf32_Phdr *)(uintptr_t)(va + (eh.e_phoff - p->p_offset));
    }
    l->phnum = eh.e_phnum;
    if (!l->dyn) {
        set_error("%s: no dynamic section", l->path);
        return -1;
    }
    return 0;
}

static void protect_lib(lib *l)
{
    for (int k = 0; k < l->phnum; k++) {
        const Elf32_Phdr *p = &l->phdr[k];
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        uint32_t s = (l->base + p->p_vaddr) & ~4095u, e = (l->base + p->p_vaddr + p->p_memsz + 4095) & ~4095u;
        mprotect((void *)(uintptr_t)s, e - s, prot);
    }
    for (int k = 0; k < l->phnum; k++) {
        const Elf32_Phdr *p = &l->phdr[k];
        if (p->p_type != PT_GNU_RELRO) continue;
        uint32_t s = (l->base + p->p_vaddr) & ~4095u, e = (l->base + p->p_vaddr + p->p_memsz) & ~4095u;
        if (e > s) mprotect((void *)(uintptr_t)s, e - s, PROT_READ);
    }
}

static void init_lib(lib *l)
{
    if (l->initialised) return;
    l->initialised = 1;
    for (int k = 0; k < l->nneeded; k++) init_lib(l->needed[k]);
    uint32_t init = 0, arr = 0, arrsz = 0;
    for (Elf32_Dyn *d = l->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_INIT) init = l->base + d->d_un.d_ptr;
        else if (d->d_tag == DT_INIT_ARRAY) arr = l->base + d->d_un.d_ptr;
        else if (d->d_tag == DT_INIT_ARRAYSZ) arrsz = d->d_un.d_val;
    }
    extern char **environ;
    if (init) ((void (*)(int, char **, char **))(uintptr_t)init)(0, NULL, environ);
    for (uint32_t k = 0; k < arrsz / 4; k++) {
        uint32_t f = ((uint32_t *)(uintptr_t)arr)[k];
        if (f && f != 0xffffffffu) ((void (*)(int, char **, char **))(uintptr_t)f)(0, NULL, environ);
    }
}

static lib *find_loaded(const char *base)
{
    for (lib *l = libs; l; l = l->next)
        if (!strcmp(l->name, base)) return l;
    return NULL;
}

static lib *load_lib(const char *name)
{
    const char *base = basename_of(name);
    lib *l = find_loaded(base);
    if (l) {
        l->refs++;
        return l;
    }
    char path[512];
    int fd = open_lib(name, path, sizeof path);
    if (fd < 0) {
        if (is_system(base)) return &system_lib;
        size_t nl = strlen(name);
        if (nl > 7 && !strcmp(name + nl - 7, ".dll.so")) set_error_quiet("dlopen failed: library \"%s\" not found", name);
        else set_error("dlopen failed: library \"%s\" not found", name);
        return NULL;
    }
    l = calloc(1, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", base);
    snprintf(l->path, sizeof l->path, "%s", path);
    l->refs = 1;
    int r = map_lib(l, fd);
    close(fd);
    if (r) {
        free(l);
        return NULL;
    }
    uint32_t rel = 0, relsz = 0, jmprel = 0, jmprelsz = 0, arel = 0, arelsz = 0, relr = 0, relrsz = 0;
    int nneeded = 0;
    for (Elf32_Dyn *d = l->dyn; d->d_tag != DT_NULL; d++) {
        uint32_t v = d->d_un.d_val;
        switch (d->d_tag) {
        case DT_SYMTAB: l->symtab = (const Elf32_Sym *)(uintptr_t)(l->base + v); break;
        case DT_STRTAB: l->strtab = (const char *)(uintptr_t)(l->base + v); break;
        case DT_HASH: l->hash = (const uint32_t *)(uintptr_t)(l->base + v); break;
        case DT_GNU_HASH: l->gnu_hash = (const uint32_t *)(uintptr_t)(l->base + v); break;
        case DT_REL: rel = v; break;
        case DT_RELSZ: relsz = v; break;
        case DT_JMPREL: jmprel = v; break;
        case DT_PLTRELSZ: jmprelsz = v; break;
        case DT_ANDROID_REL: arel = v; break;
        case DT_ANDROID_RELSZ: arelsz = v; break;
        case DT_RELR_: relr = v; break;
        case DT_RELRSZ_: relrsz = v; break;
        case DT_NEEDED: nneeded++; break;
        case DT_RELA: set_error("%s: RELA relocations on ARM32?", l->name); return NULL;
        }
    }
    if (l->hash) l->nsyms = l->hash[1];
    /* published before its dependencies load: a cycle finds it instead of loading it twice */
    if (libs_tail) libs_tail->next = l;
    else libs = l;
    libs_tail = l;
    l->needed = calloc(nneeded ? nneeded : 1, sizeof *l->needed);
    for (Elf32_Dyn *d = l->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *dep = l->strtab + d->d_un.d_val;
        lib *dl = load_lib(dep);
        if (!dl) {
            grt_log(5, "a32rt", "%s: needed library %s missing (%s)", l->name, dep, err_buf);
            continue;
        }
        l->needed[l->nneeded++] = dl;
    }
    if ((relr && relrsz && (reloc_relr(l, relr, relrsz), 0)) || (arel && reloc_android(l, arel, arelsz)) ||
        (rel && reloc_rel(l, rel, relsz)) || (jmprel && reloc_rel(l, jmprel, jmprelsz))) {
        grt_log(6, "a32rt", "%s: relocation failed: %s", l->name, err_buf);
        return NULL;
    }
    protect_lib(l);
    grt_log(4, "a32rt", "loaded %s at 0x%08x (%u KiB)", l->path, l->start, l->size / 1024);
    return l;
}

/* ------------------------------------------------------------------------------------------------ dl* API */

void *grt_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name) return &system_lib; /* the "main program": global lookup */
    pthread_mutex_lock(&lock);
    lib *l = load_lib(name);
    if (l) init_lib(l);
    pthread_mutex_unlock(&lock);
    return l;
}

void *grt_dlsym(void *handle, const char *name)
{
    uint32_t a = 0;
    pthread_mutex_lock(&lock);
    if (handle == RTLD_DEFAULT || handle == RTLD_NEXT || handle == &system_lib) {
        for (lib *l = libs; l && !a; l = l->next) a = lib_sym_addr(l, name);
        if (!a) a = grt_export(name);
    } else {
        a = resolve(handle, name);
    }
    pthread_mutex_unlock(&lock);
    if (!a) set_error_quiet("undefined symbol: %s", name);
    return (void *)(uintptr_t)a;
}

int grt_dlclose(void *handle)
{
    (void)handle; /* libraries stay loaded (as bionic does for most of them) */
    return 0;
}

char *grt_dlerror(void)
{
    if (!err_set) return NULL;
    err_set = 0;
    return err_buf;
}

static lib *lib_at(uint32_t a)
{
    for (lib *l = libs; l; l = l->next)
        if (a >= l->start && a - l->start < l->size) return l;
    return NULL;
}

/* number of entries in l's dynamic symbol table */
static uint32_t lib_nsyms(const lib *l)
{
    uint32_t n = l->nsyms;
    if (!n && l->gnu_hash) {
        /* the highest symbol index in the GNU hash chains */
        const uint32_t *g = l->gnu_hash;
        uint32_t nb = g[0], so = g[1], bs = g[2];
        const uint32_t *b = g + 4 + bs, *ch = b + nb;
        for (uint32_t k = 0; k < nb; k++)
            if (b[k] >= n) n = b[k];
        if (n >= so)
            while (!(ch[n - so] & 1)) n++;
        n++;
    }
    return n;
}

/* The defined symbols of a library whose names start with prefix: a malloc'd array of (name, address) pairs,
 * ended by a 0 name. For the host bridge (Java_* functions). */
uint32_t *grt_list_symbols(void *handle, const char *prefix)
{
    lib *l = handle;
    size_t plen = strlen(prefix), n = 0, cap = 16;
    uint32_t *v = malloc(cap * 8);
    pthread_mutex_lock(&lock);
    if (l && l != &system_lib) {
        uint32_t ns = lib_nsyms(l);
        for (uint32_t k = 1; k < ns; k++) {
            const Elf32_Sym *s = &l->symtab[k];
            const char *nm = l->strtab + s->st_name;
            if (!sym_defined(s) || strncmp(nm, prefix, plen)) continue;
            if (n + 2 > cap) v = realloc(v, (cap *= 2) * 8);
            v[2 * n] = (uint32_t)(uintptr_t)nm;
            v[2 * n + 1] = l->base + s->st_value;
            n++;
        }
    }
    pthread_mutex_unlock(&lock);
    v[2 * n] = v[2 * n + 1] = 0;
    return v;
}

int grt_dladdr(const void *addr, Dl_info *info)
{
    uint32_t a = (uint32_t)(uintptr_t)addr;
    pthread_mutex_lock(&lock);
    lib *l = lib_at(a);
    if (!l) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    info->dli_fname = l->path;
    info->dli_fbase = (void *)(uintptr_t)l->base;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    uint32_t best = 0;
    uint32_t n = lib_nsyms(l);
    for (uint32_t k = 1; k < n; k++) {
        const Elf32_Sym *s = &l->symtab[k];
        if (!sym_defined(s) || !s->st_value) continue;
        uint32_t sa = l->base + s->st_value;
        if (sa <= a && sa > best && (a - sa < s->st_size || !s->st_size)) {
            best = sa;
            info->dli_sname = l->strtab + s->st_name;
            info->dli_saddr = (void *)(uintptr_t)sa;
        }
    }
    pthread_mutex_unlock(&lock);
    return 1;
}

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    int r = 0;
    for (lib *l = libs; l && !r; l = l->next) {
        struct dl_phdr_info i;
        memset(&i, 0, sizeof i);
        i.dlpi_addr = l->base;
        i.dlpi_name = l->path;
        i.dlpi_phdr = l->phdr;
        i.dlpi_phnum = (Elf32_Half)l->phnum;
        r = cb(&i, sizeof i, data);
    }
    return r;
}

/* libc.a has its own dl_unwind_find_exidx, which only knows the runtime itself: the link wraps it (--wrap), so
 * the runtime's unwinder sees guest libraries too */
uint32_t __real_dl_unwind_find_exidx(uint32_t pc, int *count);

uint32_t __wrap_dl_unwind_find_exidx(uint32_t pc, int *count)
{
    lib *l = lib_at(pc);
    if (!l) return __real_dl_unwind_find_exidx(pc, count);
    if (!l->exidx) {
        *count = 0;
        return 0;
    }
    *count = (int)l->exidx_count;
    return l->exidx;
}

/* the "(see bionic)" variants */
void *grt_android_dlopen_ext(const char *name, int flags, const void *extinfo)
{
    (void)extinfo;
    return grt_dlopen(name, flags);
}

void *grt_dlvsym(void *handle, const char *name, const char *version)
{
    (void)version;
    return grt_dlsym(handle, name);
}
