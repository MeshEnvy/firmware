#include "LoBBSModule.h"
#include "MeshService.h"
#include "configuration.h"

#include <assert.h>

ProcessMessage LoBBSModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    // The incoming message is in p.payload
    LOG_INFO("Received LoBBS message from=0x%0x, id=%d, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif

    const char *replyStr = "Welcome to LoBBS! You have 0 unread messages and 0 unread news items.";
    auto reply = allocDataPacket();                 // Allocate a packet for sending
    reply->decoded.payload.size = strlen(replyStr); // You must specify how many bytes are in the reply
    memcpy(reply->decoded.payload.bytes, replyStr, reply->decoded.payload.size);

    // Set destination to sender
    reply->to = mp.from;
    reply->decoded.want_response = false;

    // Send the reply to mesh
    LOG_INFO("Sending LoBBS welcome message to 0x%0x", mp.from);
    service->sendToMesh(reply, RX_SRC_LOCAL, true);

    return ProcessMessage::CONTINUE; // Let other modules handle the message
}

