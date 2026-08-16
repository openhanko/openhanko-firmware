#include "ble_transport.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

// Private 128-bit UUIDs. Nothing standard applies: there is no BLE smart-card
// profile, which is the entire reason a token driver is needed on the host.
//
//   service   6b1a0001-8f2c-4a55-9d3e-2c7a5b8e0f10
//   apdu      6b1a0002-...  host writes a framed APDU here
//   response  6b1a0003-...  device notifies the framed response
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
    0x10, 0x0f, 0x8e, 0x5b, 0x7a, 0x2c, 0x3e, 0x9d,
    0x55, 0x4a, 0x2c, 0x8f, 0x01, 0x00, 0x1a, 0x6b);
static const ble_uuid128_t APDU_UUID = BLE_UUID128_INIT(
    0x10, 0x0f, 0x8e, 0x5b, 0x7a, 0x2c, 0x3e, 0x9d,
    0x55, 0x4a, 0x2c, 0x8f, 0x02, 0x00, 0x1a, 0x6b);
static const ble_uuid128_t RESPONSE_UUID = BLE_UUID128_INIT(
    0x10, 0x0f, 0x8e, 0x5b, 0x7a, 0x2c, 0x3e, 0x9d,
    0x55, 0x4a, 0x2c, 0x8f, 0x03, 0x00, 0x1a, 0x6b);

#define APDU_CAP 1024
#define RESPONSE_CAP 2048

static ble_apdu_handler_t apdu_handler;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t response_value_handle;
static uint8_t own_address_type;

// Reassembly of an inbound APDU.
static uint8_t request[APDU_CAP];
static size_t request_expected;
static size_t request_filled;

static uint8_t response[RESPONSE_CAP];

static void start_advertising(void);

bool ble_transport_connected(void) {
  return connection_handle != BLE_HS_CONN_HANDLE_NONE;
}

static void send_response(const uint8_t *payload, size_t length) {
  if (connection_handle == BLE_HS_CONN_HANDLE_NONE) return;

  uint16_t mtu = ble_att_mtu(connection_handle);
  size_t chunk_cap = (mtu > 6) ? (size_t)(mtu - 3) : 20;

  uint8_t framed[3 + 244];
  size_t sent = 0;
  bool first = true;

  while (sent < length || first) {
    size_t header = first ? 2 : 0;
    size_t take = length - sent;
    if (take > chunk_cap - header) take = chunk_cap - header;
    if (take > sizeof(framed) - header) take = sizeof(framed) - header;

    size_t offset = 0;
    if (first) {
      framed[offset++] = (uint8_t)(length >> 8);
      framed[offset++] = (uint8_t)(length & 0xff);
      first = false;
    }
    memcpy(framed + offset, payload + sent, take);
    offset += take;
    sent += take;

    struct os_mbuf *buffer = ble_hs_mbuf_from_flat(framed, offset);
    if (!buffer) return;
    if (ble_gatts_notify_custom(connection_handle, response_value_handle, buffer) != 0) return;
  }
}

// Accumulates writes until a whole APDU has arrived, then dispatches it.
static void receive_chunk(const uint8_t *data, size_t length) {
  if (request_expected == 0) {
    if (length < 2) return;
    request_expected = ((size_t)data[0] << 8) | data[1];
    request_filled = 0;
    data += 2;
    length -= 2;
    if (request_expected == 0 || request_expected > sizeof(request)) {
      request_expected = 0;
      return;
    }
  }

  if (request_filled + length > request_expected) length = request_expected - request_filled;
  memcpy(request + request_filled, data, length);
  request_filled += length;
  if (request_filled < request_expected) return;

  size_t apdu_len = request_expected;
  request_expected = 0;
  request_filled = 0;

  size_t response_len = sizeof(response);
  if (!apdu_handler ||
      !apdu_handler(request, apdu_len, response, &response_len, sizeof(response))) {
    const uint8_t failure[] = {0x6f, 0x00};
    send_response(failure, sizeof(failure));
    return;
  }
  send_response(response, response_len);
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn_handle;
  (void)attr_handle;
  (void)arg;

  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

  uint8_t buffer[256];
  uint16_t length = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &length) != 0) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  receive_chunk(buffer, length);
  return 0;
}

static const struct ble_gatt_svc_def services[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &SERVICE_UUID.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = &APDU_UUID.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE,
      },
      {
        .uuid = &RESPONSE_UUID.u,
        .access_cb = gatt_access,
        .val_handle = &response_value_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
      },
      {0},
    },
  },
  {0},
};

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        connection_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected");
      } else {
        start_advertising();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "disconnected");
      connection_handle = BLE_HS_CONN_HANDLE_NONE;
      request_expected = 0;
      request_filled = 0;
      start_advertising();
      break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      start_advertising();
      break;

    default:
      break;
  }
  return 0;
}

static void start_advertising(void) {
  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.tx_pwr_lvl_is_present = 1;
  fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

  const char *name = ble_svc_gap_device_name();
  fields.name = (uint8_t *)name;
  fields.name_len = strlen(name);
  fields.name_is_complete = 1;

  // The 128-bit service UUID does not fit alongside the name in one 31-byte
  // advertisement, so it goes in the scan response instead.
  if (ble_gap_adv_set_fields(&fields) != 0) return;

  struct ble_hs_adv_fields scan_response = {0};
  scan_response.uuids128 = (ble_uuid128_t *)&SERVICE_UUID;
  scan_response.num_uuids128 = 1;
  scan_response.uuids128_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&scan_response);

  struct ble_gap_adv_params params = {0};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &own_address_type);
  start_advertising();
  ESP_LOGI(TAG, "advertising");
}

static void host_task(void *param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void ble_transport_start(ble_apdu_handler_t handler) {
  apdu_handler = handler;

  ESP_ERROR_CHECK(nimble_port_init());

  ble_hs_cfg.sync_cb = on_sync;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
  ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
  ESP_ERROR_CHECK(ble_svc_gap_device_name_set("smart-card"));

  nimble_port_freertos_init(host_task);
}
