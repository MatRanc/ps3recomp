/* cellSpursRequestIdleSpu against the real libs/spurs/cellSpurs.c scheduler, with a manufactured policy module.
 *
 * The idle count was stored but only read under SPURS_READY_LEVEL=1: by default a workload whose ready count put it on
 * one SPU stayed on one, however many idle SPUs it asked for. And cellSpursAddWorkload did not clear the count when it
 * reused a slot, so a new workload inherited the old one's request.
 *
 * SPURS_NO_IDLE_PROBE=1 keeps the scheduler's own idle probes off, so only the ready count and the idle request put the
 * workload on an SPU and the number of SPUs it gets is exact. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <stdatomic.h>

void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n,f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart*1000/f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
void sys_event_queue_cancel_by_id(u32 id) {(void)id;}
void sys_event_queue_uncancel_by_id(u32 id) {(void)id;}

#define SPURS 0x1000u
static atomic_uint held,done,gate,active_count,peak_count;
/* Holds its SPU until the gate opens, then takes itself out of the runnable set. */
static void held_policy(spu_context* c)
{
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    unsigned n=atomic_fetch_add(&active_count,1)+1,peak=atomic_load(&peak_count);
    while(peak<n && !atomic_compare_exchange_weak(&peak_count,&peak,n)) {}
    atomic_fetch_add(&held,1);
    while(!atomic_load(&gate)) Sleep(1);
    atomic_fetch_sub(&active_count,1);atomic_fetch_add(&done,1);
    vm_write8(ea+0x80+wid,3);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void wait_for(atomic_uint* p,unsigned value,const char* what)
{
    for(unsigned i=0;i<3000;i++) { if(atomic_load(p)>=value)return;Sleep(1); }
    fprintf(stderr,"FAIL: %s: %u of %u\n",what,atomic_load(p),value);exit(1);
}
static void finalize(void)
{
    for(unsigned i=0;i<3000;i++) {
        s32 rc=cellSpursFinalize((CellSpurs*)(uintptr_t)SPURS);
        if (!rc) return;
        assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY);Sleep(1);
    }
    assert(!"finalize failed to join");
}
/* With a ready count of 1 and `idle` idle SPUs requested on 3 SPUs, how many SPUs the workload runs on at once. */
static unsigned spus_taken(unsigned idle)
{
    atomic_store(&gate,0);atomic_store(&held,0);atomic_store(&done,0);atomic_store(&peak_count,0);
    assert(cellSpursInitialize((CellSpurs*)SPURS,3,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)SPURS,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,3)==0);
    assert(vm_read32(0x8000)==0);
    assert(cellSpursRequestIdleSpu(SPURS,0,idle)==0);
    assert(cellSpursReadyCountStore((CellSpurs*)SPURS,0,1)==0);
    wait_for(&held,1,"ready count admission");
    Sleep(50);
    unsigned taken=atomic_load(&peak_count);
    atomic_store(&gate,1);wait_for(&done,taken,"release");
    finalize();
    return taken;
}
int main(void)
{
    setenv("SPURS_NO_IDLE_PROBE","1",1);
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    memset(vm_base+0x9000,0x11,16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+0x9000,16),held_policy,77,"manufactured idle SPU fixture");

    /* A new workload in a reused slot does not inherit the old one's idle request. */
    assert(cellSpursInitialize((CellSpurs*)SPURS,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)SPURS,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,1)==0);
    assert(cellSpursRequestIdleSpu(SPURS,0,2)==0);
    assert(vm_read8(SPURS+SPURS_WKL_IDLE2)==2);
    assert(cellSpursRemoveWorkload((CellSpurs*)SPURS,0)==0);   /* never ran: ready count 0, no probes */
    assert(cellSpursAddWorkload((CellSpurs*)SPURS,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,1)==0);
    assert(vm_read32(0x8000)==0);
    if(vm_read8(SPURS+SPURS_WKL_IDLE2)!=0) {
        fprintf(stderr,"FAIL: the reused slot kept idle request %u\n",vm_read8(SPURS+SPURS_WKL_IDLE2));return 1;
    }
    finalize();
    puts("SPURS reused workload slot starts with no idle SPU request PASS");

    /* The idle request is honoured without SPURS_READY_LEVEL, and bounded: ready 1 + idle SPUs. */
    unsigned n0=spus_taken(0),n1=spus_taken(1),n2=spus_taken(2);
    if(n0!=1||n1!=2||n2!=3) {
        fprintf(stderr,"FAIL: SPUs taken with idle 0/1/2 = %u/%u/%u, want 1/2/3\n",n0,n1,n2);return 1;
    }
    puts("SPURS idle SPU request admits ready + idle SPUs in the default ready-count mode PASS");
    free(vm_base);return 0;
}
