#pragma once

#ifdef ESP32

#include <cstdint>
// Fork-native protobuf types as forward decls so callers don't have to pull the full pb tree
// into every TU. The adapter .cpp is the single point in the meshtastic tree where these meet
// the lostar POD vocabulary.
struct _meshtastic_MeshPacket;
typedef struct _meshtastic_MeshPacket meshtastic_MeshPacket;
struct _meshtastic_User;
typedef struct _meshtastic_User meshtastic_User;

namespace fs {
class FS;
}

/**
 * Fork-local lostar adapter for the meshtastic fork. This is the ONLY TU in the fork that sees
 * both `meshtastic_MeshPacket` and `lostar_*` POD types in the same source file. Every other TU
 * speaks one vocabulary or the other — no ODR hazards.
 *
 * Boot ordering (`lostar_mt_install` from `main.cpp` after `MeshService::init`):
 *   1. Binds `internal_fs` to an internal `ArduinoFsVolume`, installs `lostar_host_ops`, runs
 *      `lotato::init(..., FsVolume*)`, `louser::init()`, core guard policy.
 *   2. Runs `lofi::init()`, `lostar_mt_sync_wifi_from_meshtastic_config()`, wifi guard policy.
 *      This fork sets `MESHTASTIC_EXCLUDE_BLUETOOTH` on `esp32_base`; if you re-enable BLE on
 *      ESP32, defer `lofi::init()` until after NimBLE::init (WiFi/BT coexistence) instead of step 2.
 *
 * Per-event hooks (fork’s normal call sites):
 *   - `lostar_mt_on_text(mp)` — `TextMessageModule::handleReceived`
 *   - `lostar_mt_on_advert(mp, user)` — `NodeInfoModule::handleReceivedProtobuf`
 *   - `lostar_mt_tick()` — `MeshService::loop()`
 */
void lostar_mt_install(fs::FS *internal_fs, uint32_t self_node_num, const uint8_t *self_pub_key_or_null);

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
