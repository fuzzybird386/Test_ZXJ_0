/* Real ble_link.c, injected controller errors and sequential callback events.
 * This models stack APIs; it does not emulate a radio or concurrent threads. */
#include "../src/ble_link.c"
volatile unsigned checks;
int64_t test_now;
atomic_t ecw_stop;
bool test_ctrl_sub,test_data_sub;
const struct bt_gatt_attr *test_ctrl_attr;
static int enable_result,adv_result,adv_calls,adc_calls,afe_calls;
static int cleanup_result,shutdown_step,radio_stop_step,poweroff_step;
static bool shutdown_led;
static bool adv_led,conn_led,error_led,boot_done=true;
static const struct bt_data *sent_adv,*sent_scan;
static size_t sent_adv_count,sent_scan_count;
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
int bt_enable(void *p) { (void)p;return enable_result; }
int bt_disable(void) { radio_stop_step=++shutdown_step;return 0; }
int bt_le_adv_start(const void *p,const struct bt_data *ad,size_t n,const struct bt_data *sd,size_t sn) {
    (void)p;++adv_calls;sent_adv=ad;sent_adv_count=n;sent_scan=sd;sent_scan_count=sn;return adv_result;
}
void board_led_advertising(bool v) { adv_led=v; }
void board_led_connected(bool v) { conn_led=v;adv_led=false; }
void board_led_error(bool v) { error_led=v; }
void board_led_shutdown(void) { shutdown_led=true;adv_led=conn_led=false; }
int board_battery_mv(void) { ++adc_calls;return 4000; }
int board_power_off(void) { poweroff_step=++shutdown_step;return 0; }
bool afe_boot_finished(void) { return boot_done; }
bool afe_ready(void) { return false; }
int afe_init(void) { ++afe_calls;return -ENODEV; }
bool methods_busy(void) { return false; }
int methods_submit(const struct ecw_request *r,uint32_t gen) { (void)r;(void)gen;return -ENODEV; }
void methods_state(uint8_t out[5]) { memset(out,0,5);out[3]=out[4]=1; }
int methods_stop(uint8_t method) { (void)method;return 0; }
void methods_release(void) { }
void methods_reset_error(void) { }
int methods_prepare_poweroff(void) { ++shutdown_step;return cleanup_result; }
void *memcpy(void *dst,const void *src,size_t n) { uint8_t *d=dst;const uint8_t *s=src;while(n--)*d++=*s++;return dst; }
void *memset(void *dst,int c,size_t n) { uint8_t *d=dst;while(n--)*d++=c;return dst; }
size_t strlen(const char *s) { size_t n=0;while(s[n])++n;return n; }
static void run_adv_work(void) { adv_work.pending=false;advertise(&adv_work.work); }
int run_tests(void) {
    /* Fatal host failure must not report advertising or attempt ADC. */
    enable_result=-EIO;CHECK(ble_link_init()==-EIO);
    CHECK(!adv_calls && !adv_led && !adc_calls);
    /* A temporary controller/resource failure must schedule retries. */
    enable_result=0;adv_result=-ENOMEM;CHECK(ble_link_init()==0);
    CHECK(adv_calls==1 && !adv_led && adv_work.pending && adv_work.delay==500);
    CHECK(bat_work.pending && bat_work.delay==2000 && !adc_calls);
    run_adv_work();CHECK(adv_calls==2 && adv_work.pending && !adv_led);
    adv_result=0;run_adv_work();CHECK(adv_calls==3 && adv_led && !adv_work.pending);
    /* Inspect actual bytes passed to the stack, against APP UUID/name. */
    unsigned bytes=0;bool aabb=false,name=false;
    for(unsigned i=0;i<sent_adv_count;++i) {
        const struct bt_data *d=&sent_adv[i];bytes+=2+d->data_len;
        if(d->type==BT_DATA_UUID16_ALL) {
            CHECK(d->data_len%2==0);
            for(unsigned j=0;j<d->data_len;j+=2) if(d->data[j]==0xbb && d->data[j+1]==0xaa)aabb=true;
        }
        if(d->type==BT_DATA_NAME_COMPLETE) name=d->data_len==4 && !memcmp(d->data,"CQET",4);
    }
    CHECK(aabb && name && bytes<=31);
    CHECK(sent_scan_count==1 && sent_scan[0].data_len==4 && !memcmp(sent_scan[0].data,"CQET",4));
    CHECK(service_uuid.val==0xaabb && control_uuid.val==0xaab0 && data_uuid.val==0xaabf && log_uuid.val==0xaabe);
    CHECK(CONTROL_ATTR->read==read_state && CONTROL_ATTR->write==write_control);
    CHECK((CONTROL_ATTR->properties & (BT_GATT_CHRC_READ|BT_GATT_CHRC_WRITE|BT_GATT_CHRC_NOTIFY))==26);
    CHECK(DATA_ATTR->read==read_data && !DATA_ATTR->write && (DATA_ATTR->properties&BT_GATT_CHRC_NOTIFY));
    /* Connection alone is insufficient: both notification subscriptions matter. */
    struct bt_conn peer={0};test_ctrl_attr=CONTROL_ATTR;
    callbacks.connected(&peer,0);uint32_t old_generation=ble_session();
    CHECK(ble_connected() && conn_led && !adv_led && peer.refs==1);
    CHECK(!ble_stream_ready());test_ctrl_sub=true;CHECK(!ble_stream_ready());
    test_data_sub=true;CHECK(ble_stream_ready());
    int calls=adv_calls;run_adv_work();CHECK(adv_calls==calls);
    /* Busy system work must not call the ADC from the shared workqueue. */
    battery_work(&bat_work.work);CHECK(!adc_calls && battery_wakeup==1 && bat_work.delay==30000);
    /* RESET must not race initial AD5941 ownership; reply retains APP session. */
    boot_done=false;
    struct rx_record reset={.generation=ble_session(),.len=3,.data={0x34,0x12,ECW_OP_RESET}};
    command(&reset);struct tx_record reply;
    CHECK(k_msgq_get(&tx_queue,&reply,K_NO_WAIT)==0 && reply.kind==0 && reply.len==4);
    CHECK(reply.data[0]==0x34 && reply.data[1]==0x12 && reply.data[2]==ECW_OP_RESET && reply.data[3]==(uint8_t)-EAGAIN);
    CHECK(afe_calls==0);
    tx_queue.head=tx_queue.count=0;boot_done=true;
    /* Exercise the actual ATT write ingress and command dispatch for PING. */
    uint8_t ping[]={0x78,0x56,ECW_OP_PING};
    CHECK(write_control(&peer,CONTROL_ATTR,ping,sizeof(ping),1,0)==BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
    CHECK(write_control(&peer,CONTROL_ATTR,ping,2,0,0)==BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    CHECK(write_control(&peer,CONTROL_ATTR,ping,sizeof(ping),0,0)==sizeof(ping));
    struct rx_record received;
    CHECK(k_msgq_get(&rx_queue,&received,K_NO_WAIT)==0);command(&received);
    CHECK(k_msgq_get(&tx_queue,&reply,K_NO_WAIT)==0 && reply.kind==0 && reply.data[0]==0x78 && reply.data[1]==0x56 && reply.data[3]==0);
    callbacks.disconnected(&peer,0x13);
    CHECK(!ble_connected() && !conn_led && !adv_led && peer.refs==0 && atomic_get(&ecw_stop));
    CHECK(ble_session()!=old_generation && atomic_get(&stopping) && atomic_get(&shutdown_requested) && shutdown_led && rx_wakeup);
    command(&received);CHECK(!tx_queue.count); /* old command cannot enter new link */
    /* No re-advertising during the red warning, even after object recycling. */
    calls=adv_calls;callbacks.recycled();run_adv_work();CHECK(adv_calls==calls && !adv_led);
    /* Cleanup failure retains power; success orders AFE, radio, then power. */
    cleanup_result=-ETIMEDOUT;shutdown_after_disconnect();
    CHECK(!poweroff_step && !radio_stop_step && error_led);
    cleanup_result=0;shutdown_step=0;shutdown_after_disconnect();
    CHECK(radio_stop_step==2 && poweroff_step==3);
    CHECK(!adv_led && !adv_work.pending && !bat_work.pending);
    calls=adv_calls;callbacks.recycled();run_adv_work();CHECK(adv_calls==calls && !adv_work.pending);
    return 0;
}
