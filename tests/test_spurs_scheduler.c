/* Manufactured policy modules exercise the real initializer, scheduler and
 * SPU policy execution. No game code, addresses or completion substitutions. */
#define spu_workload_dispatch_async fixture_unbounded_dispatch
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"

void spu_indirect_branch(spu_context*);
void spu_halt(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n,f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart*1000/f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
/* dxt_capture.c is not part of this fixture; spurs_policy.c calls its policy
 * hooks unconditionally. The counters are off, so these are no-ops. */
int g_dxt_capture_on;
void dxt_capture_dma(const spu_context* c,int put,uint32_t lsa,uint64_t ea,uint32_t n,const uint8_t* b)
{ (void)c;(void)put;(void)lsa;(void)ea;(void)n;(void)b; }
void dxt_capture_policy_begin(const spu_context* c,uint32_t wid,uint64_t d,uint32_t sn)
{ (void)c;(void)wid;(void)d;(void)sn; }
void dxt_capture_policy_end(const spu_context* c) { (void)c; }
static atomic_uint canceled_queues;
void sys_event_queue_cancel_by_id(u32 id) { assert(id==3 || id==4);atomic_fetch_or(&canceled_queues,1u<<id); }
void sys_event_queue_uncancel_by_id(u32 id) {(void)id;}
/* Observe the legacy bypass without substituting any policy completion. */
static atomic_uint unbounded_dispatches;
int fixture_unbounded_dispatch(const uint8_t* pm,uint32_t size,uint32_t data)
{ (void)pm;(void)size;(void)data;atomic_fetch_add(&unbounded_dispatches,1);return 0; }
static atomic_uint entered, produced, finished, release_consumer;
static atomic_uint consumer_lane,producer_lane;
static void finish(spu_context* c)
{
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    /* The manufactured guest removes itself from the runnable set. */
    vm_write8(ea+0x80+wid,3);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void consumer(spu_context* c)
{
    spu_lockline_lock();spu_coh_reserve(c,spu_ls_read32(c,0x1C4));spu_lockline_unlock();
    atomic_store(&consumer_lane,spu_ls_read32(c,0x1C8));
    atomic_fetch_add(&entered,1);
    while (!atomic_load(&produced) && !atomic_load(&release_consumer)) Sleep(1);
    assert(atomic_load(&produced));
    atomic_fetch_add(&finished,1);finish(c);
}
static void producer(spu_context* c)
{
    atomic_store(&producer_lane,spu_ls_read32(c,0x1C8));
    atomic_store(&produced,1);finish(c);
}
static void wait_for(atomic_uint* p,unsigned value,const char* what)
{
    for(unsigned i=0;i<3000;i++) { if(atomic_load(p)>=value)return;Sleep(1); }
    fprintf(stderr,"FAIL: %s: runnable producer could not run while consumer waited\n",what);exit(1);
}
static void finalize(u32 ea)
{
    for(unsigned i=0;i<3000;i++) {
        s32 rc=cellSpursFinalize((CellSpurs*)(uintptr_t)ea);
        if (!rc) return;
        assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY);Sleep(1);
    }
    assert(!"finalize failed to join after real policy return");
}
static atomic_uint held[16],done[16],gate,active_count,peak_count,lane_mask;
static void held_policy(spu_context* c)
{
    unsigned w=spu_ls_read32(c,0x1DC),lane=spu_ls_read32(c,0x1C8);
    assert(lane<3);
    atomic_fetch_or(&lane_mask,1u<<lane);
    unsigned n=atomic_fetch_add(&active_count,1)+1,peak=atomic_load(&peak_count);
    while(peak<n && !atomic_compare_exchange_weak(&peak_count,&peak,n)) {}
    atomic_fetch_add(&held[w],1);
    while(!atomic_load(&gate)) Sleep(1);
    atomic_fetch_sub(&active_count,1);atomic_fetch_add(&done[w],1);finish(c);
}
static atomic_uint fast_visits,late_seen;
static void fast_policy(spu_context* c)
{
    atomic_fetch_add(&fast_visits,1);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static atomic_uint native_runs;
static void native_policy(spu_context* c)
{
    unsigned n = atomic_fetch_add(&native_runs, 1) + 1;
    if (n == 1) {
        /* A job is loaded and not yet stored: the worker may keep the lane. */
        spu_ls_write32(c, 0x12D0, 0);
        spu_ls_write32(c, 0x12E0, 0xFFFFFFFFu);
        spu_ls_write32(c, 0x12F0, 0xFFFFFFFFu);
        c->pc = SPURS_PM_EXIT_TO_KERNEL_LS; spu_indirect_branch(c);
    } else {
        /* The rerun stored it; retire the workload so the coordinator stops. */
        spu_ls_write32(c, 0x12D0, 0xFFFFFFFFu);
        spu_ls_write32(c, 0x12E0, 0xFFFFFFFFu);
        spu_ls_write32(c, 0x12F0, 0);
        finish(c);
    }
}
static void late_policy(spu_context* c) { atomic_store(&late_seen,1);finish(c); }
static void stopped_policy(spu_context* c)
{
    vm_write8(spu_ls_read32(c,0x1C4)+0x80+spu_ls_read32(c,0x1DC),3);
    atomic_store(&late_seen,1);spu_halt(c);
}
static atomic_uint direct_enter,direct_leave,direct_exit;
static atomic_uintptr_t direct_ctx[2];
static int direct_rc[2];
static void resident_policy(spu_context* c)
{
    unsigned id=c->gpr[4]._u32[1]-1;   /* low half of the u64 argument (#309) */
    assert(id<2);atomic_store(&direct_ctx[id],(uintptr_t)c);
    spu_ls_write32(c,0x4000,0x12340000u+id);
    atomic_fetch_add(&direct_enter,1);
    while(atomic_load(&direct_enter)<2) Sleep(1);
    assert(spu_ls_read32(c,0x4000)==0x12340000u+id);
    atomic_fetch_add(&direct_leave,1);finish(c);
}
static void exit_policy(spu_context* c) { atomic_store(&direct_exit,1);finish(c); }
static void error_policy(spu_context* c)
{
    while(!atomic_load(&direct_exit)) Sleep(1);
    Sleep(10);spu_halt(c);
}
static void* direct(void* arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    direct_rc[id]=spu_run_policy_module(resident_policy,79,vm_base+0x9400,16,id+1,3,0x11000+id*0x1000,0);
    return NULL;
}
static void* error_direct(void* arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    direct_rc[id]=spu_run_policy_module(id?exit_policy:error_policy,79,vm_base+0x9400,16,id+1,3,0x11000+id*0x1000,0);
    return NULL;
}
static atomic_uint incarnation_runs,eligible_lane;
static void incarnation_policy(spu_context* c)
{
    assert(spu_ls_read32(c,0x4000)==0);
    spu_ls_write32(c,0x4000,0x9999);
    atomic_fetch_add(&incarnation_runs,1);finish(c);
}
static void eligible_policy(spu_context* c)
{
    atomic_store(&eligible_lane,spu_ls_read32(c,0x1C8)+1);finish(c);
}
static void register_pm(unsigned ea,unsigned byte,spu_lifted_entry_fn fn,int image)
{
    memset(vm_base+ea,byte,16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+ea,16),fn,image,"manufactured scheduler fixture");
}
int main(void)
{
    /* The corrected core BUSY code (0x8041080A) is what the SDK contract and
     * the guest's retry loop use; it is armed with the teardown pair. */
    setenv("PS3RECOMP_SPURS_TEARDOWN","1",1);
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    register_pm(0x9000,0x11,consumer,77);register_pm(0x9100,0x22,producer,78);
    assert((u32)cellSpursInitialize((CellSpurs*)0x1000,7,0,0,0)==CELL_SPURS_CORE_ERROR_INVAL);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)0x1000,3,(u8*)0x8010,1)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,1)==0);
    wait_for(&entered,1,"consumer admission");
    for(unsigned i=0;i<8;i++) assert(cellSpursWakeUp((CellSpurs*)0x1000)==0);
    assert(!atomic_load(&unbounded_dispatches));
    unsigned char before[0xC0];memcpy(before,vm_base+0x1000,sizeof(before));
    assert((u32)cellSpursFinalize((CellSpurs*)0x1000)==CELL_SPURS_CORE_ERROR_BUSY);
    assert(!atomic_load(&canceled_queues) && !memcmp(before,vm_base+0x1000,sizeof(before)));
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9100,16,0,NULL,1,1)==0);
    wait_for(&finished,1,"cross-workload dependency");
    assert(atomic_load(&consumer_lane)<2 && atomic_load(&producer_lane)<2);
    assert(atomic_load(&consumer_lane)!=atomic_load(&producer_lane));
    unsigned port3=vm_read8(0x8010);
    finalize(0x1000);assert(atomic_load(&canceled_queues)==(1u<<3));
    assert(!spurs_port_queue(port3));
    puts("SPURS runnable producer / blocked consumer / BUSY immutability / join PASS");

    /* Reinitialize the same EA after all handles and resident contexts retire. */
    register_pm(0x9200,0x33,held_policy,79);
    assert(cellSpursInitialize((CellSpurs*)0x1000,3,0,0,0)==0);
    for(unsigned i=0;i<4;i++) {
        assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
        assert(vm_read32(0x8000)==i);
    }
    for(unsigned i=0;i<3;i++)wait_for(&held[i],1,"configured capacity");
    Sleep(20);assert(atomic_load(&peak_count)==3 && atomic_load(&lane_mask)==7);
    assert((u32)cellSpursRemoveWorkload((CellSpurs*)0x1000,0)==CELL_SPURS_CORE_ERROR_BUSY);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,0)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==0);
    assert(!atomic_load(&held[3]));
    for(unsigned i=0;i<3;i++)assert(atomic_load(&held[i])==1);
    atomic_store(&gate,1);
    for(unsigned i=0;i<4;i++)wait_for(&done[i],1,"release and next admission");
    finalize(0x1000);puts("SPURS configured capacity / maxContention / stable physical slots / reinit PASS");

    atomic_store(&gate,0);atomic_store(&held[0],0);atomic_store(&done[0],0);
    atomic_store(&peak_count,0);atomic_store(&lane_mask,0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,3,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,2)==0);
    wait_for(&held[0],2,"maxContention two");Sleep(20);
    assert(atomic_load(&held[0])==2 && atomic_load(&peak_count)==2);
    assert(vm_read8(0x1000+SPURS_WKL_CURCONT)==2);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,0)==0);
    Sleep(20);assert(atomic_load(&held[0])==2);
    atomic_store(&gate,1);wait_for(&done[0],2,"lowered limit preserves admitted work");
    finalize(0x1000);puts("SPURS one workload has two physical lanes and obeys lowered admission limit PASS");

    register_pm(0x9600,0x66,incarnation_policy,83);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    for(unsigned incarnation=0;incarnation<2;incarnation++) {
        assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9600,16,0,NULL,1,1)==0);
        wait_for(&incarnation_runs,incarnation+1,"same policy/data workload incarnation");
        for(unsigned i=0;;i++) {
            s32 rc=cellSpursRemoveWorkload((CellSpurs*)0x1000,0);
            if(!rc)break;assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY && i<3000);Sleep(1);
        }
    }
    finalize(0x1000);puts("SPURS removal retires resident LS even with unchanged policy/data PASS");

    register_pm(0x9700,0x77,eligible_policy,84);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    memset(vm_base+0x8020,0,8);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9700,16,0,(u8*)0x8020,1,1)==0);
    Sleep(20);assert(!atomic_load(&eligible_lane));
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,5)==0);
    vm_write16(0x1000+SPURS_WKL_SIGNAL1,0x8000);
    assert(cellSpursRemoveWorkload((CellSpurs*)0x1000,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9700,16,0,(u8*)0x8020,1,1)==0);
    assert(cellSpursReadyCountSwap((CellSpurs*)0x1000,0,(u32*)0x8030,0)==0);
    assert(vm_read32(0x8030)==0 && !vm_read16(0x1000+SPURS_WKL_SIGNAL1));
    assert(cellSpursReadyCountCompareAndSwap((CellSpurs*)0x1000,0,(u32*)0x8030,5,9)==0);
    assert(vm_read32(0x8030)==0 && vm_read8(0x1000+SPURS_WKL_READY1)==0);
    assert(cellSpursReadyCountCompareAndSwap((CellSpurs*)0x1000,0,(u32*)0x8030,0,3)==0);
    assert(vm_read8(0x1000+SPURS_WKL_READY1)==3);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,0)==0);

    vm_base[0x8021]=1;
    assert(cellSpursSetPriorities((CellSpurs*)0x1000,0,(u8*)0x8020)==0);
    wait_for(&eligible_lane,1,"zero-priority physical lane exclusion");
    assert(atomic_load(&eligible_lane)==2);finalize(0x1000);
    puts("SPURS explicit zero priority excludes physical lane / SetPriorities guest publication PASS");

    register_pm(0x9300,0x44,fast_policy,80);register_pm(0x9400,0x55,late_policy,81);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9300,16,0,NULL,1,1)==0);
    wait_for(&fast_visits,2,"continuously runnable workload");
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    wait_for(&late_seen,1,"round-robin admission");
    vm_write8(0x1080,3);finalize(0x1000);puts("SPURS continuously runnable workload does not starve later producer PASS");

    /* Public API retains global workload IDs, while instances own admission,
     * worker handles, guest state and attached-queue teardown independently. */
    atomic_store(&gate,0);atomic_store(&held[0],0);atomic_store(&done[0],0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
    wait_for(&held[0],1,"first instance active");
    atomic_store(&late_seen,0);register_pm(0x9500,0x66,stopped_policy,82);
    assert(cellSpursInitialize((CellSpurs*)0x3000,1,0,0,0)==0);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)0x3000,4,(u8*)0x8010,1)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x3000,(u32*)0x8000,(void*)0x9500,16,0,NULL,1,1)==0);
    assert(vm_read32(0x8000)==1);wait_for(&late_seen,1,"independent instance stop/error execution");
    finalize(0x3000);
    assert(s_workloads[0].in_use && s_workloads[0].spurs_ea==0x1000);
    assert(vm_read8(0x1020)==1);
    atomic_store(&gate,1);wait_for(&done[0],1,"surviving instance completion");finalize(0x1000);
    puts("SPURS independent instance admission / stop return / queue ownership PASS");

    pthread_t a,b;
    assert(!pthread_create(&a,NULL,direct,(void*)0));assert(!pthread_create(&b,NULL,direct,(void*)1));
    pthread_join(a,NULL);pthread_join(b,NULL);
    assert(direct_rc[0]==0 && direct_rc[1]==0 && atomic_load(&direct_leave)==2);
    assert(atomic_load(&direct_ctx[0])!=atomic_load(&direct_ctx[1]));
    spurs_policy_release_instance(0x11000);spurs_policy_release_instance(0x12000);
    assert(!pthread_create(&a,NULL,error_direct,(void*)0));assert(!pthread_create(&b,NULL,error_direct,(void*)1));
    pthread_join(a,NULL);pthread_join(b,NULL);assert(direct_rc[0]==-2 && direct_rc[1]==0);
    spurs_policy_release_instance(0x11000);spurs_policy_release_instance(0x12000);
    puts("SPURS same workload/lane across instances retains separate LS and TLS exit classification PASS");

    /* SPURS_PM_FAST_RESCHED bookkeeping drives the same lock-line critical
     * section the coordinator uses. The first A/B attempt died with a double
     * unlock (`pthread_mutex_lock.c:88 __owner == 0`) because the consume path
     * unlocked after ef_line_end() had already done it. Drive the real
     * consume/can_rerun functions and re-acquire the line after every call so
     * a missing or duplicated unlock fails here. */
    {
        const u32 ea = 0x1E000u;
        struct SpursInst si;
        memset(&si, 0, sizeof si);
        si.ea = ea;
        struct SpursPmSlot* slot = &si.slots[0];
        slot->instance = &si;
        slot->wid = 0; slot->spu_num = 0; slot->ea = ea;
        si.active[0] = 2;
        memset(vm_base + ea, 0, 0x2000);
        vm_base[ea + SPURS_WKL_READY1] = 3;
        ef_w16(ea + SPURS_WKL_SIGNAL1, 0x8000);
        si.idle[0] = 3;

        for (int i = 0; i < 64; i++) {
            spurs_pm_fast_consume(&si, slot);
            spu_lockline_lock(); spu_lockline_unlock();
        }
        assert(si.idle[0] == 0);
        assert(vm_base[ea + SPURS_WKL_CURCONT] == 2);
        assert(vm_base[ea + SPURS_WKL_READY1] == 0);
        assert(!(ef_r16(ea + SPURS_WKL_SIGNAL1) & 0x8000u));
        puts("SPURS fast consume takes exactly one lock-line lock/unlock PASS");

        /* Every can_rerun early return must also leave the line unlocked. */
        memset(&s_workloads[0], 0, sizeof s_workloads[0]);
        s_workloads[0].in_use = 1;
        s_workloads[0].spurs_ea = ea;
        /* s_workloads keeps the guest EA (the public API form), not a host
         * pointer. */
        s_workloads[0].pm = (void*)(uintptr_t)0x1D000;
        s_workloads[0].sizePm = 16;
        slot->pm = vm_base + 0x1D000;
        slot->pm_size = 16;
        slot->data = 0;
        si.active[0] = 0;
        vm_write32(ea + SPURS_WKL_ENABLED, 0x80000000u);
        vm_write8(ea + SPURS_WKL_STATE1, 2);
        vm_write8(ea + SPURS_WKL_INFO1 + 0x18 + 0, 1);
        vm_write8(ea + SPURS_WKL_MAXCONT, 1);
        vm_write8(ea + SPURS_WKL_READY1, 0);
        ef_w16(ea + SPURS_WKL_SIGNAL1, 0x8000);
        vm_write64(ea + SPURS_WKL_INFO1 + 8, slot->data);
        int r;

        si.stopping = 1;
        r = spurs_pm_fast_can_rerun(&si, slot); si.stopping = 0;
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        slot->wid = 16;
        r = spurs_pm_fast_can_rerun(&si, slot); slot->wid = 0;
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        slot->ea = 0;
        r = spurs_pm_fast_can_rerun(&si, slot); slot->ea = ea;
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        s_workloads[0].in_use = 0;
        r = spurs_pm_fast_can_rerun(&si, slot); s_workloads[0].in_use = 1;
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        slot->data = 1;
        r = spurs_pm_fast_can_rerun(&si, slot); slot->data = 0;
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        vm_write32(ea + SPURS_WKL_ENABLED, 0);
        r = spurs_pm_fast_can_rerun(&si, slot);
        vm_write32(ea + SPURS_WKL_ENABLED, 0x80000000u);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        vm_write8(ea + SPURS_WKL_STATE1, 3);
        r = spurs_pm_fast_can_rerun(&si, slot);
        vm_write8(ea + SPURS_WKL_STATE1, 2);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        vm_write8(ea + SPURS_WKL_INFO1 + 0x18, 0);
        r = spurs_pm_fast_can_rerun(&si, slot);
        vm_write8(ea + SPURS_WKL_INFO1 + 0x18, 1);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        vm_write8(ea + SPURS_WKL_MAXCONT, 0);
        r = spurs_pm_fast_can_rerun(&si, slot);
        vm_write8(ea + SPURS_WKL_MAXCONT, 1);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        ef_w16(ea + SPURS_WKL_SIGNAL1, 0);
        r = spurs_pm_fast_can_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);

        ef_w16(ea + SPURS_WKL_SIGNAL1, 0x8000);
        r = spurs_pm_fast_can_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 1);
        puts("SPURS fast can_rerun early returns leave the lock line unlocked PASS");

        /* SPURS_PM_FAST_ACTIVE: the worker is already one of si->active[wid].
         * The coordinator's admission test (active >= maxContention) rejects
         * a rerun at the c690 steady state (5 of 5 lanes); the fixed test
         * only rejects a demand the guest has lowered below the active set. */
        s_workloads[0].in_use = 1;
        s_workloads[0].spurs_ea = ea;
        s_workloads[0].pm = (void*)(uintptr_t)0x1D000;
        s_workloads[0].sizePm = 16;
        s_workloads[0].shutting_down = 0;
        slot->pm = vm_base + 0x1D000;
        slot->pm_size = 16;
        slot->data = 0;
        slot->reg_epoch = __atomic_load_n(&s_workload_epoch, __ATOMIC_ACQUIRE);
        vm_write32(ea + SPURS_WKL_ENABLED, 0x80000000u);
        vm_write8(ea + SPURS_WKL_STATE1, 2);
        vm_write8(ea + SPURS_WKL_INFO1 + 0x18 + 0, 1);
        vm_write8(ea + SPURS_WKL_MAXCONT, 5);
        vm_write8(ea + SPURS_WKL_READY1, 8);
        vm_write8(ea + SPURS_WKL_IDLE2, 0);
        ef_w16(ea + SPURS_WKL_SIGNAL1, 0x8000);
        vm_write64(ea + SPURS_WKL_INFO1 + 8, 0);
        si.active[0] = 5;
        setenv("SPURS_PM_FAST_ACTIVE", "0", 1);
        r = spurs_pm_fast_can_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);          /* today: a lane at maxContention never reruns */
        setenv("SPURS_PM_FAST_ACTIVE", "1", 1);
        r = spurs_pm_fast_can_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 1);          /* fixed: rerunning adds no lane */
        assert(spurs_pm_fast_lane_within_level(5, 5, 0, 0) == 0);
        assert(spurs_pm_fast_lane_within_level(5, 5, 0, 1) == 1);
        assert(spurs_pm_fast_lane_within_level(5, 3, 0, 1) == 0);
        assert(spurs_pm_fast_lane_within_level(5, 3, 2, 1) == 1);
        assert(spurs_pm_fast_lane_within_level(5, 0, 0, 1) == 1);
        puts("SPURS fast rerun keeps an already-active lane at maxContention PASS");

        /* SPURS_PM_FAST_ONE_LOCK: check and consume under one lock-line
         * acquisition. Re-acquire the line after each call, exactly like the
         * split-path test above, so a missing or duplicate unlock fails. */
        setenv("SPURS_PM_FAST_ONE_LOCK", "1", 1);
        vm_write8(ea + SPURS_WKL_READY1, 8);
        ef_w16(ea + SPURS_WKL_SIGNAL1, 0x8000);
        si.idle[0] = 3;
        r = spurs_pm_fast_try_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 1);
        assert(vm_read8(ea + SPURS_WKL_READY1) == 7);
        assert(!(ef_r16(ea + SPURS_WKL_SIGNAL1) & 0x8000u));
        assert(si.idle[0] == 0);
        assert(vm_read8(ea + SPURS_WKL_CURCONT) == 5);
        vm_write8(ea + SPURS_WKL_READY1, 0);
        ef_w16(ea + SPURS_WKL_SIGNAL1, 0);
        r = spurs_pm_fast_try_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);
        puts("SPURS fast rerun checks and consumes under one lock line PASS");

        /* SPURS_LANE6: only the title's mesh workloads (wids 1 and 2) get the
         * sixth physical lane, and only lane 5. The guest publishes
         * priority[5] = 0 and maxContention = 5 for them, so the key must
         * relax both checks; every other wid/lane keeps the guest's policy. */
        {
            const u32 e1 = ea;
            memset(&s_workloads[1], 0, sizeof s_workloads[1]);
            s_workloads[1].in_use = 1;
            s_workloads[1].spurs_ea = e1;
            s_workloads[1].pm = (void*)(uintptr_t)0x1D000;
            s_workloads[1].sizePm = 16;
            slot->wid = 1;
            slot->spu_num = 5;
            slot->pm = vm_base + 0x1D000;
            slot->pm_size = 16;
            slot->data = 0;
            si.active[1] = 5;
            vm_write32(e1 + SPURS_WKL_ENABLED, 0x80000000u | 0x40000000u);
            vm_write8(e1 + SPURS_WKL_STATE1 + 1, 2);
            vm_write8(e1 + SPURS_WKL_INFO1 + 1 * SPURS_WKL_INFO_SZ + 0x18 + 5, 0);
            vm_write8(e1 + SPURS_WKL_INFO1 + 1 * SPURS_WKL_INFO_SZ + 0x18 + 4, 1);
            vm_write8(e1 + SPURS_WKL_MAXCONT + 1, 5);
            vm_write8(e1 + SPURS_WKL_READY1 + 1, 8);
            vm_write8(e1 + SPURS_WKL_IDLE2 + 1, 0);
            ef_w16(e1 + SPURS_WKL_SIGNAL1, 0x4000);
            vm_write64(e1 + SPURS_WKL_INFO1 + 1 * SPURS_WKL_INFO_SZ + 8, 0);
            setenv("SPURS_PM_FAST_ACTIVE", "0", 1);
            setenv("SPURS_LANE6", "0", 1);
            r = spurs_pm_fast_can_rerun(&si, slot);
            spu_lockline_lock(); spu_lockline_unlock();
            assert(r == 0);          /* priority[5] = 0 keeps MeshJobs off lane 5 */
            setenv("SPURS_LANE6", "1", 1);
            r = spurs_pm_fast_can_rerun(&si, slot);
            spu_lockline_lock(); spu_lockline_unlock();
            assert(r == 1);          /* the key relaxes priority and the cap */
            si.active[1] = 4;
            slot->spu_num = 4;       /* only lane 5 is overridden */
            r = spurs_pm_fast_can_rerun(&si, slot);
            spu_lockline_lock(); spu_lockline_unlock();
            assert(r == 1);          /* lane 4 priority 1, below the guest cap */
            vm_write8(e1 + SPURS_WKL_INFO1 + 1 * SPURS_WKL_INFO_SZ + 0x18 + 4, 0);
            r = spurs_pm_fast_can_rerun(&si, slot);
            spu_lockline_lock(); spu_lockline_unlock();
            assert(r == 0);          /* lane 4 priority 0 is not a lane-6 case */
            slot->spu_num = 5;
            si.active[1] = 5;
            slot->wid = 0;           /* wid 0 (DXT) keeps the guest policy */
            r = spurs_pm_fast_can_rerun(&si, slot);
            spu_lockline_lock(); spu_lockline_unlock();
            assert(r == 0);
            slot->wid = 1;
            setenv("SPURS_LANE6", "0", 1);
            memset(&s_workloads[1], 0, sizeof s_workloads[1]);
            slot->wid = 0;           /* restore the FAST_ACTIVE/GEN fixture */
            slot->spu_num = 0;
            setenv("SPURS_PM_FAST_ACTIVE", "1", 1);
        }
        puts("SPURS lane 6 override only relaxes the mesh workloads' lane 5 PASS");

        /* SPURS_PM_FAST_GEN: the slot's dispatch-time snapshot is trusted
         * while the registry epoch is unchanged, and refreshed (which then
         * sees the removal) as soon as a mutator bumps it. */
        setenv("SPURS_PM_FAST_GEN", "1", 1);
        slot->reg_epoch = __atomic_load_n(&s_workload_epoch, __ATOMIC_ACQUIRE);
        vm_write8(ea + SPURS_WKL_READY1, 8);
        ef_w16(ea + SPURS_WKL_SIGNAL1, 0x8000);
        s_workloads[0].in_use = 0;   /* mutation without the epoch bump */
        r = spurs_pm_fast_can_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 1);              /* cached snapshot still matches */
        __atomic_fetch_add(&s_workload_epoch, 1, __ATOMIC_RELEASE);
        r = spurs_pm_fast_can_rerun(&si, slot);
        spu_lockline_lock(); spu_lockline_unlock();
        assert(r == 0);              /* refreshed: the workload is gone */
        s_workloads[0].in_use = 1;
        setenv("SPURS_PM_FAST_GEN", "0", 1);
        puts("SPURS fast rerun skips the registry lock only while its epoch holds PASS");


        /* SPURS_PM_NATIVE_DISPATCH: the pure staged-job predicate reads the
         * wwsjob PM's persistent pipeline words. Cover every shape so the
         * worker cannot keep a lane for an already-stored or empty slot. */
        {
            uint8_t ls[0x1300];
            memset(ls, 0, sizeof ls);
            #define LS32(o, v) do { uint32_t _v = (v); ls[(o)] = _v >> 24;                 ls[(o)+1] = _v >> 16; ls[(o)+2] = _v >> 8; ls[(o)+3] = _v; } while (0)
            LS32(0x12D0, 0xFFFFFFFFu); LS32(0x12E0, 0xFFFFFFFFu); LS32(0x12F0, 0xFFFFFFFFu);
            assert(spurs_policy_pm_pipeline_staged(ls) == 0);   /* empty */
            LS32(0x12D0, 3); LS32(0x12E0, 0xFFFFFFFFu); LS32(0x12F0, 2);
            assert(spurs_policy_pm_pipeline_staged(ls) == 1);   /* staged */
            LS32(0x12E0, 3);
            assert(spurs_policy_pm_pipeline_staged(ls) == 1);   /* running */
            LS32(0x12E0, 0xFFFFFFFFu); LS32(0x12F0, 3);
            assert(spurs_policy_pm_pipeline_staged(ls) == 0);   /* stored */
            LS32(0x12E0, 2); LS32(0x12F0, 2);
            assert(spurs_policy_pm_pipeline_staged(ls) == 0);   /* inconsistent */
            assert(spurs_policy_pm_pipeline_staged(NULL) == 0);
            #undef LS32
            puts("SPURS native staged-job predicate covers empty/staged/stored PASS");
        }

        /* End-to-end: a manufactured policy leaves a job staged, and the
         * worker runs it again in the same residency (no coordinator
         * dispatch) when the native wid mask names the workload. The second
         * run clears the staged state and retires the workload. */
        {
            memset(&s_workloads[0], 0, sizeof s_workloads[0]);
            const unsigned long coord_before =
                __atomic_load_n(&s_pm_coord_dispatches, __ATOMIC_RELAXED);
            const unsigned long native_before =
                __atomic_load_n(&s_pm_native_reruns, __ATOMIC_RELAXED);
            setenv("SPURS_PM_NATIVE_DISPATCH", "1", 1);
            setenv("SPURS_PM_FAST_RESCHED", "0", 1);
            setenv("SPURS_PM_FAST_ACTIVE", "0", 1);
            register_pm(0x9800, 0x88, native_policy, 85);
            assert(cellSpursInitialize((CellSpurs*)0x1000, 1, 0, 0, 0) == 0);
            assert(cellSpursAddWorkload((CellSpurs*)0x1000, (u32*)0x8000,
                                        (void*)0x9800, 16, 0x1234, NULL, 1, 5) == 0);
            assert(cellSpursReadyCountStore((CellSpurs*)0x1000, 0, 5) == 0);
            wait_for(&native_runs, 2, "native staged-job rerun");
            Sleep(20);
            assert(atomic_load(&native_runs) == 2);
            assert(vm_read8(0x1000 + SPURS_WKL_READY1) == 3);  /* 5 - dispatch - native */
            assert(__atomic_load_n(&s_pm_coord_dispatches, __ATOMIC_RELAXED) -
                   coord_before == 1);   /* the rerun did not go through the coordinator */
            assert(__atomic_load_n(&s_pm_native_reruns, __ATOMIC_RELAXED) -
                   native_before == 1);
            finalize(0x1000);
            setenv("SPURS_PM_NATIVE_DISPATCH", "0", 1);
            puts("SPURS native dispatch runs a staged job without the coordinator PASS");
        }

        memset(&s_workloads[0], 0, sizeof s_workloads[0]);
    }
    free(vm_base);return 0;
}
