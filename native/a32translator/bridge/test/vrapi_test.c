/* Linked only into the qemu host harness. Exercise the real AAPCS readers and thunks with a fake 64-bit API. */
#include <sys/mman.h>
static int test_tracking_calls, test_submit_calls;
static int test_tracked = 1, test_identity, test_current_only;
static posef64 test_set_pose;
static tracking2 test_tracking(void *m, double t)
{
    tracking2 tr = {0};
    tr.Status = test_tracked && (!test_current_only || t == 0) ? 143 : 0;
    tr.HeadPose.Orientation[3] = 1;
    ++test_tracking_calls;
    tr.HeadPose.Position[1] = (!tr.Status || test_identity) ? 1.6f : (float)test_tracking_calls;
    tr.HeadPose.TimeInSeconds = t == 0 ? 42.75 : t;
    return tr;
}
static posef64 test_transform(void *m, int which)
{
    posef64 pose = {{0,0,0,1}, {0, which == 3 ? -1.6f : 0, 0}};
    return pose;
}
static void test_set_transform(void *m, posef64 pose) { test_set_pose = pose; }
static double test_display(void *m, int64_t index) { return 42.25; }
static pose64 test_submit_pose;
static uint64_t test_submit_index;
static unsigned test_submit_layers;
static double test_submit_time;
static int test_submit(void *m, const submit2 *d)
{
    ++test_submit_calls;
    test_submit_layers = d->LayerCount;
    test_submit_index = d->FrameIndex;
    test_submit_time = d->DisplayTime;
    if (d->LayerCount == 1) test_submit_pose = ((const projection2 *)d->Layers[0])->HeadPose;
    return 0;
}
int a32_vrapi_selftest(void)
{
    int failed = 0;
#define VT(x) do { if (!(x)) { fprintf(stderr, "FAIL vrapi: %s\n", #x); ++failed; } } while (0)
    uint8_t *buf = mmap((void *)0x30000000, 8192, PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    if (buf == MAP_FAILED) return 1;
    uint32_t g = p2g(buf);
    a32_cpu cpu = {0};
    argreader r;
    cpu.r[0] = 123; cpu.r[1] = 0xdeadbeef; cpu.r[2] = 0x89abcdef; cpu.r[3] = 0x01234567;
    ar_init(&r, &cpu, 0, g);
    VT(rd32(&r) == 123); VT(rd64(&r) == UINT64_C(0x0123456789abcdef));
    bridge(); /* finish pthread_once (the static harness has no real bridge DSO) */
    __typeof__(B) saved = B;
    int saved_ok = bridge_ok;
    B.GetPredictedTracking2 = test_tracking;
    B.GetTrackingTransform = test_transform; B.SetTrackingTransform = test_set_transform;
    B.GetPredictedDisplayTime = test_display; B.SubmitFrame2 = test_submit;
    bridge_ok = 1;
    void *m = &test_set_pose;
    uint32_t mh = ptr2h(m);
    tracking_reset();
    tracking2 a = tracking_of(m, 42.25), b = tracking_of(m, 42.25);
    VT(!memcmp(&a, &b, sizeof a)); VT(test_tracking_calls == 1);
    tracking_of(m, 42.5); VT(test_tracking_calls == 2);
    cpu.r[0]=g; cpu.r[1]=mh; double requested=42.25; memcpy(&cpu.r[2],&requested,8);
    ar_init(&r,&cpu,0,g+4096); imp_vrapi_GetPredictedTracking(&cpu,&r);
    VT(!memcmp(buf,&a,104)); VT(cpu.r[0] == g);
    cpu.r[0]=g; cpu.r[1]=mh;
    ar_init(&r,&cpu,0,g+4096); imp_vrapi_GetPredictedTracking2(&cpu,&r);
    VT(!memcmp(buf,&a,sizeof a)); VT(test_tracking_calls == 2);
    tracking_reset(); test_tracked = 0;
    tracking_of(m, 42.25); tracking_of(m, 42.25); VT(test_tracking_calls == 4);
    test_tracked = 1;
    cpu.r[0] = g; cpu.r[1] = mh; cpu.r[2] = 3;
    ar_init(&r, &cpu, 0, g+4096); imp_vrapi_GetTrackingTransform(&cpu, &r);
    posef64 floor; memcpy(&floor, buf, sizeof floor);
    VT(floor.Orientation[3] == 1 && floor.Position[1] == -1.6f); VT(cpu.r[0] == g);
    posef64 wanted = {{0.1f,0.2f,0.3f,0.9f}, {2,-1.6f,4}};
    cpu.r[0] = mh;
    memcpy(&cpu.r[1], &wanted, 12); memcpy(buf+4096, (uint8_t *)&wanted+12, 16);
    ar_init(&r, &cpu, 0, g+4096); imp_vrapi_SetTrackingTransform(&cpu, &r);
    VT(!memcmp(&wanted, &test_set_pose, sizeof wanted));
    tracking_of(m, 42.25); VT(test_tracking_calls == 5); /* transform invalidated the cache */
    /* Projection2: pointer widening and 88 -> 96 byte texture stride; HeadPose is ABI-identical. */
    memset(buf, 0, 4096);
    uint32_t layer = g+128, array = g+1024;
    uint32_t chain_object = 0; uint32_t ch = ptr2h(&chain_object);
    pose64 render = {0}; render.Orientation[3] = 1; render.Position[1] = 1.75f;
    render.AngularVelocity[1] = 0.25f; render.LinearAcceleration[2] = 0.75f;
    render.TimeInSeconds = 42.25; render.PredictionInSeconds = 0.0125;
    st32(g+32,1); st32(g+36,array); st32(array,layer);
    st32(layer,1); memcpy(g2p(layer+40), &render, sizeof render);
    st32(layer+136,ch); st32(layer+224,ch);
    uint64_t index = UINT64_C(0x123456789);
    memcpy(buf+8,&index,8); double time=42.25; memcpy(buf+16,&time,8);
    cpu.r[0] = mh; cpu.r[1] = g;
    ar_init(&r,&cpu,0,g+4096); imp_vrapi_SubmitFrame2(&cpu,&r);
    VT(test_submit_calls == 1); VT(test_submit_index == index && test_submit_time == time);
    VT(!memcmp(&render,&test_submit_pose,sizeof render));
    /* Legacy VrApi 1.5: 200-byte eye textures and 432-byte layers. */
    memset(buf,0,4096); st32(g,3); st32(g+1736,1);
    st32(g+8,ch); st32(g+208,ch); memcpy(buf+8+96,&render,sizeof render);
    memcpy(buf+1744,&index,8);
    cpu.r[0]=mh; cpu.r[1]=g;
    ar_init(&r,&cpu,0,g+4096); imp_vrapi_SubmitFrame(&cpu,&r);
    VT(test_submit_calls == 2); VT(test_submit_index == index && test_submit_time == time);
    VT(!memcmp(&render,&test_submit_pose,sizeof render));
    /* The early loader's 140-byte prefix shifts the otherwise identical legacy layout. */
    memset(buf,0,4096); st32(g,0x10000001); st32(g+1876,1);
    st32(g+148,ch); st32(g+348,ch); memcpy(buf+148+96,&render,sizeof render);
    memcpy(buf+1884,&index,8);
    cpu.r[0]=mh; cpu.r[1]=g;
    ar_init(&r,&cpu,0,g+4096); imp_vrapi_SubmitFrame(&cpu,&r);
    VT(test_submit_calls == 3); VT(test_submit_index == index && test_submit_time == time);
    VT(!memcmp(&render,&test_submit_pose,sizeof render));
    /* Runtime tracking loss: hold head and eyes together for <=100 ms, while
     * keeping repeated answers consistent without renewing the original sample age. */
    tracking2 good = tracking_of(m, 42.25);
    test_tracked = 0;
    int calls = test_tracking_calls;
    tracking2 held = tracking_of(m, 42.26);
    VT(held.Status == good.Status && held.HeadPose.Position[1] == good.HeadPose.Position[1]);
    VT(held.HeadPose.TimeInSeconds == 42.26);
    VT(!memcmp(held.Eye, good.Eye, sizeof held.Eye));
    tracking_of(m, 42.26); VT(test_tracking_calls == calls+1);
    VT(tracking_of(m, 42.36).Status == 0); /* held .26 did not renew .25 */
    VT(tracking_of(m, 42.5).Status == 0);
    test_tracked = 1;
    /* Default submitted without a matching tracking request: repair at display time.
     * Both conversion paths use this gate; preserve valid arbitrary render poses. */
    projection2 projection = {0}; projection.Header.Type = 1;
    projection.HeadPose.Orientation[3] = 1; projection.HeadPose.Position[1] = 1.6f;
    projection.HeadPose.TimeInSeconds = 42.26;
    const header2 *headers[] = {&projection.Header};
    submit2 desc = {0}; desc.DisplayTime = 42.25; desc.LayerCount = 1; desc.Layers = headers;
    submit_prepare(m, &desc);
    VT(desc.LayerCount == 1 && projection.HeadPose.Position[1] == good.HeadPose.Position[1]);
    VT(projection.HeadPose.TimeInSeconds == 42.26); /* exact render-time held answer */
    /* Genuine tracked identity and head-locked identity must remain unchanged. */
    tracking_reset(); test_identity = 1;
    tracking2 identity = tracking_of(m, 42.25);
    projection.HeadPose = identity.HeadPose; submit_prepare(m, &desc);
    VT(!memcmp(&projection.HeadPose, &identity.HeadPose, sizeof(pose64)));
    tracking_reset(); test_tracked = 0;
    projection.Header.Flags = 4; submit_prepare(m, &desc); VT(desc.LayerCount == 1);
    projection.Header.Flags = 0; submit_prepare(m, &desc); VT(desc.LayerCount == 0);
    /* Check the real SubmitFrame2 thunk ends an empty frame with no tracked fallback. */
    memset(buf, 0, 4096);
    st32(g+32,1); st32(g+36,array); st32(array,layer); st32(layer,1);
    st32(layer+136,ch); st32(layer+224,ch);
    memcpy(g2p(layer+40), &identity.HeadPose, sizeof(pose64)); memcpy(buf+16,&time,8);
    cpu.r[0]=mh; cpu.r[1]=g;
    ar_init(&r,&cpu,0,g+4096); imp_vrapi_SubmitFrame2(&cpu,&r);
    VT(test_submit_calls == 4 && test_submit_layers == 0);
    test_tracked = 1; test_identity = 0; test_current_only = 1;
    desc.LayerCount = 1; projection.HeadPose = identity.HeadPose;
    submit_prepare(m, &desc);
    VT(desc.LayerCount == 1 && projection.HeadPose.TimeInSeconds == 42.75);
    VT(desc.DisplayTime == 42.25); /* no extra WaitFrame or display-time advance */
    test_current_only = 0;
    /* Keep an update/render pair across more than the old 16 cache entries. */
    tracking_reset();
    good = tracking_of(m, 42.25);
    for (int j = 1; j <= 40; ++j) tracking_of(m, 42.25+j*0.001);
    calls = test_tracking_calls;
    held = tracking_of(m, 42.25);
    VT(test_tracking_calls == calls && !memcmp(&held, &good, sizeof held));
    test_tracked = 1; test_identity = 0;
    tracking_reset(); B = saved; bridge_ok = saved_ok;
    munmap(buf,8192);
    fprintf(stderr, "vrapi ABI/cache/transform tests: %s\n", failed ? "FAILED" : "OK");
#undef VT
    return failed;
}
