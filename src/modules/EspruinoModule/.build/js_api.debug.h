/*
 * Auto-generated file - DO NOT EDIT
 * Generated from api.ts using build-espruino.py
 *
 * This file contains the JavaScript bootstrap code that initializes
 * the Meshtastic API for Espruino scripts.
 */

#ifndef JS_API_DEBUG_H
#define JS_API_DEBUG_H

// Bootstrap JavaScript code executed when Espruino initializes
const char *JS_API_BOOTSTRAP = R"js((() => {
  // types.ts
  var PortNum = {
    /** Deprecated: A message from outside the mesh (formerly OPAQUE) */
    UNKNOWN_APP: 0,
    /** Simple UTF-8 text message */
    TEXT_MESSAGE_APP: 1,
    /** Built-in GPIO/remote hardware control */
    REMOTE_HARDWARE_APP: 2,
    /** Built-in position messaging */
    POSITION_APP: 3,
    /** Built-in user info */
    NODEINFO_APP: 4,
    /** Protocol control packets for mesh routing */
    ROUTING_APP: 5,
    /** Admin control packets */
    ADMIN_APP: 6,
    /** Compressed text messages (Unishox2) */
    TEXT_MESSAGE_COMPRESSED_APP: 7,
    /** Waypoint messages */
    WAYPOINT_APP: 8,
    /** Audio payloads (codec2 frames, 2.4 GHz only) */
    AUDIO_APP: 9,
    /** Detection sensor messages */
    DETECTION_SENSOR_APP: 10,
    /** Critical alert messages */
    ALERT_APP: 11,
    /** Key verification requests */
    KEY_VERIFICATION_APP: 12,
    /** Ping service for testing */
    REPLY_APP: 32,
    /** Python IP tunnel feature */
    IP_TUNNEL_APP: 33,
    /** Paxcounter integration */
    PAXCOUNTER_APP: 34,
    /** Hardware serial interface (38400 8N1, max 240 bytes) */
    SERIAL_APP: 64,
    /** Store and forward app */
    STORE_FORWARD_APP: 65,
    /** Range test module */
    RANGE_TEST_APP: 66,
    /** Telemetry data */
    TELEMETRY_APP: 67,
    /** Zero-GPS positioning system */
    ZPS_APP: 68,
    /** Linux native app simulator */
    SIMULATOR_APP: 69,
    /** Traceroute functionality */
    TRACEROUTE_APP: 70,
    /** Network neighbor info aggregation */
    NEIGHBORINFO_APP: 71,
    /** Official Meshtastic ATAK plugin */
    ATAK_PLUGIN: 72,
    /** Unencrypted node info for MQTT map */
    MAP_REPORT_APP: 73,
    /** PowerStress monitoring */
    POWERSTRESS_APP: 74,
    /** Reticulum Network Stack tunnel */
    RETICULUM_TUNNEL_APP: 76,
    /** Arbitrary telemetry (CayenneLLP) */
    CAYENNE_APP: 77,
    /** Private applications (use >= 256) */
    PRIVATE_APP: 256,
    /** ATAK Forwarder Module (libcotshrink) */
    ATAK_FORWARDER: 257,
    /** Maximum allowed port number */
    MAX: 511
  };

  // api.ts
  var __handlers = {};
  function notImplemented() {
    throw new Error("Not implemented");
  }
  var Meshtastic = {
    hello() {
      console.log("Hello from JS");
    },
    echo(message) {
      console.log(`Echoing message from JS: ${message}`);
    },
    ping(message) {
      return message;
    },
    sendMessage(portNum, to, message) {
      MeshtasticNative.addPendingMessage(portNum, to, message);
    },
    sendTextMessage(to, message) {
      Meshtastic.sendMessage(PortNum.TEXT_MESSAGE_APP, to, message);
    },
    /** Port number constants for Meshtastic applications */
    PortNum,
    on(event, listener) {
      if (!(event in __handlers)) {
        __handlers[event] = [];
      }
      __handlers[event].push(listener);
      return () => {
        Meshtastic.removeListener(event, listener);
      };
    },
    emit(event, data) {
      if (!(event in __handlers)) {
        return;
      }
      __handlers[event].forEach((handler) => {
        handler(data);
      });
    },
    removeListener(event, listener) {
      if (!(event in __handlers)) {
        return;
      }
      __handlers[event] = __handlers[event].filter((l) => l !== listener);
    },
    onPortMessage(portNum, callback) {
      const eventName = `message:${portNum}`;
      Meshtastic.on(eventName, (data) => {
        callback(data[0], data[1]);
      });
      return () => {
        Meshtastic.removeListener(eventName, callback);
      };
    },
    onTextMessage(callback) {
      return Meshtastic.onPortMessage(PortNum.TEXT_MESSAGE_APP, callback);
    },
    onAudioMessage(callback) {
      notImplemented();
    },
    onPositionMessage(callback) {
      notImplemented();
    },
    onNodeInfoMessage(callback) {
      notImplemented();
    },
    onRoutingMessage(callback) {
      notImplemented();
    },
    onAdminMessage(callback) {
      notImplemented();
    },
    onTextMessageCompressedMessage(callback) {
      notImplemented();
    },
    onDetectionSensorMessage(callback) {
      notImplemented();
    },
    onAlertMessage(callback) {
      notImplemented();
    },
    onKeyVerificationMessage(callback) {
      notImplemented();
    },
    onReplyMessage(callback) {
      notImplemented();
    },
    onIPTunnelMessage(callback) {
      notImplemented();
    },
    onPaxcounterMessage(callback) {
      notImplemented();
    },
    onSerialMessage(callback) {
      notImplemented();
    },
    onStoreForwardMessage(callback) {
      notImplemented();
    },
    onRangeTestMessage(callback) {
      notImplemented();
    },
    onTelemetryMessage(callback) {
      notImplemented();
    },
    onZPSMessage(callback) {
      notImplemented();
    },
    onSimulatorMessage(callback) {
      notImplemented();
    },
    onTracerouteMessage(callback) {
      notImplemented();
    },
    onNeighborInfoMessage(callback) {
      notImplemented();
    },
    onATAKPluginMessage(callback) {
      notImplemented();
    },
    onMapReportMessage(callback) {
      notImplemented();
    },
    onPowerStressMessage(callback) {
      notImplemented();
    },
    onReticulumTunnelMessage(callback) {
      notImplemented();
    },
    onCayenneMessage(callback) {
      notImplemented();
    },
    onPrivateMessage(callback) {
      notImplemented();
    },
    onATAKForwarderMessage(callback) {
      notImplemented();
    },
    onWaypointMessage(callback) {
      notImplemented();
    }
  };
  var MeshtasticNative = {
    pendingMessages: [],
    addPendingMessage(portNum, to, message) {
      MeshtasticNative.pendingMessages.push({ portNum, to, message });
      console.log(
        `Added pending message to queue: ${portNum}, ${to}, ${message}`
      );
    },
    flushPendingMessages() {
      const message = MeshtasticNative.pendingMessages.shift();
      if (!message) {
        return;
      }
      const success = MeshtasticNative.sendMessage(message);
      if (!success) {
        console.log("Script: Failed to send message, queueing again");
        MeshtasticNative.pendingMessages.unshift(message);
      } else {
        console.log("Script: Message sent successfully");
      }
    },
    sendMessage(params) {
      throw new Error("sendMessage is a native function");
    }
  };
  global.Meshtastic = Meshtastic;
  global.PortNum = PortNum;
  global.MeshtasticNative = MeshtasticNative;
})();
)js";

#endif // JS_API_DEBUG_H
