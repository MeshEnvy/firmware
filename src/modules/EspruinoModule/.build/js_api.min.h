/*
 * Auto-generated file - DO NOT EDIT
 * Generated from api.ts using build-espruino.py
 *
 * This file contains the JavaScript bootstrap code that initializes
 * the Meshtastic API for Espruino scripts.
 */

#ifndef JS_API_MIN_H
#define JS_API_MIN_H

// Bootstrap JavaScript code executed when Espruino initializes
const char *JS_API_BOOTSTRAP =
    R"js((()=>{var g={UNKNOWN_APP:0,TEXT_MESSAGE_APP:1,REMOTE_HARDWARE_APP:2,POSITION_APP:3,NODEINFO_APP:4,ROUTING_APP:5,ADMIN_APP:6,TEXT_MESSAGE_COMPRESSED_APP:7,WAYPOINT_APP:8,AUDIO_APP:9,DETECTION_SENSOR_APP:10,ALERT_APP:11,KEY_VERIFICATION_APP:12,REPLY_APP:32,IP_TUNNEL_APP:33,PAXCOUNTER_APP:34,SERIAL_APP:64,STORE_FORWARD_APP:65,RANGE_TEST_APP:66,TELEMETRY_APP:67,ZPS_APP:68,SIMULATOR_APP:69,TRACEROUTE_APP:70,NEIGHBORINFO_APP:71,ATAK_PLUGIN:72,MAP_REPORT_APP:73,POWERSTRESS_APP:74,RETICULUM_TUNNEL_APP:76,CAYENNE_APP:77,PRIVATE_APP:256,ATAK_FORWARDER:257,MAX:511};var n={};function a(){throw new Error("Not implemented")}var o={hello(){console.log("Hello from JS")},echo(e){console.log(`Echoing message from JS: ${e}`)},ping(e){return e},sendMessage(e,s,l){c.addPendingMessage(e,s,l)},sendTextMessage(e,s){o.sendMessage(g.TEXT_MESSAGE_APP,e,s)},PortNum:g,on(e,s){return e in n||(n[e]=[]),n[e].push(s),()=>{o.removeListener(e,s)}},emit(e,s){e in n&&n[e].forEach(l=>{l(s)})},removeListener(e,s){e in n&&(n[e]=n[e].filter(l=>l!==s))},onPortMessage(e,s){let l=`message:${e}`;return o.on(l,t=>{s(t[0],t[1])}),()=>{o.removeListener(l,s)}},onTextMessage(e){return o.onPortMessage(g.TEXT_MESSAGE_APP,e)},onAudioMessage(e){a()},onPositionMessage(e){a()},onNodeInfoMessage(e){a()},onRoutingMessage(e){a()},onAdminMessage(e){a()},onTextMessageCompressedMessage(e){a()},onDetectionSensorMessage(e){a()},onAlertMessage(e){a()},onKeyVerificationMessage(e){a()},onReplyMessage(e){a()},onIPTunnelMessage(e){a()},onPaxcounterMessage(e){a()},onSerialMessage(e){a()},onStoreForwardMessage(e){a()},onRangeTestMessage(e){a()},onTelemetryMessage(e){a()},onZPSMessage(e){a()},onSimulatorMessage(e){a()},onTracerouteMessage(e){a()},onNeighborInfoMessage(e){a()},onATAKPluginMessage(e){a()},onMapReportMessage(e){a()},onPowerStressMessage(e){a()},onReticulumTunnelMessage(e){a()},onCayenneMessage(e){a()},onPrivateMessage(e){a()},onATAKForwarderMessage(e){a()},onWaypointMessage(e){a()}},c={pendingMessages:[],addPendingMessage(e,s,l){c.pendingMessages.push({portNum:e,to:s,message:l}),console.log(`Added pending message to queue: ${e}, ${s}, ${l}`)},flushPendingMessages(){let e=c.pendingMessages.shift();if(!e)return;c.sendMessage(e)?console.log("Script: Message sent successfully"):(console.log("Script: Failed to send message, queueing again"),c.pendingMessages.unshift(e))},sendMessage(e){throw new Error("sendMessage is a native function")}};global.Meshtastic=o;global.PortNum=g;global.MeshtasticNative=c;})();
)js";

#endif // JS_API_MIN_H
