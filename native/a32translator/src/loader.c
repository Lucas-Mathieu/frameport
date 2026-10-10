/* ELF32 ARM loader: maps the program (and its PT_INTERP) into the guest address space and builds the initial
 * stack the way the ARM Linux kernel does (argc, argv, envp, auxv). */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>
#include "interp.h"
#include "runtime.h"

char g_exe_path[4096];

/* ARM Linux HWCAP / HWCAP2 bits (arch/arm/include/uapi/asm/hwcap.h) */
#define ARM_HWCAP_SWP (1 << 0)
#define ARM_HWCAP_HALF (1 << 1)
#define ARM_HWCAP_THUMB (1 << 2)
#define ARM_HWCAP_FAST_MULT (1 << 4)
#define ARM_HWCAP_VFP (1 << 6)
#define ARM_HWCAP_EDSP (1 << 7)
#define ARM_HWCAP_NEON (1 << 12)
#define ARM_HWCAP_VFPv3 (1 << 13)
#define ARM_HWCAP_TLS (1 << 15)
#define ARM_HWCAP_VFPv4 (1 << 16)
#define ARM_HWCAP_IDIVA (1 << 17)
#define ARM_HWCAP_IDIVT (1 << 18)
#define ARM_HWCAP_VFPD32 (1 << 19)
#define ARM_HWCAP_LPAE (1 << 20)
#define ARM_HWCAP2_AES (1 << 0)
#define ARM_HWCAP2_PMULL (1 << 1)
#define ARM_HWCAP2_SHA1 (1 << 2)
#define ARM_HWCAP2_SHA2 (1 << 3)
#define ARM_HWCAP2_CRC32 (1 << 4)

typedef struct {
    uint32_t bias, entry, phdr, phnum, lo, hi;
} image;

static int read_full(int fd, void *buf, size_t n, off_t off)
{
    return pread(fd, buf, n, off) == (ssize_t)n ? 0 : -1;
}

static const char *sysroot_path(const char *sysroot, const char *p, char *buf, size_t n)
{
    if (sysroot && p[0] == '/') {
        snprintf(buf, n, "%s%s", sysroot, p);
        if (access(buf, F_OK) == 0) return buf;
    }
    return p;
}

static void relocate_static_pie(const Elf32_Phdr *ph, int phnum, uint32_t bias)
{
    uint32_t dyn = 0, rel = 0, relsz = 0, relr = 0, relrsz = 0;
    for (int k = 0; k < phnum; k++)
        if (ph[k].p_type == PT_DYNAMIC) dyn = bias + ph[k].p_vaddr;
    if (!dyn) return;
    for (uint32_t d = dyn; ld32(d) != DT_NULL; d += 8) {
        uint32_t tag = ld32(d), val = ld32(d + 4);
        if (tag == DT_REL) rel = bias + val;
        else if (tag == DT_RELSZ) relsz = val;
        else if (tag == 36 /* DT_RELR */) relr = bias + val;
        else if (tag == 35 /* DT_RELRSZ */) relrsz = val;
    }
    for (uint32_t r = rel; r < rel + relsz; r += 8)
        if ((ld32(r + 4) & 0xff) == 23 /* R_ARM_RELATIVE */) st32(bias + ld32(r), ld32(bias + ld32(r)) + bias);
    uint32_t where = 0;
    for (uint32_t r = relr; r < relr + relrsz; r += 4) {
        uint32_t e = ld32(r);
        if (!(e & 1)) {
            where = bias + e;
            st32(where, ld32(where) + bias);
            where += 4;
        } else {
            for (uint32_t k = 0; (e >>= 1) != 0; k++)
                if (e & 1) st32(where + 4 * k, ld32(where + 4 * k) + bias);
            where += 31 * 4;
        }
    }
}

/* bionic's arm32 static start-up code also reads PT_TLS (the TLS initialisation image) and PT_GNU_RELRO from the
 * program headers (AT_PHDR) as if the program were at its link address: move them by the load bias */
static void move_static_pie_phdrs(uint32_t phdr, int phnum, uint32_t bias)
{
    uint32_t page = phdr & ~(GUEST_PAGE - 1), end = (phdr + (uint32_t)phnum * sizeof(Elf32_Phdr) + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    mem_mprotect(page, end - page, PROT_READ | PROT_WRITE);
    for (int k = 0; k < phnum; k++) {
        uint32_t p = phdr + (uint32_t)k * sizeof(Elf32_Phdr);
        uint32_t type = ld32(p);
        if (type == PT_TLS || type == PT_GNU_RELRO) {
            st32(p + 8, ld32(p + 8) + bias);   /* p_vaddr */
            st32(p + 12, ld32(p + 12) + bias); /* p_paddr */
        }
    }
    mem_mprotect(page, end - page, PROT_READ);
}

static int load_image(const char *path, uint32_t want_base, image *im, char *interp, size_t interp_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "a32: %s: %s\n", path, strerror(errno));
        return -1;
    }
    Elf32_Ehdr eh;
    if (read_full(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_ARM || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) ||
        eh.e_phentsize != sizeof(Elf32_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 64) {
        fprintf(stderr, "a32: %s: not a 32-bit little-endian ARM executable\n", path);
        close(fd);
        return -1;
    }
    Elf32_Phdr ph[64];
    if (read_full(fd, ph, eh.e_phnum * sizeof(Elf32_Phdr), eh.e_phoff)) {
        close(fd);
        return -1;
    }
    uint32_t lo = 0xffffffffu, hi = 0, phdr_vaddr = 0;
    if (interp) interp[0] = 0;
    for (int k = 0; k < eh.e_phnum; k++) {
        if (ph[k].p_type == PT_LOAD) {
            if (ph[k].p_vaddr < lo) lo = ph[k].p_vaddr;
            if (ph[k].p_vaddr + ph[k].p_memsz > hi) hi = ph[k].p_vaddr + ph[k].p_memsz;
        } else if (ph[k].p_type == PT_INTERP && interp) {
            uint32_t n = ph[k].p_filesz < interp_len ? ph[k].p_filesz : (uint32_t)interp_len - 1;
            read_full(fd, interp, n, ph[k].p_offset);
            interp[n] = 0;
        } else if (ph[k].p_type == PT_PHDR) {
            phdr_vaddr = ph[k].p_vaddr;
        }
    }
    lo &= ~(GUEST_PAGE - 1);
    hi = (hi + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    uint32_t bias = 0;
    if (eh.e_type == ET_DYN) {
        int64_t r = mem_mmap(want_base, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (r < 0) {
            fprintf(stderr, "a32: %s: no room for %u bytes\n", path, hi - lo);
            close(fd);
            return -1;
        }
        bias = (uint32_t)r - lo;
    }
    for (int k = 0; k < eh.e_phnum; k++) {
        Elf32_Phdr *p = &ph[k];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        uint32_t va = bias + p->p_vaddr, start = va & ~(GUEST_PAGE - 1), delta = va - start;
        uint32_t fileend = va + p->p_filesz, memend = va + p->p_memsz;
        uint32_t fpage_end = (fileend + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        if (p->p_filesz) {
            int64_t r = mem_mmap(start, fpage_end - start, prot | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd,
                                 p->p_offset - delta);
            if (r < 0) {
                fprintf(stderr, "a32: %s: mapping segment at 0x%x failed (%s)\n", path, start, strerror((int)-r));
                close(fd);
                return -1;
            }
            /* zero the rest of the last file page (start of .bss) */
            if (memend > fileend && fpage_end > fileend) memset(g2h(fileend), 0, fpage_end - fileend);
        } else {
            fpage_end = start;
        }
        uint32_t mpage_end = (memend + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        if (mpage_end > fpage_end) {
            int64_t r = mem_mmap(fpage_end, mpage_end - fpage_end, prot | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (r < 0) {
                close(fd);
                return -1;
            }
        }
        if (!(prot & PROT_WRITE)) mem_mprotect(start, mpage_end - start, prot);
        if (!phdr_vaddr && eh.e_phoff >= p->p_offset && eh.e_phoff < p->p_offset + p->p_filesz)
            phdr_vaddr = p->p_vaddr + (eh.e_phoff - p->p_offset);
    }
    /* a static PIE (ET_DYN without an interpreter) moved from its link address: bionic's arm32 start-up code
     * doesn't relocate itself, so apply its relative relocations here */
    if (eh.e_type == ET_DYN && bias && !(interp && interp[0])) {
        relocate_static_pie(ph, eh.e_phnum, bias);
        if (phdr_vaddr) move_static_pie_phdrs(bias + phdr_vaddr, eh.e_phnum, bias);
    }
    close(fd);
    im->bias = bias;
    im->entry = bias + eh.e_entry;
    im->phdr = bias + phdr_vaddr;
    im->phnum = eh.e_phnum;
    im->lo = bias + lo;
    im->hi = bias + hi;
    return 0;
}

static void setup_kuser_page(void)
{
    if (mem_mmap(KUSER_PAGE, GUEST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) < 0)
        return;
    /* the helpers themselves are executed by kuser_helper(); fill the page with UDF so stray jumps trap */
    for (uint32_t a = KUSER_PAGE; a < KUSER_PAGE + GUEST_PAGE; a += 4) st32(a, 0xe7fddef1u);
    st32(0xffff0ffcu, 5); /* __kuser_helper_version */
    /* sigreturn trampolines used when a handler has no SA_RESTORER: mov r7, #119 / #173; svc 0 (ARM) */
    st32(0xffff0500u, 0xe3a07077u);
    st32(0xffff0504u, 0xef000000u);
    st32(0xffff0508u, 0xe3a070adu);
    st32(0xffff050cu, 0xef000000u);
    /* bridge mode: guest functions called by the host return here (hypercall A32_HC_RETURN) */
    st32(A32_RETURN_TRAMPOLINE, 0xe3007000u | A32_HC_RETURN); /* movw r7, #A32_HC_RETURN */
    st32(A32_RETURN_TRAMPOLINE + 4, 0xe3407a32u);             /* movt r7, #0x0a32 */
    st32(A32_RETURN_TRAMPOLINE + 8, 0xef000000u);             /* svc 0 */
    mem_mprotect(KUSER_PAGE, GUEST_PAGE, PROT_READ | PROT_EXEC);
}

int load_program(const load_args *a, a32_cpu *c)
{
    char interp[PATH_MAX], buf[PATH_MAX];
    image main_im, interp_im = {0};
    if (!realpath(a->path, g_exe_path)) snprintf(g_exe_path, sizeof g_exe_path, "%s", a->path);
    if (load_image(a->path, 0x00400000u, &main_im, interp, sizeof interp)) return -1;
    uint32_t entry = main_im.entry;
    if (interp[0]) {
        const char *ip = sysroot_path(a->sysroot, interp, buf, sizeof buf);
        if (load_image(ip, 0, &interp_im, NULL, 0)) {
            fprintf(stderr, "a32: cannot load the program interpreter %s (use --sysroot)\n", interp);
            return -1;
        }
        entry = interp_im.entry;
    }
    mem_set_brk(main_im.hi);
    setup_kuser_page();

    /* bridge mode: wherever the host process left room */
    int64_t stk = mem_mmap(GUEST_STACK_TOP - GUEST_STACK_SIZE, GUEST_STACK_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | (opt_bridge ? 0 : MAP_FIXED), -1, 0);
    if (stk < 0) return -1;
    uint32_t sp = (uint32_t)stk + GUEST_STACK_SIZE;
#define PUSH_BYTES(p, n) (sp -= (n), memcpy(g2h(sp), (p), (n)), sp)
    /* strings */
    uint32_t execfn = PUSH_BYTES(a->path, strlen(a->path) + 1);
    uint32_t platform = PUSH_BYTES("v8l", 4);
    uint8_t rnd[16];
    if (getrandom(rnd, sizeof rnd, 0) != sizeof rnd) memset(rnd, 0x5a, sizeof rnd);
    uint32_t random = PUSH_BYTES(rnd, 16);
    int nenv = 0;
    while (a->envp[nenv]) nenv++;
    uint32_t *argp = calloc(a->argc + 1, 4), *envp = calloc(nenv + 1, 4);
    for (int k = a->argc - 1; k >= 0; k--) argp[k] = PUSH_BYTES(a->argv[k], strlen(a->argv[k]) + 1);
    for (int k = nenv - 1; k >= 0; k--) envp[k] = PUSH_BYTES(a->envp[k], strlen(a->envp[k]) + 1);
    sp &= ~15u;

    unsigned long hh = getauxval(AT_HWCAP);
    uint32_t hwcap = ARM_HWCAP_SWP | ARM_HWCAP_HALF | ARM_HWCAP_THUMB | ARM_HWCAP_FAST_MULT | ARM_HWCAP_VFP |
                     ARM_HWCAP_EDSP | ARM_HWCAP_NEON | ARM_HWCAP_VFPv3 | ARM_HWCAP_TLS | ARM_HWCAP_VFPv4 |
                     ARM_HWCAP_IDIVA | ARM_HWCAP_IDIVT | ARM_HWCAP_VFPD32 | ARM_HWCAP_LPAE;
    uint32_t hwcap2 = 0;
    if (hh & (1 << 3)) hwcap2 |= ARM_HWCAP2_AES;   /* HWCAP_AES */
    if (hh & (1 << 4)) hwcap2 |= ARM_HWCAP2_PMULL; /* HWCAP_PMULL */
    if (hh & (1 << 5)) hwcap2 |= ARM_HWCAP2_SHA1;  /* HWCAP_SHA1 */
    if (hh & (1 << 6)) hwcap2 |= ARM_HWCAP2_SHA2;  /* HWCAP_SHA2 */
    hwcap2 |= ARM_HWCAP2_CRC32;                     /* emulated in C */
    uint32_t auxv[] = {
        AT_PHDR, main_im.phdr, AT_PHENT, sizeof(Elf32_Phdr), AT_PHNUM, main_im.phnum, AT_PAGESZ, GUEST_PAGE,
        AT_BASE, interp_im.bias + (interp[0] ? interp_im.lo - interp_im.bias : 0), AT_FLAGS, 0, AT_ENTRY,
        main_im.entry, AT_UID, (uint32_t)getuid(), AT_EUID, (uint32_t)geteuid(), AT_GID, (uint32_t)getgid(),
        AT_EGID, (uint32_t)getegid(), AT_HWCAP, hwcap, AT_HWCAP2, hwcap2, AT_CLKTCK, 100, AT_SECURE, 0,
        AT_RANDOM, random, AT_PLATFORM, platform, AT_EXECFN, execfn, AT_NULL, 0,
    };
    if (!interp[0]) auxv[9] = 0;
    uint32_t words = 1 + (a->argc + 1) + (nenv + 1) + sizeof auxv / 4;
    sp -= words * 4;
    sp &= ~15u;
    uint32_t p = sp;
    st32(p, a->argc), p += 4;
    for (int k = 0; k < a->argc; k++) st32(p, argp[k]), p += 4;
    st32(p, 0), p += 4;
    for (int k = 0; k < nenv; k++) st32(p, envp[k]), p += 4;
    st32(p, 0), p += 4;
    memcpy(g2h(p), auxv, sizeof auxv);
    free(argp);
    free(envp);
#undef PUSH_BYTES

    memset(c, 0, sizeof *c);
    c->r[13] = sp;
    c->r[15] = entry & ~1u;
    c->thumb = entry & 1;
    if (opt_verbose)
        fprintf(stderr, "a32: loaded %s at 0x%x-0x%x, entry 0x%x%s, sp 0x%x\n", a->path, main_im.lo, main_im.hi, entry,
                interp[0] ? " (interpreter)" : "", sp);
    return 0;
}
