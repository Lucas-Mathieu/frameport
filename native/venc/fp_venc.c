/* fp_venc: hardware H.264 encoding of the Steam Frame's SteamVR "headset view" webcam for FramePort's live view.
 * Source (v4l2loopback, RGB24) -> NV12 (convert.c) -> the SoC's V4L2 stateful encoder -> H.264 Annex B on stdout.
 * Specification: native/venc/SPEC.md (V4L2: kernel Documentation/userspace-api/media/v4l/dev-encoder.rst). */
#include "sys.h"
#include "convert.h"
#include <linux/videodev2.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define SRC_BUFS 3
#define ENC_BUFS 4
#define TAG "fp_venc: "

/* ---------------------------------------------------------------- output helpers */

static void say(const char *a, const char *b, long n, int with_n)
{
    struct line l = {.n = 0};
    ls(&l, TAG);
    ls(&l, a);
    if (b) ls(&l, b);
    if (with_n) ln(&l, n);
    ls(&l, "\n");
    write_all(2, l.b, (size_t)l.n);
}
static void msg(const char *a) { say(a, 0, 0, 0); }
static void msg_err(const char *what, long err) { say(what, " failed, errno ", -err, 1); }
static int fail(int code, const char *what, long err)
{
    msg_err(what, err);
    return code;
}

/* Whole-packet blocking write to stdout. Returns 0, or -1 when the reader is gone (EPIPE etc.). */
static int out_all(const uint8_t *p, size_t n)
{
    while (n) {
        long r = sys_write(1, p, n);
        if (r == -EINTR) continue;
        if (r == -EAGAIN) {   /* stdout happens to be non-blocking: wait instead of spinning */
            struct pollfd_k f = {1, POLLOUT, 0};
            sys_ppoll(&f, 1, 0);
            continue;
        }
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* ---------------------------------------------------------------- options */

struct opts {
    long height, bitrate, fps, max_fps, gop_seconds;
    const char *source, *encoder;
    int probe, selftest;
};

static int parse_args(int argc, char **argv, struct opts *o)
{
    o->height = 0, o->bitrate = 3000000, o->fps = 0, o->max_fps = 45, o->gop_seconds = 4;
    o->source = o->encoder = 0;
    o->probe = o->selftest = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        long *num = 0;
        long lo = 1, hi = 0x7fffffff;
        if (str_eq(a, "--probe")) { o->probe = 1; continue; }
        if (str_eq(a, "--selftest")) { o->selftest = 1; continue; }
        if (i + 1 >= argc) return 0;
        if (str_eq(a, "--source")) { o->source = argv[++i]; continue; }
        if (str_eq(a, "--encoder")) { o->encoder = argv[++i]; continue; }
        if (str_eq(a, "--height")) num = &o->height, lo = 2;
        else if (str_eq(a, "--bitrate")) num = &o->bitrate, lo = 10000;
        else if (str_eq(a, "--fps")) num = &o->fps, hi = 240;
        else if (str_eq(a, "--max-fps")) num = &o->max_fps, hi = 240;
        else if (str_eq(a, "--gop-seconds")) num = &o->gop_seconds, hi = 3600;
        else return 0;
        if (!parse_uint(argv[++i], num) || *num < lo || *num > hi) return 0;
    }
    return !(o->probe && o->selftest);
}

static const char *getenv_(char **envp, const char *name)
{
    size_t n = str_len(name);
    for (; envp && *envp; envp++) {
        const char *e = *envp;
        size_t i = 0;
        while (i < n && e[i] == name[i]) i++;
        if (i == n && e[n] == '=') return e + n + 1;
    }
    return 0;
}

static void *alloc(size_t n)
{
    void *p = sys_mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mmap_failed(p) ? 0 : p;
}

/* ---------------------------------------------------------------- converter self-test */

static void fill_image(uint8_t *p, int w, int h, int stride, int kind)
{
    uint32_t s = 12345;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *q = p + (size_t)y * stride + x * 3;
            uint8_t r = 0, g = 0, b = 0;
            switch (kind) {
            case 0: /* 32-bit LCG noise */
                s = s * 1664525u + 1013904223u, r = (uint8_t)(s >> 24);
                s = s * 1664525u + 1013904223u, g = (uint8_t)(s >> 24);
                s = s * 1664525u + 1013904223u, b = (uint8_t)(s >> 24);
                break;
            case 1: r = g = b = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1)); break;   /* horizontal gradient */
            case 2: break;                                                    /* black */
            case 3: r = g = b = 255; break;                                   /* white */
            case 4: r = 255; break;
            case 5: g = 255; break;
            case 6: b = 255; break;
            default: r = g = b = ((x ^ y) & 1) ? 255 : 0; break;              /* 1-pixel checkerboard */
            }
            q[0] = r, q[1] = g, q[2] = b;
        }
}

static int selftest(void)
{
    static const int sizes[4][2] = {{1920, 1080}, {1922, 1080}, {333, 201}, {64, 64}};
    static const int heights[5] = {0, 720, 480, 360, 100};
    size_t src_cap = (size_t)(1922 * 3 + 13) * 1080, plane_cap = (size_t)(1922 + 37) * 1080;
    uint8_t *src = alloc(src_cap), *ya = alloc(plane_cap), *yb = alloc(plane_cap);
    uint8_t *uva = alloc(plane_cap), *uvb = alloc(plane_cap);
    void *scratch = alloc(fp_scratch_size(1922, 1080, 1922, 1080) + 4096);
    if (!src || !ya || !yb || !uva || !uvb || !scratch) return fail(1, "selftest memory", -ENOMEM);
    int cases = 0;
    for (int s = 0; s < 4; s++)
        for (int k = 0; k < 8; k++) {
            int w = sizes[s][0], h = sizes[s][1], stride = w * 3 + 13;   /* odd stride: catches stride bugs */
            fill_image(src, w, h, stride, k);
            for (int hi = 0; hi < 5; hi++) {
                if (heights[hi] > h) continue;
                int ow, oh;
                fp_out_size(w, h, heights[hi], &ow, &oh);
                int ys = ow + 37, uvs = ow + 21;
                memset(ya, 0x5a, (size_t)ys * oh), memset(yb, 0x5a, (size_t)ys * oh);
                memset(uva, 0x5a, (size_t)uvs * oh / 2), memset(uvb, 0x5a, (size_t)uvs * oh / 2);
                fp_convert_scalar(src, w, h, stride, ya, ys, uva, uvs, ow, oh, scratch);
                fp_convert(src, w, h, stride, yb, ys, uvb, uvs, ow, oh, scratch);
                cases++;
                for (int pl = 0; pl < 2; pl++) {
                    const uint8_t *a = pl ? uva : ya, *b = pl ? uvb : yb;
                    int st = pl ? uvs : ys, rows = pl ? oh / 2 : oh;
                    for (int y = 0; y < rows; y++)
                        for (int x = 0; x < st; x++) {
                            if (a[(size_t)y * st + x] == b[(size_t)y * st + x]) continue;
                            struct line l = {.n = 0};
                            ls(&l, TAG "selftest mismatch: ");
                            ln(&l, w), ls(&l, "x"), ln(&l, h), ls(&l, " -> "), ln(&l, ow), ls(&l, "x"), ln(&l, oh);
                            ls(&l, " image "), ln(&l, k), ls(&l, pl ? " plane UV" : " plane Y");
                            ls(&l, " x="), ln(&l, x), ls(&l, " y="), ln(&l, y);
                            ls(&l, " scalar="), ln(&l, a[(size_t)y * st + x]);
                            ls(&l, " fast="), ln(&l, b[(size_t)y * st + x]), ls(&l, "\n");
                            write_all(2, l.b, (size_t)l.n);
                            return 1;
                        }
                }
            }
        }
    struct line ok = {.n = 0};
    ls(&ok, TAG "selftest ok ("), ln(&ok, cases), ls(&ok, " cases)\n");
    write_all(2, ok.b, (size_t)ok.n);
    /* Speed of the real path on the real source size. */
    fill_image(src, 1920, 1080, 5760, 0);
    for (int t = 0; t < 2; t++) {
        int ow, oh;
        fp_out_size(1920, 1080, t ? 720 : 1080, &ow, &oh);
        int64_t t0 = now_ns();
        for (int i = 0; i < 50; i++) fp_convert(src, 1920, 1080, 5760, ya, ow, uva, ow, ow, oh, scratch);
        struct line l = {.n = 0};
        ls(&l, TAG "selftest 1920x1080 -> "), ln(&l, oh), ls(&l, ": "), lms(&l, (now_ns() - t0) / 50), ls(&l, " ms\n");
        write_all(2, l.b, (size_t)l.n);
    }
    return 0;
}

/* ---------------------------------------------------------------- devices */

static int read_small(const char *path, char *buf, int cap)
{
    int fd = sys_open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, (size_t)cap - 1);
    sys_close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = 0;
    return (int)n;
}

/* "/sys/class/video4linux/video<n>/name" / "/dev/video<n>" */
static void node_path(char *out, const char *pre, int n, const char *post)
{
    struct line l = {.n = 0};
    ls(&l, pre), ln(&l, n), ls(&l, post);
    memcpy(out, l.b, (size_t)l.n);
    out[l.n] = 0;
}

static uint32_t v4l2_caps(int fd)
{
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (sys_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) return 0;
    return (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
}

static int enc_caps_ok(uint32_t c) { return (c & V4L2_CAP_VIDEO_M2M_MPLANE) && (c & V4L2_CAP_STREAMING); }

static int find_source(char *path)
{
    char name[128], sys[96];
    for (int n = 0; n < 256; n++) {
        node_path(sys, "/sys/class/video4linux/video", n, "/name");
        if (read_small(sys, name, sizeof name) >= 0 && str_eq(name, "SteamVR")) {
            node_path(path, "/dev/video", n, "");
            return 1;
        }
    }
    return 0;
}

static int find_encoder(char *path)
{
    char link[64];
    long n = sys_readlink("/dev/video-enc0", link, sizeof link - 1);
    if (n > 0) {   /* report the real node (/dev/video23), as the PC side shows it */
        link[n] = 0;
        struct line l = {.n = 0};
        if (link[0] != '/') ls(&l, "/dev/");
        ls(&l, link);
        memcpy(path, l.b, (size_t)l.n);
        path[l.n] = 0;
        return 1;
    }
    int fd = sys_open("/dev/video-enc0", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        sys_close(fd);
        memcpy(path, "/dev/video-enc0", 16);
        return 1;
    }
    char name[128], sys[96];
    for (int i = 0; i < 256; i++) {
        node_path(sys, "/sys/class/video4linux/video", i, "/name");
        if (read_small(sys, name, sizeof name) < 0 || !str_contains(name, "encoder")) continue;
        node_path(path, "/dev/video", i, "");
        fd = sys_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        int ok = enc_caps_ok(v4l2_caps(fd));
        sys_close(fd);
        if (ok) return 1;
    }
    return 0;
}

/* Panel refresh in whole Hz via DRM KMS (read-only ioctls, no master needed); 0 = unknown. */
static int panel_refresh(void)
{
    int fd = sys_open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    struct drm_mode_card_res res;
    uint32_t ids[16];
    int best = 0;
    uint64_t best_area = 0;
    memset(&res, 0, sizeof res);
    if (sys_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 && res.count_crtcs) {
        uint32_t n = res.count_crtcs > 16 ? 16 : res.count_crtcs;
        memset(&res, 0, sizeof res);
        res.count_crtcs = n;   /* other counts 0: the kernel copies only the arrays we provide room for */
        res.crtc_id_ptr = (uint64_t)(uintptr_t)ids;
        if (sys_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
            if (res.count_crtcs < n) n = res.count_crtcs;
            for (uint32_t i = 0; i < n; i++) {
                struct drm_mode_crtc c;
                memset(&c, 0, sizeof c);
                c.crtc_id = ids[i];
                if (sys_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0 || !c.mode_valid) continue;
                uint64_t area = (uint64_t)c.mode.hdisplay * c.mode.vdisplay;
                uint64_t tot = (uint64_t)c.mode.htotal * c.mode.vtotal;
                if (area <= best_area || !tot) continue;
                uint64_t mhz = (uint64_t)c.mode.clock * 1000000 / tot;
                best_area = area;
                best = (int)((mhz + 500) / 1000);
            }
        }
    }
    sys_close(fd);
    return best;
}

/* H.264 Table A-1 (MaxMBPS, MaxFS, MaxBR in 1000 bit/s; High profile allows 1.25x MaxBR). */
static int pick_level(int w, int h, int fps, long bitrate)
{
    static const struct { int level; long mbps, fs, br; } t[] = {
        {V4L2_MPEG_VIDEO_H264_LEVEL_3_1, 108000, 3600, 14000}, {V4L2_MPEG_VIDEO_H264_LEVEL_3_2, 216000, 5120, 20000},
        {V4L2_MPEG_VIDEO_H264_LEVEL_4_0, 245760, 8192, 20000}, {V4L2_MPEG_VIDEO_H264_LEVEL_4_1, 245760, 8192, 50000},
        {V4L2_MPEG_VIDEO_H264_LEVEL_4_2, 522240, 8704, 50000}, {V4L2_MPEG_VIDEO_H264_LEVEL_5_0, 589824, 22080, 135000},
        {V4L2_MPEG_VIDEO_H264_LEVEL_5_1, 983040, 36864, 240000}, {V4L2_MPEG_VIDEO_H264_LEVEL_5_2, 2073600, 36864, 240000},
    };
    long fs = (long)((w + 15) / 16) * ((h + 15) / 16);
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (fs <= t[i].fs && fs * fps <= t[i].mbps && bitrate <= t[i].br * 1250) return t[i].level;
    return V4L2_MPEG_VIDEO_H264_LEVEL_5_2;
}

static int set_ctrl(int fd, uint32_t id, int32_t value, const char *name, int quiet)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;
    memset(&c, 0, sizeof c);
    memset(&cs, 0, sizeof cs);
    c.id = id;
    c.value = value;
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = &c;
    int r = sys_ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs);
    if (r < 0 && !quiet) {
        struct line l = {.n = 0};
        ls(&l, TAG "warning: "), ls(&l, name), ls(&l, " refused ("), ln(&l, -r), ls(&l, ")\n");
        write_all(2, l.b, (size_t)l.n);
    }
    return r;
}

struct src {
    int fd, w, h, stride;
    uint8_t *mem[SRC_BUFS];
    uint32_t len[SRC_BUFS];
    int held;          /* index of the dequeued (newest) buffer we keep, -1 = none yet */
    int fresh;         /* the held picture has not been encoded yet */
};

struct enc {
    int fd, stride, hpad;
    uint32_t out_size, cap_size;
    uint8_t *out_mem[ENC_BUFS], *cap_mem[ENC_BUFS];
    uint32_t out_len[ENC_BUFS], cap_len[ENC_BUFS];
    int out_free[ENC_BUFS];
    long out_pic[ENC_BUFS];   /* which picture each OUTPUT buffer holds (0 = black, -1 = none): a repeat that finds
                               * its picture already in the buffer skips the conversion (standby, still scenes) */
};

static int src_open(struct src *s, const char *path)
{
    s->held = -1, s->fresh = 0;
    s->fd = sys_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) return fail(3, "open source", s->fd);
    uint32_t c = v4l2_caps(s->fd);
    if (!(c & V4L2_CAP_VIDEO_CAPTURE) || !(c & V4L2_CAP_STREAMING)) return fail(3, "source capabilities", -EINVAL);
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int r = sys_ioctl(s->fd, VIDIOC_G_FMT, &f);
    if (r < 0) return fail(3, "source VIDIOC_G_FMT", r);
    if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB24) {
        msg("source pixel format is not RGB24");
        return 4;
    }
    s->w = (int)f.fmt.pix.width, s->h = (int)f.fmt.pix.height;
    s->stride = f.fmt.pix.bytesperline ? (int)f.fmt.pix.bytesperline : s->w * 3;
    if (s->w < 2 || s->h < 2 || s->stride < s->w * 3) return fail(4, "source size", -EINVAL);
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = SRC_BUFS, rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, rb.memory = V4L2_MEMORY_MMAP;
    r = sys_ioctl(s->fd, VIDIOC_REQBUFS, &rb);
    /* The driver may grant fewer (the Frame's v4l2loopback has max_buffers=2); two are enough: one held, one filling. */
    if (r < 0 || rb.count < 2) return fail(3, "source VIDIOC_REQBUFS", r < 0 ? r : -ENOMEM);
    if (rb.count > SRC_BUFS) rb.count = SRC_BUFS;
    for (int i = 0; i < (int)rb.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.index = (uint32_t)i, b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, b.memory = V4L2_MEMORY_MMAP;
        r = sys_ioctl(s->fd, VIDIOC_QUERYBUF, &b);
        if (r < 0) return fail(3, "source VIDIOC_QUERYBUF", r);
        if (b.length < (uint32_t)(s->stride * (s->h - 1) + s->w * 3)) return fail(3, "source buffer size", -EINVAL);
        s->mem[i] = sys_mmap(0, b.length, PROT_READ, MAP_SHARED, s->fd, b.m.offset);
        if (mmap_failed(s->mem[i])) return fail(3, "source mmap", (long)s->mem[i]);
        s->len[i] = b.length;
        r = sys_ioctl(s->fd, VIDIOC_QBUF, &b);
        if (r < 0) return fail(3, "source VIDIOC_QBUF", r);
    }
    return 0;
}

/* Drain every ready source buffer, keeping only the newest one. */
static void src_drain(struct src *s)
{
    for (;;) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, b.memory = V4L2_MEMORY_MMAP;
        if (sys_ioctl(s->fd, VIDIOC_DQBUF, &b) < 0) return;
        if (b.flags & V4L2_BUF_FLAG_ERROR) {
            sys_ioctl(s->fd, VIDIOC_QBUF, &b);
            continue;
        }
        if (s->held >= 0) {
            struct v4l2_buffer q;
            memset(&q, 0, sizeof q);
            q.index = (uint32_t)s->held, q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, q.memory = V4L2_MEMORY_MMAP;
            sys_ioctl(s->fd, VIDIOC_QBUF, &q);
        }
        s->held = (int)b.index;
        s->fresh = 1;
    }
}

/* ts_us: OUTPUT buffers carry their slot time (the encoder copies it to the coded buffer; rate control may use it). */
static int enc_queue_buf(struct enc *e, uint32_t type, int i, uint32_t used, int64_t ts_us)
{
    struct v4l2_plane p;
    struct v4l2_buffer b;
    memset(&p, 0, sizeof p);
    memset(&b, 0, sizeof b);
    b.index = (uint32_t)i, b.type = type, b.memory = V4L2_MEMORY_MMAP, b.field = V4L2_FIELD_NONE;
    b.m.planes = &p, b.length = 1;
    b.timestamp.tv_sec = (long)(ts_us / 1000000), b.timestamp.tv_usec = (long)(ts_us % 1000000);
    p.bytesused = used;
    p.length = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? e->out_len[i] : e->cap_len[i];
    return sys_ioctl(e->fd, VIDIOC_QBUF, &b);
}

static int enc_open(struct enc *e, const char *path, int w, int h, int fps, const struct opts *o)
{
    e->fd = sys_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (e->fd < 0) return fail(4, "open encoder", e->fd);
    if (!enc_caps_ok(v4l2_caps(e->fd))) return fail(4, "encoder capabilities", -EINVAL);

    /* dev-encoder.rst "Initialization": the coded format first, then the raw format. */
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.width = (uint32_t)w, f.fmt.pix_mp.height = (uint32_t)h, f.fmt.pix_mp.num_planes = 1;
    int r = sys_ioctl(e->fd, VIDIOC_S_FMT, &f);
    if (r < 0) return fail(4, "encoder VIDIOC_S_FMT (H264)", r);
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264) {
        msg("encoder refused H264");
        return 4;
    }
    e->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;

    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.width = (uint32_t)w, f.fmt.pix_mp.height = (uint32_t)h, f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709, f.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
    f.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE, f.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    r = sys_ioctl(e->fd, VIDIOC_S_FMT, &f);
    if (r < 0) return fail(4, "encoder VIDIOC_S_FMT (NV12)", r);
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 || f.fmt.pix_mp.num_planes != 1)
    {
        msg("encoder refused single-plane NV12");
        return 4;
    }
    e->stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;
    e->hpad = (int)f.fmt.pix_mp.height;
    e->out_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (e->stride < w || e->hpad < h || (uint64_t)e->stride * e->hpad * 3 / 2 > e->out_size)
        return fail(4, "encoder NV12 layout", -EINVAL);

    /* Visible rectangle (the padded height is not picture). The selection API takes the single-planar type. */
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof sel);
    sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, sel.target = V4L2_SEL_TGT_CROP;
    sel.r.width = (uint32_t)w, sel.r.height = (uint32_t)h;
    r = sys_ioctl(e->fd, VIDIOC_S_SELECTION, &sel);
    if (r < 0) {
        sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        sel.r.left = sel.r.top = 0, sel.r.width = (uint32_t)w, sel.r.height = (uint32_t)h;
        r = sys_ioctl(e->fd, VIDIOC_S_SELECTION, &sel);
    }
    if (r < 0) {
        memset(&sel, 0, sizeof sel);
        sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, sel.target = V4L2_SEL_TGT_CROP;
        int g = sys_ioctl(e->fd, VIDIOC_G_SELECTION, &sel);
        if (g < 0 || sel.r.left || sel.r.top || sel.r.width != (uint32_t)w || sel.r.height != (uint32_t)h)
            return fail(4, "encoder VIDIOC_S_SELECTION", r);
        msg_err("warning: VIDIOC_S_SELECTION (crop already right)", r);
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1, parm.parm.output.timeperframe.denominator = (uint32_t)fps;
    r = sys_ioctl(e->fd, VIDIOC_S_PARM, &parm);
    if (r < 0) msg_err("warning: VIDIOC_S_PARM", r);

    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "H264_PROFILE", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, pick_level(w, h, fps, o->bitrate), "H264_LEVEL", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "BITRATE_MODE", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)o->bitrate, "BITRATE", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, (int32_t)o->bitrate, "BITRATE_PEAK", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "FRAME_RC_ENABLE", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "B_FRAMES", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int32_t)(fps * o->gop_seconds), "GOP_SIZE", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "HEADER_MODE", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "PREPEND_SPSPPS_TO_IDR", 0);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_FRAME_SKIP_MODE, V4L2_MPEG_VIDEO_FRAME_SKIP_MODE_DISABLED, "FRAME_SKIP_MODE", 0);

    for (int q = 0; q < 2; q++) {
        uint32_t type = q ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct v4l2_requestbuffers rb;
        memset(&rb, 0, sizeof rb);
        rb.count = ENC_BUFS, rb.type = type, rb.memory = V4L2_MEMORY_MMAP;
        r = sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb);
        if (r < 0 || rb.count < ENC_BUFS) return fail(4, "encoder VIDIOC_REQBUFS", r < 0 ? r : -ENOMEM);
        for (int i = 0; i < ENC_BUFS; i++) {
            struct v4l2_plane p;
            struct v4l2_buffer b;
            memset(&p, 0, sizeof p);
            memset(&b, 0, sizeof b);
            b.index = (uint32_t)i, b.type = type, b.memory = V4L2_MEMORY_MMAP, b.m.planes = &p, b.length = 1;
            r = sys_ioctl(e->fd, VIDIOC_QUERYBUF, &b);
            if (r < 0) return fail(4, "encoder VIDIOC_QUERYBUF", r);
            uint8_t *m = sys_mmap(0, p.length, q ? PROT_READ : PROT_READ | PROT_WRITE, MAP_SHARED, e->fd, p.m.mem_offset);
            if (mmap_failed(m)) return fail(4, "encoder mmap", (long)m);
            if (q) {
                e->cap_mem[i] = m, e->cap_len[i] = p.length;
            } else {
                if (p.length < e->out_size) return fail(4, "encoder OUTPUT buffer size", -EINVAL);
                e->out_mem[i] = m, e->out_len[i] = p.length, e->out_free[i] = 1;
                /* Black, including the padding rows the encoder may read. */
                memset(m, 16, (size_t)e->stride * e->hpad);
                memset(m + (size_t)e->stride * e->hpad, 128, (size_t)e->stride * e->hpad / 2);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- streaming */

struct stats { long frames, repeats, late, skipped, errors; uint64_t bytes; };

/* Dequeue finished OUTPUT buffers (free again) and coded CAPTURE buffers (written to stdout, re-queued).
 * Returns 1 when the encoder signalled the last buffer, -1 when stdout is gone, else 0. */
static int enc_reap(struct enc *e, struct stats *st)
{
    for (;;) {
        struct v4l2_plane p;
        struct v4l2_buffer b;
        memset(&p, 0, sizeof p);
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, b.memory = V4L2_MEMORY_MMAP, b.m.planes = &p, b.length = 1;
        if (sys_ioctl(e->fd, VIDIOC_DQBUF, &b) < 0) break;
        if (b.index < ENC_BUFS) e->out_free[b.index] = 1;
    }
    for (;;) {
        struct v4l2_plane p;
        struct v4l2_buffer b;
        memset(&p, 0, sizeof p);
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.memory = V4L2_MEMORY_MMAP, b.m.planes = &p, b.length = 1;
        int r = sys_ioctl(e->fd, VIDIOC_DQBUF, &b);
        if (r == -EPIPE) return 1;   /* the last buffer was already dequeued (dev-encoder.rst "Drain") */
        if (r < 0 || b.index >= ENC_BUFS) return 0;
        int last = (b.flags & V4L2_BUF_FLAG_LAST) != 0;
        if (b.flags & V4L2_BUF_FLAG_ERROR) {
            st->errors++;
        } else if (p.bytesused > p.data_offset && p.bytesused <= e->cap_len[b.index]) {
            uint32_t n = p.bytesused - p.data_offset;
            if (out_all(e->cap_mem[b.index] + p.data_offset, n) < 0) return -1;
            st->bytes += n;
        }
        if (last) return 1;
        enc_queue_buf(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (int)b.index, 0, 0);
    }
}

static void drain_and_close(struct enc *e, struct src *s, struct stats *st)
{
    struct v4l2_encoder_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cmd = V4L2_ENC_CMD_STOP;
    if (sys_ioctl(e->fd, VIDIOC_ENCODER_CMD, &cmd) == 0) {
        int64_t end = now_ns() + 1000000000;
        for (;;) {
            if (enc_reap(e, st) != 0) break;
            int64_t left = end - now_ns();
            if (left <= 0) break;
            struct timespec ts = {left / 1000000000, left % 1000000000};
            struct pollfd_k f = {e->fd, POLLIN, 0};
            sys_ppoll(&f, 1, &ts);
        }
    }
    uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    sys_ioctl(e->fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    sys_ioctl(e->fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sys_ioctl(s->fd, VIDIOC_STREAMOFF, &t);
    sys_close(e->fd);
    sys_close(s->fd);
}

static void info_json(struct line *l, const char *enc, const char *srcp, const struct src *s, int ow, int oh,
                      int refresh, int fps, long bitrate)
{
    ls(l, "{\"encoder\":\""), ls(l, enc), ls(l, "\",\"source\":\""), ls(l, srcp);
    ls(l, "\",\"src\":["), ln(l, s->w), ls(l, ","), ln(l, s->h), ls(l, "],\"out\":["), ln(l, ow), ls(l, ","), ln(l, oh);
    ls(l, "],\"refresh\":"), ln(l, refresh), ls(l, ",\"fps\":"), ln(l, fps), ls(l, ",\"bitrate\":"), ln(l, bitrate);
    ls(l, "}\n");
}

static void stats_line(const struct stats *st, int64_t elapsed)
{
    struct line l = {.n = 0};
    ls(&l, TAG "stats frames="), ln(&l, st->frames), ls(&l, " repeats="), ln(&l, st->repeats);
    ls(&l, " late="), ln(&l, st->late), ls(&l, " skipped="), ln(&l, st->skipped);
    ls(&l, " kbps="), ln(&l, elapsed > 0 ? (long)(st->bytes * 8 * 1000000 / (uint64_t)elapsed) : 0);
    if (st->errors) ls(&l, " errors="), ln(&l, st->errors);
    ls(&l, "\n");
    write_all(2, l.b, (size_t)l.n);
}

int main(int argc, char **argv, char **envp)
{
    struct opts o;
    if (!parse_args(argc, argv, &o)) {
        msg("usage: fp_venc [--height N] [--bitrate BPS] [--fps N] [--max-fps N] [--gop-seconds N] "
            "[--source PATH] [--encoder PATH] [--probe | --selftest]");
        return 2;
    }
    if (o.selftest) return selftest();
    const char *dis = getenv_(envp, "FP_VENC_DISABLE");
    if (dis && str_eq(dis, "1")) {
        msg("hardware encoder disabled (FP_VENC_DISABLE=1)");
        return 4;
    }

    char src_path[256], enc_path[256];
    if (o.source) {
        if (str_len(o.source) >= sizeof src_path) return 2;
        memcpy(src_path, o.source, str_len(o.source) + 1);
    } else if (!find_source(src_path)) {
        msg("no SteamVR headset view device (/sys/class/video4linux/*/name == SteamVR)");
        return 3;
    }
    struct src s;
    int r = src_open(&s, src_path);
    if (r) return r;

    if (o.encoder) {
        if (str_len(o.encoder) >= sizeof enc_path) return 2;
        memcpy(enc_path, o.encoder, str_len(o.encoder) + 1);
    } else if (!find_encoder(enc_path)) {
        msg("no V4L2 hardware encoder found");
        return 4;
    }

    int ow, oh;
    fp_out_size(s.w, s.h, (int)o.height, &ow, &oh);
    int refresh = panel_refresh();
    int fps = (int)o.fps;
    if (!fps) {
        if (refresh > 0) {
            int d = (refresh + (int)o.max_fps - 1) / (int)o.max_fps;
            fps = (refresh + d / 2) / d;
        } else {
            fps = 30;
        }
    }

    struct enc e;
    memset(&e, 0, sizeof e);
    r = enc_open(&e, enc_path, ow, oh, fps, &o);
    if (r) return r;

    struct line info = {.n = 0};
    info_json(&info, enc_path, src_path, &s, ow, oh, refresh, fps, o.bitrate);
    if (o.probe) {
        write_all(1, info.b, (size_t)info.n);
        sys_close(e.fd);
        sys_close(s.fd);
        return 0;
    }
    struct line il = {.n = 0};
    ls(&il, TAG "info "), ls(&il, info.b);
    write_all(2, il.b, (size_t)il.n);

    void *scratch = alloc(fp_scratch_size(s.w, s.h, ow, oh));
    if (!scratch) return fail(4, "scratch memory", -ENOMEM);

    /* Ignore SIGPIPE: a closed stdout becomes EPIPE from write() and we stop quietly. */
    struct { void *handler; unsigned long flags; void *restorer; uint64_t mask; } sa = {(void *)1, 0, 0, 0};
    sys6(__NR_rt_sigaction, 13 /* SIGPIPE */, (long)&sa, 0, 8, 0, 0);

    for (int i = 0; i < ENC_BUFS; i++)
        if ((r = enc_queue_buf(&e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, 0, 0)) < 0) return fail(4, "encoder QBUF", r);
    uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if ((r = sys_ioctl(e.fd, VIDIOC_STREAMON, &t)) < 0) return fail(4, "encoder STREAMON (OUTPUT)", r);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if ((r = sys_ioctl(e.fd, VIDIOC_STREAMON, &t)) < 0) return fail(4, "encoder STREAMON (CAPTURE)", r);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ((r = sys_ioctl(s.fd, VIDIOC_STREAMON, &t)) < 0) return fail(3, "source STREAMON", r);

    struct stats st = {0, 0, 0, 0, 0, 0};
    const int64_t t0 = now_ns(), period = 1000000000 / fps;
    int64_t next_stats = t0 + 10000000000LL, quiet_src = 0, quiet_enc = 0;
    long slot = 0;                 /* next slot to submit; due at t0 + slot * 1e9 / fps */
    long pic = 0;                  /* current picture: 0 = black (no source frame yet), +1 per new source frame */
    for (int i = 0; i < ENC_BUFS; i++) e.out_pic[i] = -1;
    int want_key = 0, stdin_open = 1, sent_any = 0, rc = 0;

    for (;;) {
        int64_t now = now_ns();
        long due_upto = (long)((now - t0) * fps / 1000000000);   /* last slot whose time has come */
        if (due_upto - slot + 1 > fps) {   /* > 1 s behind (e.g. SIGSTOP): don't replay it all */
            say("fell behind; skipping slots: ", 0, due_upto - slot, 1);
            st.skipped += due_upto - slot;
            slot = due_upto;
        }
        /* Submit every due slot for which an OUTPUT buffer is free. */
        while (slot <= due_upto) {
            int i = 0;
            while (i < ENC_BUFS && !e.out_free[i]) i++;
            if (i == ENC_BUFS) break;
            uint8_t *m = e.out_mem[i];
            if (s.held >= 0 && s.fresh) pic++;
            if (e.out_pic[i] != pic) {
                if (s.held >= 0) {
                    fp_convert(s.mem[s.held], s.w, s.h, s.stride, m, e.stride, m + (size_t)e.stride * e.hpad,
                               e.stride, ow, oh, scratch);
                } else {   /* no source frame yet: black */
                    memset(m, 16, (size_t)e.stride * oh);
                    memset(m + (size_t)e.stride * e.hpad, 128, (size_t)e.stride * oh / 2);
                }
                e.out_pic[i] = pic;
            }
            if (!s.fresh && sent_any) st.repeats++;
            s.fresh = 0;
            if (want_key) set_ctrl(e.fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "FORCE_KEY_FRAME", 0), want_key = 0;
            if ((r = enc_queue_buf(&e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i, e.out_size, slot * 1000000 / fps)) < 0) {
                rc = fail(1, "encoder QBUF (OUTPUT)", r);
                goto stop;
            }
            e.out_free[i] = 0;
            if (now_ns() >= t0 + (slot + 1) * 1000000000 / fps) st.late++;
            st.frames++, slot++, sent_any = 1;
        }
        if (now >= next_stats) {
            stats_line(&st, now - t0);
            next_stats += 10000000000LL;
        }

        /* Wait for the next slot, a source frame, encoder progress or a stdin command. If a slot is already due we
         * are waiting for a free OUTPUT buffer: the encoder fd's POLLOUT wakes us. */
        int64_t wait = slot <= due_upto ? period : t0 + slot * 1000000000 / fps - now;
        if (wait < 0) wait = 0;
        struct pollfd_k f[3];
        int nf = 0, isrc = -1, ienc = -1, iin = -1;
        if (now >= quiet_src) isrc = nf, f[nf++] = (struct pollfd_k){s.fd, POLLIN, 0};
        if (now >= quiet_enc) ienc = nf, f[nf++] = (struct pollfd_k){e.fd, POLLIN | POLLOUT, 0};
        if (stdin_open) iin = nf, f[nf++] = (struct pollfd_k){0, POLLIN, 0};
        /* An fd left out after an error comes back when its quiet time ends. */
        if (now < quiet_src && quiet_src - now < wait) wait = quiet_src - now;
        if (now < quiet_enc && quiet_enc - now < wait) wait = quiet_enc - now;
        struct timespec ts = {wait / 1000000000, wait % 1000000000};
        int n = sys_ppoll(f, (unsigned)nf, &ts);
        if (n < 0 && n != -EINTR) {
            rc = fail(1, "ppoll", n);
            goto stop;
        }
        if (n <= 0) continue;
        if (isrc >= 0 && f[isrc].revents) {
            int had = s.held;
            int was_fresh = s.fresh;
            src_drain(&s);
            /* POLLERR without a frame (e.g. the writer is gone): don't spin on it, look again in 100 ms. */
            if ((f[isrc].revents & (POLLERR | POLLHUP | POLLNVAL)) && s.held == had && s.fresh == was_fresh)
                quiet_src = now + 100000000;
        }
        if (ienc >= 0 && f[ienc].revents) {
            int k = enc_reap(&e, &st);
            if (k < 0) goto stop_quiet;
            if (k > 0) {
                rc = fail(1, "encoder stopped unexpectedly", -EIO);
                goto stop;
            }
            if (f[ienc].revents & (POLLERR | POLLNVAL)) quiet_enc = now + 2000000;
        }
        if (iin >= 0 && f[iin].revents) {
            if (f[iin].revents & POLLNVAL) {
                stdin_open = 0;
            } else {
                char cmd[64];
                long got = sys_read(0, cmd, sizeof cmd);
                if (got == 0) goto stop;            /* EOF: the PC side closed the channel */
                for (long k = 0; k < got; k++) {
                    if (cmd[k] == 'k') want_key = 1;
                    if (cmd[k] == 'q') goto stop;
                }
            }
        }
    }

stop:
    drain_and_close(&e, &s, &st);
    stats_line(&st, now_ns() - t0);
    return rc;
stop_quiet:   /* stdout is gone: nothing left to drain into */
    {
        uint32_t ty = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        sys_ioctl(e.fd, VIDIOC_STREAMOFF, &ty);
        ty = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        sys_ioctl(e.fd, VIDIOC_STREAMOFF, &ty);
        sys_close(e.fd);
        sys_close(s.fd);
    }
    return 0;
}
