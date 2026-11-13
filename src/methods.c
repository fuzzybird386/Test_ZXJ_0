#include "ecw.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
atomic_t ecw_stop;
static atomic_t busy, current_method, last_error;
static bool powering_off;
struct job { struct ecw_request req; uint32_t generation; };
K_MSGQ_DEFINE(jobs,sizeof(struct job),1,4);
K_SEM_DEFINE(start_gate,0,1);
K_MUTEX_DEFINE(config_lock);
static K_THREAD_STACK_DEFINE(worker_stack,6144);
static struct k_thread worker;
bool methods_busy(void) { return atomic_get(&busy)!=0; }
void methods_state(uint8_t out[5])
{
    k_mutex_lock(&config_lock,K_FOREVER);
    out[0]=methods_busy()?2:(atomic_get(&last_error)?255:0);
    out[1]=atomic_get(&current_method); out[2]=wire_error(atomic_get(&last_error));
    out[3]=1; out[4]=1;
    k_mutex_unlock(&config_lock);
}
void methods_reset_error(void) { atomic_clear(&last_error); }
int methods_stop(uint8_t method)
{
    k_mutex_lock(&config_lock,K_FOREVER);
    int rc=0;
    if (methods_busy() && method && method!=atomic_get(&current_method)) { rc=-EINVAL; }
    else if (methods_busy()) { atomic_set(&ecw_stop,1); }
    k_mutex_unlock(&config_lock);
    return rc;
}
int methods_submit(const struct ecw_request *r,uint32_t generation)
{
    int rc=wire_validate(r);
    if (rc) { return rc; }
    k_mutex_lock(&config_lock,K_FOREVER);
    if (powering_off || methods_busy()) { rc=-EBUSY; }
    else if (!afe_boot_finished()) { rc=-EAGAIN; }
    else if (!afe_ready()) { rc=-ENODEV; }
    else if (generation!=ble_session() || !ble_stream_ready()) { rc=-ENOTCONN; }
    else {
        struct job job={.req=*r,.generation=generation};
        atomic_clear(&ecw_stop); atomic_clear(&last_error);
        atomic_set(&current_method,r->method); atomic_set(&busy,1);
        rc=k_msgq_put(&jobs,&job,K_NO_WAIT);
        if (rc) { atomic_clear(&busy); atomic_clear(&current_method); }
    }
    k_mutex_unlock(&config_lock);
    return rc;
}
/* RX enqueues START ACK first, then releases this gate. This prevents a very
 * short measurement's END from overtaking its ACK on the shared TX queue. */
void methods_release(void) { k_sem_give(&start_gate); }
int methods_prepare_poweroff(void)
{
    k_mutex_lock(&config_lock,K_FOREVER);
    powering_off=true; atomic_set(&ecw_stop,1);
    k_mutex_unlock(&config_lock);
    int64_t until=k_uptime_get()+2000;
    /* BLE can now disconnect during initial AFE setup. Do not idle SPI until
     * boot and the measurement worker have both released it. */
    while (!afe_boot_finished() || methods_busy()) {
        if (k_uptime_get()>=until) { return -ETIMEDOUT; }
        k_msleep(5);
    }
    afe_idle(); return 0;
}
static int interrupted(const struct job *j)
{
    if (ble_tx_error()) { return -ENOBUFS; }
    return atomic_get(&ecw_stop)||j->generation!=ble_session()||!ble_stream_ready() ? -ECANCELED:0;
}
static int wait_until(int64_t until,const struct job *j)
{
    for (;;) {
        int rc=interrupted(j); if (rc) { return rc; }
        int64_t left=until-k_uptime_get(); if (left<=0) { return 0; }
        k_msleep(MIN(left,5));
    }
}
static int sample_at(struct ecw_sample *s,int64_t deadline,int64_t origin,const struct job *j,bool ocp)
{
    int rc=wait_until(deadline-(ocp?40:20),j); if (rc) { return rc; }
    bool range_changed=false;
    s->flags=0;
    rc=ocp?afe_ocp(&s->potential_uv,&s->flags):afe_current(&s->current_na,&s->flags);
    /* Never send clipped samples. CV/CA can widen the range at this same
     * potential; DPV keeps its fixed pulse timing and OCP has no TIA to change. */
    for (unsigned retry=0;!rc && (s->flags&ECW_FLAG_ADC_RAIL) &&
         (j->req.method==ECW_CV || j->req.method==ECW_CA) && retry<7;++retry) {
        rc=interrupted(j); if (rc) { return rc; }
        rc=afe_reduce_current_range(); if (rc) { return rc; }
        range_changed=true;s->flags=0;
        rc=afe_current(&s->current_na,&s->flags);
    }
    s->elapsed_ms=(uint32_t)(k_uptime_get()-origin);
    if (rc) { return rc; }
    if (s->flags&ECW_FLAG_ADC_RAIL) { return -ERANGE; }
    if (range_changed) {
        ble_log(2,0x100,"Current range widened; saturated sample remeasured");
    } else if (k_uptime_get()>deadline+5) { return -ETIME; }
    return wait_until(deadline,j);
}
static int emit(const struct job *j,struct ecw_sample *s)
{
    int rc=interrupted(j); if (rc) { return rc; }
    rc=ble_frame(j->generation,j->req.method,0,j->req.session_id,(uint16_t)s->index,s);
    if (!rc) { ++s->index; } return rc;
}
static int cv_leg(const struct job *j,struct ecw_sample *s,int32_t *mv,int32_t target,int64_t *deadline,int64_t origin)
{
    while (*mv!=target) {
        int32_t delta=MIN(abs(target-*mv),j->req.step_mv);
        *mv+=target>*mv?delta:-delta;
        int rc=afe_set_potential(*mv,&s->potential_uv); if (rc) { return rc; }
        *deadline+=MAX(50,delta*1000/j->req.rate_mv_s);
        rc=sample_at(s,*deadline,origin,j,false);
        if (rc || (rc=emit(j,s))) { return rc; }
        /* A range change can consume extra time. Resume the requested scan
         * rate from the real sample time, never race through catch-up steps. */
        *deadline=MAX(*deadline,k_uptime_get());
    }
    return 0;
}
static int run_cv(const struct job *j,struct ecw_sample *s,int64_t origin)
{
    const struct ecw_request *r=&j->req;
    int32_t mv=r->start_mv;
    int64_t deadline=origin+MAX(50,r->step_mv*1000/r->rate_mv_s);
    int rc=sample_at(s,deadline,origin,j,false);
    if (rc || (rc=emit(j,s))) { return rc; }
    deadline=MAX(deadline,k_uptime_get());
    for (uint32_t c=0;c<r->cycles;++c) {
        char message[92];
        snprintf(message,sizeof(message),"CV %lu/%lu leg1 %ld->%ldmV seq=%lu",
            (unsigned long)c+1,(unsigned long)r->cycles,(long)mv,(long)r->vertex1_mv,(unsigned long)s->index);
        ble_log(1,0x110,message);
        rc=cv_leg(j,s,&mv,r->vertex1_mv,&deadline,origin);
        if (rc) { return rc; }
        snprintf(message,sizeof(message),"CV %lu/%lu leg2 %ld->%ldmV seq=%lu",
            (unsigned long)c+1,(unsigned long)r->cycles,(long)mv,(long)r->vertex2_mv,(unsigned long)s->index);
        ble_log(1,0x110,message);
        rc=cv_leg(j,s,&mv,r->vertex2_mv,&deadline,origin);
        if (rc) { return rc; }
        snprintf(message,sizeof(message),"CV %lu/%lu done total=%lu points",
            (unsigned long)c+1,(unsigned long)r->cycles,(unsigned long)s->index);
        ble_log(1,0x111,message);
    }
    return cv_leg(j,s,&mv,r->end_mv,&deadline,origin);
}
static int run_dpv(const struct job *j,struct ecw_sample *s,int64_t origin)
{
    const struct ecw_request *r=&j->req;
    int32_t mv=r->start_mv;
    int64_t phase=origin;
    for (;;) {
        int32_t base_uv;
        int rc=afe_set_potential(mv,&base_uv); if (rc) { return rc; }
        rc=sample_at(s,phase+r->period_ms-r->pulse_ms,origin,j,false); if (rc) { return rc; }
        s->base_na=s->current_na;
        rc=afe_set_potential(mv+r->pulse_mv,&s->potential_uv); if (rc) { return rc; }
        rc=sample_at(s,phase+r->period_ms,origin,j,false);
        int restore=afe_set_potential(mv,&s->potential_uv);
        if (rc || restore) { return rc?rc:restore; }
        s->pulse_na=s->current_na; s->potential_uv=base_uv;
        rc=emit(j,s); if (rc) { return rc; }
        if (mv==r->end_mv) { break; }
        int32_t step=MIN(abs(r->end_mv-mv),r->step_mv);
        mv+=r->end_mv>mv?step:-step; phase+=r->period_ms;
    }
    return 0;
}
static int run_timed(const struct job *j,struct ecw_sample *s,int64_t origin)
{
    const struct ecw_request *r=&j->req;
    uint32_t pulses=r->method==ECW_CA?r->cycles:1;
    for (uint32_t pulse=0;pulse<pulses;++pulse) {
        if (pulse) {
            int rc=afe_set_potential(0,&s->potential_uv); if (rc) { return rc; }
            rc=wait_until(k_uptime_get()+r->quiet_ms,j); if (rc) { return rc; }
            rc=afe_set_potential(r->start_mv,&s->potential_uv); if (rc) { return rc; }
        }
        int64_t start=k_uptime_get();
        for (uint32_t t=r->interval_ms;t<=r->duration_ms;t+=r->interval_ms) {
            int rc=sample_at(s,start+t,origin,j,r->method==ECW_OCP);
            if (rc || (rc=emit(j,s))) { return rc; }
        }
        int rc=wait_until(start+r->duration_ms,j); if (rc) { return rc; }
    }
    return 0;
}
static int run_eis(const struct job *j,struct ecw_sample *s)
{
    const struct ecw_request *r=&j->req;
    double freq=r->freq_start_mhz;
    bool up=r->freq_end_mhz>=r->freq_start_mhz;
    double factor=pow(10.0,1.0/r->points_per_dec);
    for (;;) {
        int rc=interrupted(j); if (rc) { return rc; }
        uint32_t f=(uint32_t)llround(freq);
        rc=afe_eis_point(r,f,s);
        if (rc || (rc=emit(j,s))) { return rc; }
        if (f==r->freq_end_mhz) { return 0; }
        double next=up?freq*factor:freq/factor;
        if ((up&&next>=r->freq_end_mhz)||(!up&&next<=r->freq_end_mhz)) { next=r->freq_end_mhz; }
        freq=next;
    }
}
static void measurement_thread(void *a,void *b,void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct job j;
    for (;;) {
        k_msgq_get(&jobs,&j,K_FOREVER); k_sem_take(&start_gate,K_FOREVER);
        struct ecw_sample s={.method=j.req.method};
        int rc=interrupted(&j);
        if (!rc) { rc=afe_prepare(j.req.method==ECW_OCP||j.req.method==ECW_EIS,j.req.rtia_ohm); }
        if (!rc && j.req.method!=ECW_OCP && j.req.method!=ECW_EIS) {
            rc=afe_set_potential(j.req.method==ECW_CA?0:j.req.start_mv,&s.potential_uv);
            if (!rc) { rc=wait_until(k_uptime_get()+j.req.quiet_ms,&j); }
            if (!rc && j.req.method==ECW_CA) { rc=afe_set_potential(j.req.start_mv,&s.potential_uv); }
        }
        int64_t origin=k_uptime_get();
        if (!rc) {
            switch(j.req.method) {
            case ECW_CV: rc=run_cv(&j,&s,origin); break;
            case ECW_DPV: rc=run_dpv(&j,&s,origin); break;
            case ECW_EIS: rc=run_eis(&j,&s); break;
            default: rc=run_timed(&j,&s,origin); break;
            }
        }
        afe_idle();
        if (!rc && afe_port_error()) { rc=afe_port_error(); }
        bool failed=rc && rc!=-ECANCELED;
        if (failed) {
            const char *why=rc==-ERANGE ? "AFE overrange: current/ADC limit or input common mode" :
                rc==-ETIME ? "Sampling deadline missed; reduce scan/sample rate" :
                rc==-ETIMEDOUT ? "AFE acquisition timed out; check SPI and power" :
                "Measurement failed; check AFE or BLE transmission";
            ble_log(3,(uint16_t)(-rc),why);
        }
        /* Keep the captured APP session and connection generation even after STOP/RESET. */
        int end_rc=ble_frame(j.generation,j.req.method,failed?255:2,j.req.session_id,(uint16_t)s.index,NULL);
        if (!failed && end_rc && j.generation==ble_session()) { rc=end_rc; failed=true; }
        k_mutex_lock(&config_lock,K_FOREVER);
        atomic_set(&last_error,failed?rc:0); atomic_clear(&current_method); atomic_clear(&busy);
        k_mutex_unlock(&config_lock);
    }
}
void methods_init(void)
{
    k_thread_create(&worker,worker_stack,K_THREAD_STACK_SIZEOF(worker_stack),measurement_thread,NULL,NULL,NULL,5,0,K_NO_WAIT);
    k_thread_name_set(&worker,"ecw_measure");
}
