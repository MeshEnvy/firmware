#pragma once

#ifdef ESP32

#include <cstdint>
#include <lofs/FsBackend.h>

// Fork-native protobuf types as forward decls so callers don't have to pull the full pb tree
// into every TU. The adapter .cpp is the single point in the meshtastic tree where these meet
// the lostar POD vocabulary.
struct _meshtastic_MeshPacket;
typedef struct _meshtastic_MeshPacket meshtastic_MeshPacket;
struct _meshtastic_User;
typedef struct _meshtastic_User meshtastic_User;

/**
 * Fork-local lostar adapter for the meshtastic fork. This is the ONLY TU in the fork that sees
 * both `meshtastic_MeshPacket` and `lostar_*` POD types in the same source file. Every other TU
 * speaks one vocabulary or the other — no ODR hazards.
 *
 * Boot ordering is split so WiFi (lofi) can be deferred until after NimBLE::init on ESP32
 * (BT/WiFi coexistence):
 *
 *   1. `lostar_mt_install(fs, self_node_num, self_pub_key)` from `main.cpp` early in setup:
 *        installs `lostar_host_ops` (send_text_dm, self_nodenum, self_pubkey), runs
 *        `lotato::init()` then `louser::init()`, applies the non-wifi meshtastic guard policy.
 *   2. `lostar_mt_start_wifi_after_ble()` from `main-esp32.cpp:setBluetoothEnable(true)` (or
 *        directly for BLE-less builds): runs `lofi::init()`, syncs `config.network` into lofi via
 *        `lostar_mt_sync_wifi_from_meshtastic_config()`, and attaches wifi guard policy.
 *   3. Per-event hooks (called from the fork's normal places):
 *        - `lostar_mt_on_text(mp)`  — from `TextMessageModule::handleReceived`
 *        - `lostar_mt_on_advert(mp, user)` — from `NodeInfoModule::handleReceivedProtobuf`
 *        - `lostar_mt_tick()` — from `MeshService::loop()`
 */
void lostar_mt_install(lofs::FSys *internal_fs, uint32_t self_node_num,
                       const uint8_t *self_pub_key_or_null);

/** Start lofi (WiFi + wifi CLI engine) and apply wifi guard policy. Must run after NimBLE::init
 *  on ESP32 when BT is enabled; safe to call unconditionally on BLE-less builds. Idempotent. */
void lostar_mt_start_wifi_after_ble();

/**
 * Mirror Meshtastic `config.network` WiFi fields into lofi (`saveWifiConnect`) for HTTP and `wifi status`.
 * Does not call `WiFi.begin` (Meshtastic owns STA). When `HAS_WIFI` and the reconnect task exists,
 * nudges WiFi reconnect so admin updates apply without reboot.
 *
 * The reverse path is `lofi_on_lo_settings_changed` → weak `lofi_on_lo_settings_changed_platform`
 * (strong in `lostar_adapter.cpp`): lofi active SSID/PSK are copied into `config.network`,
 * `config.has_network` set, and `nodeDB->saveToDisk(SEGMENT_CONFIG)` so Meshtastic admin matches
 * after `wifi connect` / `wifi forget` / `config set lofi.active.*`.
 */
void lostar_mt_sync_wifi_from_meshtastic_config(void);

/** Route a text-DM mesh packet through lostar. Returns true if consumed (host should STOP). */
bool lostar_mt_on_text(const meshtastic_MeshPacket &mp);

/** Fan a NodeInfo/User update into the lostar advert observers (lotato ingestor etc.). */
void lostar_mt_on_advert(const meshtastic_MeshPacket &mp, const meshtastic_User &user);

/** Service tick: drives lostar tick hooks (lotato ingestor, lofi scan service, ...). */
void lostar_mt_tick();

#endif  // ESP32
