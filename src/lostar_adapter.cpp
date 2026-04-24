#include "lostar_adapter.h"

#if defined(ESP32) && defined(LOTATO_PLATFORM_MESHTASTIC)

#include <Arduino.h>
#include <cstddef>
#include <cstring>

// Fork-native includes. THIS is the fence: meshtastic_MeshPacket / meshtastic_User only exist
// inside this TU in the lotato+lo-star integration. The lostar POD types below never coexist
// with these in any other TU.
#include "gps/RTC.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "mesh/RadioInterface.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

// lo-star / lotato / lostar API surface. All headers below are POD-only — no nanopb types.
#include <Lotato.h>
#include <lofi/Lofi.h>
#include <lolog/LoLog.h>
#include <lostar/Busy.h>
#include <lostar/Deferred.h>
#include <lostar/Host.h>
#include <lostar/Router.h>
#include <lostar/Types.h>
#include <lomessage/Queue.h>
#include <louser/Engine.h>
#include <louser/Guard.h>
#include <louser/LoUser.h>

#include <cstdlib>

// Fork globals from meshtastic/src/main.cpp and friends.
extern MeshService *service;
extern Router      *router;

namespace {

/*
 * CLI reply chunking — `lomessage` split budget (adapter-specific).
 *
 * Meshtastic caps the *encoded* `meshtastic_Data` blob in `Router::perhapsEncode`:
 *   numbytes + MESHTASTIC_HEADER_LENGTH <= MAX_LORA_PAYLOAD_LEN   (channel crypto)
 *   numbytes + MESHTASTIC_HEADER_LENGTH + MESHTASTIC_PKC_OVERHEAD <= MAX_LORA_PAYLOAD_LEN   (PKI)
 *
 * `meshtastic_Constants_DATA_PAYLOAD_LEN` (233) is the protobuf *field* max for `payload.bytes`,
 * not “guaranteed safe UTF-8 length after framing,” so we derive a plaintext budget from the
 * radio constants and a conservative protobuf slack.
 *
 * Override from PlatformIO: -DLOTATO_MT_CLI_REPLY_CHUNK_BYTES=180
 *                            -DLOTATO_MT_CLI_REPLY_INTER_CHUNK_MS=300
 */
#if defined(LOTATO_MT_CLI_REPLY_CHUNK_BYTES)
constexpr size_t kMtCliReplyChunkBytes = (size_t)LOTATO_MT_CLI_REPLY_CHUNK_BYTES;
#else
constexpr size_t kMtMaxEncodedDataPki =
    (size_t)(MAX_LORA_PAYLOAD_LEN - MESHTASTIC_HEADER_LENGTH - MESHTASTIC_PKC_OVERHEAD);
/** Nanopb overhead for `Data` (portnum, bitfield, bytes length prefix, etc.) — not exported by upstream. */
constexpr size_t kMtDataProtobufSlackBytes = 36;
static_assert(kMtMaxEncodedDataPki > kMtDataProtobufSlackBytes, "meshtastic radio header math");
constexpr size_t kMtCliReplyChunkBytes = kMtMaxEncodedDataPki - kMtDataProtobufSlackBytes;
#endif

static_assert(kMtCliReplyChunkBytes > 0 && kMtCliReplyChunkBytes <= meshtastic_Constants_DATA_PAYLOAD_LEN,
              "CLI reply chunk must fit Data.payload capacity");

#if defined(LOTATO_MT_CLI_REPLY_INTER_CHUNK_MS)
constexpr unsigned long kMtCliReplyInterChunkMs = (unsigned long)LOTATO_MT_CLI_REPLY_INTER_CHUNK_MS;
#else
constexpr unsigned long kMtCliReplyInterChunkMs = 250;
#endif

/* ── Layout sentinels: catch drift between adapter TU and lostar TU compile flavors. ────
 *
 * Values assume the 32-bit-pointer ABI used by every MCU target (ESP32, nRF52). The matching
 * sentinels in the lostar TU live in `lo-star/lostar/Host.cpp` — any mismatch would be a
 * compile error on either side instead of a silent runtime bug.
 */
#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(sizeof(lostar_TextDm)         == 56,  "lostar_TextDm layout changed");
static_assert(sizeof(lostar_NodeAdvert)     == 104, "lostar_NodeAdvert layout changed");
static_assert(sizeof(lostar_host_ops)       == 20,  "lostar_host_ops layout changed");
static_assert(sizeof(lostar_deferred_reply) == 8,   "lostar_deferred_reply layout changed");
#endif

/* ── host_ops + single-packet text TX ─────────────────────────────────────────────────
 *
 * `perhapsEncode` rejects oversize `Data` with Routing_Error_TOO_LARGE (7). PKI adds 12 bytes
 * overhead, so keep per-chunk text well under `meshtastic_Constants_DATA_PAYLOAD_LEN` after
 * protobuf framing. Long CLI replies (e.g. `help`) must be split — see `g_mt_reply_queue`. */

/** Unix rx_time for packets we originate. `Router::allocForSending` only sets FromNet-valid time;
 *  if the radio clock is merely RTCQualityDevice (phone/RTC, no mesh time yet), that is 0 and
 *  clients render 1969/1970. Fall back like PositionModule-style stamping. */
static uint32_t mt_packet_rx_time_now() {
  uint32_t t = getValidTime(RTCQualityFromNet);
  if (t != 0) return t;
  t = getValidTime(RTCQualityDevice);
  if (t != 0) return t;
  return getTime(false);
}

static void mt_send_text_one(uint32_t to, const char *text, uint32_t len) {
  if (!router || !service || !text || len == 0) {
    ::lolog::LoLog::warn("lostar.mt", "send_text_one skipped router=%p service=%p text=%p len=%u",
                         router, service, text, (unsigned)len);
    return;
  }
  meshtastic_MeshPacket *p = router->allocForSending();
  if (!p) {
    ::lolog::LoLog::warn("lostar.mt", "send_text_one allocForSending returned null");
    return;
  }
  p->to                 = to;
  p->decoded.portnum    = meshtastic_PortNum_TEXT_MESSAGE_APP;
  p->want_ack           = false;
  uint32_t cap          = (uint32_t)sizeof(p->decoded.payload.bytes);
  if (len > cap) len = cap;
  memcpy(p->decoded.payload.bytes, text, len);
  p->decoded.payload.size = (uint16_t)len;
  p->rx_time              = mt_packet_rx_time_now();
  service->sendToMesh(p, RX_SRC_LOCAL, true);
}

void mt_send_text_dm(void * /*ctx*/, uint32_t to, const char *text, uint32_t len) {
  mt_send_text_one(to, text, len);
}

class MeshtasticReplySink : public lomessage::Sink {
public:
  lomessage::SendResult sendChunk(const uint8_t *data, size_t len, size_t /*chunk_idx*/,
                                  size_t /*total_chunks*/, bool /*is_final*/,
                                  void *user_ctx) override {
    if (!data || len == 0 || !user_ctx) return lomessage::SendResult::Abandon;
    uint32_t to = 0;
    std::memcpy(&to, user_ctx, sizeof(to));
    char tmp[meshtastic_Constants_DATA_PAYLOAD_LEN + 1];
    if (len > sizeof(tmp) - 1) len = sizeof(tmp) - 1;
    std::memcpy(tmp, data, len);
    tmp[len] = '\0';
    mt_send_text_one(to, tmp, (uint32_t)len);
    return lomessage::SendResult::Sent;
  }
};

lomessage::Queue       g_mt_reply_queue;
MeshtasticReplySink    g_mt_reply_sink;

static bool mt_reply_queue_busy(void * /*ctx*/) { return !g_mt_reply_queue.empty(); }

uint32_t mt_self_nodenum(void * /*ctx*/) {
  return nodeDB ? nodeDB->getNodeNum() : 0;
}

int mt_self_pubkey(void * /*ctx*/, uint8_t out[32]) {
  if (owner.public_key.size != 32) {
    memset(out, 0, 32);
    return 0;
  }
  memcpy(out, owner.public_key.bytes, 32);
  return 1;
}

/* ── deferred reply: route_ctx encodes the `from` nodenum directly (no pool needed) ───── */

void mt_fire_reply(void *route_ctx, const char *text, uint32_t len) {
  const uint32_t to = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(route_ctx));
  if (!text || len == 0 || !router || !service) return;

  if (len <= kMtCliReplyChunkBytes) {
    mt_send_text_one(to, text, len);
    return;
  }

  char *buf = (char *)std::malloc(len + 1);
  if (!buf) {
    mt_send_text_one(to, "Err - OOM", 9);
    return;
  }
  std::memcpy(buf, text, len);
  buf[len] = '\0';

  lomessage::Options opts;
  opts.max_chunk            = kMtCliReplyChunkBytes;
  opts.inter_chunk_delay_ms = kMtCliReplyInterChunkMs;
  opts.split_flags          = lomessage::CHUNK_ABSORB_LINE_BOUNDARY;

  uint32_t to_blob = to;
  if (!g_mt_reply_queue.send(buf, &to_blob, sizeof(to_blob), opts, millis())) {
    std::free(buf);
    mt_send_text_one(to, "Err - reply queue", 17);
    return;
  }
  std::free(buf);
}

/* ── guard policy (mirrors the pre-refactor MeshtasticDelegate attach_meshtastic_guards) ─ */

void apply_core_policy() {
  auto &rt = lostar::router();
  if (auto *eng = rt.engineByName("lotato")) {
    eng->setGuardFor("pause",    &louser::require_admin);
    eng->setGuardFor("resume",   &louser::require_admin);
    eng->setGuardFor("endpoint", &louser::require_admin);
    eng->setGuardFor("auth",     &louser::require_admin);
    eng->setGuardFor("ingest",   &louser::require_admin);
  }
  if (auto *eng = rt.engineByName("config")) {
    eng->setGuardFor("set",   &louser::require_admin);
    eng->setGuardFor("unset", &louser::require_admin);
  }
}

void apply_wifi_policy() {
  auto &rt = lostar::router();
  if (auto *eng = rt.engineByName("wifi")) {
    eng->setGuardFor("scan",    &louser::require_user);
    eng->setGuardFor("connect", &louser::require_admin);
    eng->setGuardFor("forget",  &louser::require_admin);
  }
}

/* ── install state ──────────────────────────────────────────────────────────────────── */

bool g_installed   = false;
bool g_wifi_begun  = false;

}  // namespace

/* ── public entry points ────────────────────────────────────────────────────────────── */

void lostar_mt_install(lofs::FSys *internal_fs, uint32_t /*self_node_num*/,
                       const uint8_t * /*self_pub_key_or_null*/) {
  if (g_installed) return;
  g_installed = true;

  lostar_host_ops ops{};
  ops.size         = (uint32_t)sizeof(ops);
  ops.send_text_dm = &mt_send_text_dm;
  ops.self_nodenum = &mt_self_nodenum;
  ops.self_pubkey  = &mt_self_pubkey;
  ops.ctx          = nullptr;
  lostar_install_host(&ops);

  lotato::init(LOSTAR_PROTOCOL_MESHTASTIC, internal_fs);
  louser::init();
  apply_core_policy();

  lostar_register_busy_hint(&mt_reply_queue_busy, nullptr);
}

void lostar_mt_start_wifi_after_ble() {
  if (g_wifi_begun) return;
  g_wifi_begun = true;
  lofi::init();
  apply_wifi_policy();
  lostar_mt_sync_wifi_from_meshtastic_config();
}

void lostar_mt_sync_wifi_from_meshtastic_config() {
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
  if (!config.has_network) return;
  const auto &n = config.network;
  if (n.wifi_enabled && n.wifi_ssid[0]) {
    lofi::Lofi::instance().saveWifiConnect(n.wifi_ssid, n.wifi_psk[0] ? n.wifi_psk : "");
  } else {
    lofi::Lofi::instance().saveWifiConnect("", "");
  }
  if (wifiReconnect) {
    needReconnect = true;
    wifiReconnect->setIntervalFromNow(100);
  }
#endif
}

extern "C" void lofi_on_lo_settings_changed_platform(void) {
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
  if (!nodeDB) return;
  char ssid[33]{};
  char psk[65]{};
  lofi::Lofi::instance().getActiveCredentials(ssid, sizeof(ssid), psk, sizeof(psk));
  config.has_network = true;
  if (ssid[0] != '\0') {
    config.network.wifi_enabled = true;
    strncpy(config.network.wifi_ssid, ssid, sizeof(config.network.wifi_ssid) - 1);
    config.network.wifi_ssid[sizeof(config.network.wifi_ssid) - 1] = '\0';
    strncpy(config.network.wifi_psk, psk, sizeof(config.network.wifi_psk) - 1);
    config.network.wifi_psk[sizeof(config.network.wifi_psk) - 1] = '\0';
  } else {
    config.network.wifi_enabled = false;
    config.network.wifi_ssid[0] = '\0';
    config.network.wifi_psk[0] = '\0';
  }
  nodeDB->saveToDisk(SEGMENT_CONFIG);
#endif
}

bool lostar_mt_on_text(const meshtastic_MeshPacket &mp) {
  if (!g_installed) return false;
  if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return false;
  if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) return false;
  const uint32_t self = mt_self_nodenum(nullptr);
  if (mp.to != self) return false;

  lostar_TextDm dm{};
  dm.from        = mp.from;
  dm.to          = mp.to;
  dm.rx_time     = mp.rx_time;
  dm.payload     = mp.decoded.payload.bytes;
  dm.payload_len = mp.decoded.payload.size;
  if (mp.public_key.size == 32) {
    memcpy(dm.from_pubkey, mp.public_key.bytes, 32);
    dm.from_pubkey_len = 32;
  }

  lostar_deferred_reply d{};
  d.fire      = &mt_fire_reply;
  d.route_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(mp.from));
  lostar_ingress_attach_deferrer(&d);

  const bool consumed = lostar_ingress_text_dm(&dm);
  lostar_ingress_attach_deferrer(nullptr);
  return consumed;
}

void lostar_mt_on_advert(const meshtastic_MeshPacket &mp, const meshtastic_User &user) {
  if (!g_installed) return;

  lostar_NodeAdvert adv{};
  adv.protocol    = LOSTAR_PROTOCOL_MESHTASTIC;
  adv.advert_type = LOSTAR_ADVERT_TYPE_UNKNOWN;
  adv.num         = mp.from;
  adv.last_heard  = mp.rx_time;
  adv.hw_model    = (uint16_t)user.hw_model;

  static_assert(sizeof(adv.long_name)  >= sizeof(user.long_name),
                "lostar long_name must hold meshtastic user.long_name");
  static_assert(sizeof(adv.short_name) >= sizeof(user.short_name),
                "lostar short_name must hold meshtastic user.short_name");
  memcpy(adv.long_name, user.long_name, sizeof(user.long_name));
  adv.long_name[sizeof(adv.long_name) - 1]   = '\0';
  memcpy(adv.short_name, user.short_name, sizeof(user.short_name));
  adv.short_name[sizeof(adv.short_name) - 1] = '\0';

  if (user.public_key.size == 32) {
    memcpy(adv.public_key, user.public_key.bytes, 32);
    adv.public_key_len = 32;
  }

  lostar_ingress_node_advert(&adv);
}

void lostar_mt_tick() {
  if (!g_installed) return;
  g_mt_reply_queue.service(millis(), g_mt_reply_sink);
  lostar_tick();
}

#endif  // ESP32 && LOTATO_PLATFORM_MESHTASTIC
