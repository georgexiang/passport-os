// main/apps/link/link_ble.c —— 见 link_ble.h。NimBLE 外设,一个服务两个特征。
#include "link_ble.h"
#include "nvs.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "link.ble";
static const char *kName = "Passport-Link";

// 6a1f0001-3c55-4d2b-9a7e-5f3b2c1d0e00(NimBLE 用小端字节序)
#define AWAY_UUID(last) BLE_UUID128_INIT(0x00, 0x0e, 0x1d, 0x2c, 0x3b, 0x5f, 0x7e, 0x9a, \
                                         0x2b, 0x4d, 0x55, 0x3c, last, 0x00, 0x1f, 0x6a)
static const ble_uuid128_t SVC_UUID = AWAY_UUID(0x01);
static const ble_uuid128_t RX_UUID = AWAY_UUID(0x02);
static const ble_uuid128_t TX_UUID = AWAY_UUID(0x03);

#define RX_MAX 640   // 一行最长(Mac 端按 512B 拆快照)

static link_ble_line_cb s_cb;
static uint16_t s_tx_handle;
static atomic_uint_fast16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static atomic_bool s_subscribed;
static atomic_bool s_started;
static atomic_bool s_stopping;
static uint16_t s_mtu = 23;
static char s_rx[RX_MAX];
static size_t s_rx_len;

static void start_advertising(void);
// 用随机静态地址广播,不用芯片的公共地址:这颗芯片以前跑别的固件时可能和 Mac 配对过,
// Mac 会拿旧密钥去加密,本服务又不存配对信息,结果每次都"Peer removed pairing information"。
// 换个地址,Mac 就把它当成新设备,直接连、不加密(本服务也不需要加密)。
static uint8_t s_own_addr_type = BLE_OWN_ADDR_RANDOM;

// ---- GATT ----
static void rx_feed(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            if (s_rx_len && s_cb) s_cb(s_rx, s_rx_len);
            s_rx_len = 0;
        } else if (s_rx_len < RX_MAX) {
            s_rx[s_rx_len++] = (char)p[i];
        } else {
            s_rx_len = 0;   // 超长行:丢掉,等下一个换行重新开始
        }
    }
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[RX_MAX];
    uint16_t len = 0;
    const uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    rx_feed(buf, len);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &RX_UUID.u, .access_cb = rx_access,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = &TX_UUID.u, .access_cb = tx_access, .val_handle = &s_tx_handle,
             .flags = BLE_GATT_CHR_F_NOTIFY},
            {0},
        },
    },
    {0},
};

// ---- GAP ----
static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            atomic_store(&s_conn, ev->connect.conn_handle);
            s_rx_len = 0;
            ESP_LOGI(TAG, "connected");
        } else if (!atomic_load(&s_stopping)) {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", ev->disconnect.reason);
        atomic_store(&s_conn, BLE_HS_CONN_HANDLE_NONE);
        atomic_store(&s_subscribed, false);
        s_mtu = 23;
        s_rx_len = 0;
        if (!atomic_load(&s_stopping)) start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        // reason 0 = 广播因为连上而结束,不用重启
        if (ev->adv_complete.reason != 0 && !atomic_load(&s_stopping) &&
            atomic_load(&s_conn) == BLE_HS_CONN_HANDLE_NONE) start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            atomic_store(&s_subscribed, ev->subscribe.cur_notify);
            ESP_LOGI(TAG, "subscribe=%d", ev->subscribe.cur_notify);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        ESP_LOGI(TAG, "mtu=%u", (unsigned)s_mtu);
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void) {
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)kName;
    rsp.name_len = strlen(kName);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 广播间隔 ~500ms:找得到就行,省电
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(400);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(600);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else ESP_LOGI(TAG, "advertising as %s", kName);
}


static void on_sync(void) {
    ble_addr_t addr;
    if (ble_hs_id_gen_rnd(0, &addr) == 0 && ble_hs_id_set_rnd(addr.val) == 0) {
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else if (ble_hs_util_ensure_addr(0) == 0) {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    } else {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    start_advertising();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset reason=%d", reason); }

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---- 对外接口 ----
bool link_ble_start(link_ble_line_cb cb) {
    if (atomic_load(&s_started)) return true;
    s_cb = cb;
    s_rx_len = 0;
    atomic_store(&s_stopping, false);
    atomic_store(&s_conn, BLE_HS_CONN_HANDLE_NONE);
    atomic_store(&s_subscribed, false);
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed"); return false; }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc) {
        ESP_LOGE(TAG, "gatts setup rc=%d", rc);
        nimble_port_deinit();
        return false;
    }
    ble_svc_gap_device_name_set(kName);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    atomic_store(&s_started, true);
    ESP_LOGI(TAG, "started");
    return true;
}

void link_ble_stop(void) {
    if (!atomic_load(&s_started)) return;
    atomic_store(&s_stopping, true);
    ble_gap_adv_stop();
    const uint16_t conn = atomic_load(&s_conn);
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    int rc = -1;
    for (int i = 0; i < 3 && rc != 0; i++) {
        rc = nimble_port_stop();
        if (rc) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (rc) { ESP_LOGE(TAG, "nimble_port_stop failed rc=%d", rc); return; }
    nimble_port_deinit();
    atomic_store(&s_conn, BLE_HS_CONN_HANDLE_NONE);
    atomic_store(&s_subscribed, false);
    atomic_store(&s_started, false);
    s_cb = NULL;
    ESP_LOGI(TAG, "stopped, memory released");
}

bool link_ble_connected(void) {
    return atomic_load(&s_started) && atomic_load(&s_conn) != BLE_HS_CONN_HANDLE_NONE &&
           atomic_load(&s_subscribed);
}

bool link_ble_send(const char *line, size_t len) {
    if (!link_ble_connected()) return false;
    const uint16_t conn = atomic_load(&s_conn);
    const size_t chunk = s_mtu > 3 ? s_mtu - 3 : 20;
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(line + off, n);
        if (!om || ble_gatts_notify_custom(conn, s_tx_handle, om) != 0) return false;
    }
    return true;
}

bool link_ble_mode(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("link", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "ble", &v); nvs_close(h); }
    if (v == 0 && nvs_open("away", NVS_READONLY, &h) == ESP_OK) {   // 兼容旧版离席哨自己存的选择
        nvs_get_u8(h, "ble", &v);
        nvs_close(h);
    }
    return v != 0;
}

void link_ble_set_mode(bool ble) {
    nvs_handle_t h;
    if (nvs_open("link", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ble", ble ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    if (!ble && nvs_open("away", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, "ble"); nvs_commit(h); nvs_close(h); }
}
