#include "ecw.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/logging/log.h>
#include <string.h>
LOG_MODULE_REGISTER(ecw_ble,LOG_LEVEL_INF);
static struct bt_uuid_16 service_uuid=BT_UUID_INIT_16(0xaabb);
static struct bt_uuid_16 control_uuid=BT_UUID_INIT_16(0xaab0);
static struct bt_uuid_16 data_uuid=BT_UUID_INIT_16(0xaabf);
static struct bt_uuid_16 log_uuid=BT_UUID_INIT_16(0xaabe);
static struct bt_conn *connection;
static atomic_t generation, tx_fault, stopping, shutdown_requested;
K_MUTEX_DEFINE(link_lock);
K_MUTEX_DEFINE(log_lock);
struct rx_record { uint32_t generation; uint16_t len; uint8_t data[24]; };
struct tx_record { uint32_t generation; uint16_t len; uint8_t kind; uint8_t data[104]; };
K_MSGQ_DEFINE(rx_queue,sizeof(struct rx_record),8,4);
K_MSGQ_DEFINE(tx_queue,sizeof(struct tx_record),128,4);
K_SEM_DEFINE(rx_wakeup,0,1);
static K_THREAD_STACK_DEFINE(rx_stack,3072);
static K_THREAD_STACK_DEFINE(tx_stack,3072);
static struct k_thread rx_obj,tx_obj;
K_SEM_DEFINE(battery_wakeup,0,1);
static K_THREAD_STACK_DEFINE(battery_stack,1536);
static struct k_thread battery_obj;
static uint16_t log_id, pending_error;
static uint8_t last_log[104];
static size_t last_log_len;
static int enqueue(const struct tx_record *r)
{
    if (r->generation!=ble_session()) { return -ENOTCONN; }
    int rc=k_msgq_put(&tx_queue,r,K_NO_WAIT);
    if (rc && r->kind!=2) { atomic_set(&tx_fault,1); atomic_set(&ecw_stop,1); }
    return rc ? -ENOBUFS : 0;
}
static void ccc_changed(const struct bt_gatt_attr *attr,uint16_t value)
{
    LOG_INF("CCC handle=0x%04x value=0x%04x",attr->handle,value);
    if (value!=BT_GATT_CCC_NOTIFY && methods_busy()) { atomic_set(&ecw_stop,1); }
}
static void log_ccc_changed(const struct bt_gatt_attr *attr,uint16_t value)
{ ARG_UNUSED(attr); ARG_UNUSED(value); }
static ssize_t read_state(struct bt_conn *conn,const struct bt_gatt_attr *attr,void *buf,uint16_t len,uint16_t offset)
{
    uint8_t state[5]; methods_state(state);
    if (!afe_ready() && !methods_busy()) { state[0]=255; state[2]=wire_error(-ENODEV); }
    return bt_gatt_attr_read(conn,attr,buf,len,offset,state,sizeof(state));
}
static ssize_t read_data(struct bt_conn *conn,const struct bt_gatt_attr *attr,void *buf,uint16_t len,uint16_t offset)
{
    return bt_gatt_attr_read(conn,attr,buf,len,offset,NULL,0);
}
static ssize_t read_log(struct bt_conn *conn,const struct bt_gatt_attr *attr,void *buf,uint16_t len,uint16_t offset)
{
    k_mutex_lock(&log_lock,K_FOREVER);
    ssize_t rc=bt_gatt_attr_read(conn,attr,buf,len,offset,last_log,last_log_len);
    k_mutex_unlock(&log_lock); return rc;
}
static ssize_t write_log(struct bt_conn *conn,const struct bt_gatt_attr *attr,const void *buf,uint16_t len,uint16_t offset,uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); const uint8_t *p=buf;
    if (offset || (flags&BT_GATT_WRITE_FLAG_PREPARE)) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    if (len!=3 || (p[0]!=1 && p[0]!=2)) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    k_mutex_lock(&log_lock,K_FOREVER);
    if (pending_error && wire_u16(p+1)==pending_error) { pending_error=0; if(last_log_len>=12) { last_log[10]=0; } board_led_error(false); }
    k_mutex_unlock(&log_lock); return len;
}
static ssize_t write_control(struct bt_conn *conn,const struct bt_gatt_attr *attr,const void *buf,uint16_t len,uint16_t offset,uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr);
    if (offset || (flags&BT_GATT_WRITE_FLAG_PREPARE)) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    if (len<3 || len>24) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    struct rx_record r={.generation=ble_session(),.len=len}; memcpy(r.data,buf,len);
    if (k_msgq_put(&rx_queue,&r,K_NO_WAIT)) {
        atomic_set(&ecw_stop,1); return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    k_sem_give(&rx_wakeup); return len;
}
BT_GATT_SERVICE_DEFINE(ecw_service,
    BT_GATT_PRIMARY_SERVICE(&service_uuid),
    BT_GATT_CHARACTERISTIC(&control_uuid.uuid,BT_GATT_CHRC_READ|BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP|BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ|BT_GATT_PERM_WRITE,read_state,write_control,NULL),
    BT_GATT_CCC(ccc_changed,BT_GATT_PERM_READ|BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&data_uuid.uuid,BT_GATT_CHRC_READ|BT_GATT_CHRC_NOTIFY,BT_GATT_PERM_READ,read_data,NULL,NULL),
    BT_GATT_CCC(ccc_changed,BT_GATT_PERM_READ|BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&log_uuid.uuid,BT_GATT_CHRC_READ|BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP|BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ|BT_GATT_PERM_WRITE,read_log,write_log,NULL),
    BT_GATT_CCC(log_ccc_changed,BT_GATT_PERM_READ|BT_GATT_PERM_WRITE)
);
#define CONTROL_ATTR (&ecw_service.attrs[2])
#define DATA_ATTR (&ecw_service.attrs[5])
#define LOG_ATTR (&ecw_service.attrs[8])
uint32_t ble_session(void) { return (uint32_t)atomic_get(&generation); }
int ble_tx_error(void) { return atomic_get(&tx_fault); }
bool ble_connected(void)
{
    k_mutex_lock(&link_lock,K_FOREVER); bool yes=connection!=NULL; k_mutex_unlock(&link_lock); return yes;
}
bool ble_stream_ready(void)
{
    k_mutex_lock(&link_lock,K_FOREVER);
    bool yes=connection && bt_gatt_is_subscribed(connection,CONTROL_ATTR,BT_GATT_CCC_NOTIFY) && bt_gatt_is_subscribed(connection,DATA_ATTR,BT_GATT_CCC_NOTIFY);
    k_mutex_unlock(&link_lock); return yes;
}
bool ble_error_active(void)
{
    k_mutex_lock(&log_lock,K_FOREVER); bool yes=pending_error!=0; k_mutex_unlock(&log_lock); return yes;
}
void ble_log(uint8_t level,uint16_t code,const char *message)
{
    struct tx_record r={.generation=ble_session(),.kind=2};
    k_mutex_lock(&log_lock,K_FOREVER);
    if (++log_id==0) { ++log_id; }
    r.data[0]=1; r.data[1]=level; wire_put16(r.data+2,log_id);
    wire_put32(r.data+4,k_uptime_get_32()); wire_put16(r.data+8,code);
    r.data[10]=level==3?3:0;
    r.data[11]=MIN(strlen(message),sizeof(r.data)-12);
    memcpy(r.data+12,message,r.data[11]); r.len=12+r.data[11];
    if (level==3) { pending_error=log_id; board_led_error(true); }
    if (!pending_error || level==3) { memcpy(last_log,r.data,r.len); last_log_len=r.len; }
    k_mutex_unlock(&log_lock);
    LOG_INF("event %u level %u: %s",code,level,message);
    if (ble_connected()) { (void)enqueue(&r); }
}
int ble_frame(uint32_t gen,uint8_t method,uint8_t type,uint16_t session,uint16_t seq,const struct ecw_sample *s)
{
    if (gen!=ble_session() || !ble_stream_ready()) { return -ENOTCONN; }
    struct tx_record r={.generation=gen,.kind=1};
    r.len=wire_frame(r.data,method,type,session,seq,s); return enqueue(&r);
}
static int ack(uint32_t gen,uint16_t session,uint8_t opcode,int rc)
{
    struct tx_record r={.generation=gen,.len=4,.kind=0};
    wire_put16(r.data,session); r.data[2]=opcode; r.data[3]=(uint8_t)wire_error(rc); return enqueue(&r);
}
static void command(const struct rx_record *rx)
{
    if (rx->generation!=ble_session()) { return; }
    const uint8_t *p=rx->data; uint8_t op=p[2]; uint16_t sid=wire_u16(p);
    LOG_INF("Command session=%u opcode=0x%02x bytes=%u",sid,op,rx->len);
    int rc=0; bool started=false;
    switch(op) {
    case ECW_OP_START: {
        struct ecw_request req;
        rc=wire_decode_start(p,rx->len,&req);
        if (!rc) {
            if (!methods_busy()) { atomic_clear(&tx_fault); }
            rc=methods_submit(&req,rx->generation); started=rc==0;
        }
        break;
    }
    case ECW_OP_STOP:
        rc=rx->len==4 ? methods_stop(p[3]) : -EMSGSIZE; break;
    case ECW_OP_RESET: {
        if (rx->len!=3) { rc=-EMSGSIZE; break; }
        if (!afe_boot_finished()) { rc=-EAGAIN; break; }
        methods_stop(0);
        int64_t until=k_uptime_get()+2000;
        while (methods_busy() && k_uptime_get()<until) { k_msleep(5); }
        if (methods_busy()) { rc=-ETIMEDOUT; }
        else {
            /* A bus fault holds the AFE in reset. Recover only after the
             * measurement worker has released SPI and opened the electrodes. */
            if (!afe_ready()) { atomic_clear(&ecw_stop); rc=afe_init(); }
            if (!rc) { methods_reset_error(); atomic_clear(&tx_fault); }
        }
        break;
    }
    case ECW_OP_PING: rc=rx->len==3?0:-EMSGSIZE; break;
    default: rc=ECW_UNSUPPORTED; break;
    }
    int ack_rc=ack(rx->generation,sid,op,rc);
    if (started) { if (ack_rc) { atomic_set(&ecw_stop,1); } methods_release(); }
    if (rc) {
        const char *msg=rc==-EBUSY?"Measurement active; stop and wait before START":
            rc==ECW_UNSUPPORTED?"This PCB supports WE0 only; no STIM circuit":
            (rc==-EINVAL?"Invalid range or sampling interval (CV/CA >=50ms, OCP >=100ms)":"Command rejected; check device state and parameters");
        ble_log(3,(uint16_t)(-rc),msg);
    }
}
static void shutdown_after_disconnect(void)
{
    int rc=methods_prepare_poweroff();
    if (!rc) {
        rc=ble_link_stop();
        if (!rc) { rc=board_power_off(); }
    }
    if (rc) { ble_log(3,-rc,"Power off cleanup failed; power hold retained"); }
}
static void rx_thread(void *a,void *b,void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        k_sem_take(&rx_wakeup,K_FOREVER);
        if (atomic_get(&shutdown_requested)) {
            shutdown_after_disconnect();
            continue;
        }
        struct rx_record r;
        while (!k_msgq_get(&rx_queue,&r,K_NO_WAIT)) { command(&r); }
    }
}
static void tx_thread(void *a,void *b,void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct tx_record r;
    for (;;) {
        k_msgq_get(&tx_queue,&r,K_FOREVER);
        if (r.generation!=ble_session()) { continue; }
        k_mutex_lock(&link_lock,K_FOREVER);
        struct bt_conn *conn=connection?bt_conn_ref(connection):NULL;
        k_mutex_unlock(&link_lock); if (!conn) { continue; }
        const struct bt_gatt_attr *attr=r.kind==0?CONTROL_ATTR:(r.kind==1?DATA_ATTR:LOG_ATTR);
        uint16_t mtu=bt_gatt_get_mtu(conn);
        /* AABF frames are indivisible. All measurement frames fit MTU 23. */
        if (r.kind==2 && r.len>mtu-3) { r.len=mtu-3; r.data[11]=r.len-12; }
        int rc=0; int64_t until=k_uptime_get()+500;
        for (;;) {
            if (r.generation!=ble_session()) { rc=-ENOTCONN; break; }
            if (!bt_gatt_is_subscribed(conn,attr,BT_GATT_CCC_NOTIFY)) { rc=-ENOTCONN; break; }
            if (r.len>mtu-3) { rc=-EMSGSIZE; break; }
            rc=bt_gatt_notify(conn,attr,r.data,r.len);
            if (rc!=-ENOMEM && rc!=-EAGAIN && rc!=-ENOBUFS) { break; }
            if (k_uptime_get()>=until) { rc=-ETIMEDOUT; break; }
            k_msleep(5);
        }
        if (rc && r.kind!=2 && r.generation==ble_session()) {
            atomic_set(&tx_fault,1); atomic_set(&ecw_stop,1);
            /* An out-of-band error frame prevents a silently incomplete curve.
             * Delivery itself may fail if the link is gone; RTT retains the error. */
            LOG_ERR("Notify failed: %d",rc);
            if (r.kind==1 && r.data[1]==0) { r.data[1]=255; wire_put16(r.data+6,0); (void)bt_gatt_notify(conn,DATA_ATTR,r.data,8); }
        }
        bt_conn_unref(conn);
    }
}
static const struct bt_data advertising[]={
    BT_DATA_BYTES(BT_DATA_FLAGS,BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL,0xbb,0xaa,0x0f,0x18,0x0a,0x18),
    /* Include the name in ADV_IND as well as SCAN_RSP so discovery does not
     * depend on an active scanner receiving the scan response. 17 bytes total. */
    BT_DATA(BT_DATA_NAME_COMPLETE,CONFIG_BT_DEVICE_NAME,sizeof(CONFIG_BT_DEVICE_NAME)-1),
};
static const struct bt_data scan_response[]={BT_DATA(BT_DATA_NAME_COMPLETE,CONFIG_BT_DEVICE_NAME,sizeof(CONFIG_BT_DEVICE_NAME)-1)};
static void advertise(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_work,advertise);
static int start_advertising(void)
{
    int rc=bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,advertising,ARRAY_SIZE(advertising),scan_response,ARRAY_SIZE(scan_response));
    if (!rc || rc==-EALREADY) {
        board_led_advertising(true);
        LOG_INF("Advertising active: %s service=0000aabb-0000-1000-8000-00805f9b34fb",CONFIG_BT_DEVICE_NAME);
        return 0;
    }
    board_led_advertising(false);
    LOG_ERR("Advertising start failed: %d; retrying in 500ms",rc);
    return rc;
}
static void advertise(struct k_work *work)
{
    ARG_UNUSED(work);
    if (atomic_get(&stopping)||ble_connected()) { return; }
    if (start_advertising()) { k_work_reschedule(&adv_work,K_MSEC(500)); }
}
static void connected(struct bt_conn *conn,uint8_t err)
{
    if (err) {
        LOG_ERR("Connection establishment failed: HCI 0x%02x",err);
        board_led_advertising(false);
        k_work_reschedule(&adv_work,K_MSEC(200)); return;
    }
    k_mutex_lock(&link_lock,K_FOREVER);
    atomic_inc(&generation); atomic_clear(&tx_fault); connection=bt_conn_ref(conn);
    board_led_connected(true); k_mutex_unlock(&link_lock);
    LOG_INF("BLE link connected; waiting for APP service discovery and CCC");
}
static void disconnected(struct bt_conn *conn,uint8_t reason)
{
    ARG_UNUSED(conn); atomic_set(&ecw_stop,1);
    /* Prevent recycled/advertise from racing teardown of the final reference. */
    if (IS_ENABLED(CONFIG_ECW_DISCONNECT_POWEROFF)) { atomic_set(&stopping,1); }
    LOG_WRN("BLE disconnected: HCI reason 0x%02x",reason);
    k_mutex_lock(&link_lock,K_FOREVER);
    atomic_inc(&generation);
    if (connection) { bt_conn_unref(connection); connection=NULL; }
    board_led_connected(false); k_mutex_unlock(&link_lock);
    if (IS_ENABLED(CONFIG_ECW_DISCONNECT_POWEROFF)) {
        board_led_shutdown();
        atomic_set(&shutdown_requested,1); k_sem_give(&rx_wakeup);
    } else { k_work_reschedule(&adv_work,K_MSEC(100)); }
}
static void recycled(void)
{
    /* CONFIG_BT_MAX_CONN=1: retry only after the last stack/TX reference is
     * returned too. A phone can disconnect before the app sees a ready link. */
    if (!atomic_get(&stopping)) { k_work_reschedule(&adv_work,K_MSEC(100)); }
}
BT_CONN_CB_DEFINE(callbacks)={.connected=connected,.disconnected=disconnected,.recycled=recycled};
static void battery_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(bat_work,battery_work);
static void battery_work(struct k_work *work)
{
    ARG_UNUSED(work);
    /* ADC reads can wait on hardware. Never block the system workqueue used by
     * advertising retries, LEDs, and Bluetooth maintenance. */
    k_sem_give(&battery_wakeup);
    if (!atomic_get(&stopping)) { k_work_reschedule(&bat_work,K_SECONDS(30)); }
}
static void battery_thread(void *a,void *b,void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        k_sem_take(&battery_wakeup,K_FOREVER);
        if (atomic_get(&stopping)) { continue; }
        int mv=board_battery_mv();
        if (mv>0) { bt_bas_set_battery_level(CLAMP((mv-3000)*100/1200,0,100)); }
        else { LOG_WRN("Battery ADC unavailable: %d",mv); }
    }
}
int ble_link_stop(void)
{
    atomic_set(&stopping,1);
    board_led_advertising(false);
    struct k_work_sync sync; k_work_cancel_delayable_sync(&adv_work,&sync);
    k_work_cancel_delayable_sync(&bat_work,&sync); return bt_disable();
}
int ble_link_init(void)
{
    LOG_INF("Starting CQET firmware 1.1.4-cv-audit (AABB source protocol)");
    int rc=bt_enable(NULL);
    if (rc) { LOG_ERR("bt_enable failed: %d",rc); return rc; }
    bt_addr_le_t identities[CONFIG_BT_ID_MAX]; size_t count=ARRAY_SIZE(identities);
    bt_id_get(identities,&count);
    if (count) {
        char address[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(&identities[0],address,sizeof(address));
        LOG_INF("BLE identity %s; firmware 1.1.4-cv-audit",address);
    }
    k_thread_create(&rx_obj,rx_stack,K_THREAD_STACK_SIZEOF(rx_stack),rx_thread,NULL,NULL,NULL,7,0,K_NO_WAIT);
    k_thread_create(&tx_obj,tx_stack,K_THREAD_STACK_SIZEOF(tx_stack),tx_thread,NULL,NULL,NULL,8,0,K_NO_WAIT);
    k_thread_create(&battery_obj,battery_stack,K_THREAD_STACK_SIZEOF(battery_stack),battery_thread,NULL,NULL,NULL,9,0,K_NO_WAIT);
    k_thread_name_set(&rx_obj,"ecw_commands"); k_thread_name_set(&tx_obj,"ecw_ble_tx");
    k_thread_name_set(&battery_obj,"ecw_battery");
    /* The first attempt runs immediately, independently of workqueue progress. */
    if (start_advertising()) { k_work_reschedule(&adv_work,K_MSEC(500)); }
    k_work_reschedule(&bat_work,K_SECONDS(2)); return 0;
}
