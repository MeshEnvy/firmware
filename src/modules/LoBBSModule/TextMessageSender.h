#pragma once

#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <cstdint>
#include <deque>
#include <queue>
#include <string>
#include <vector>

/**
 * TextMessageSender - Handles splitting and sending large text messages
 *
 * Splits messages on whitespace boundaries, adds pagination markers [n/m],
 * and round-robins between multiple recipients for fair sending.
 * Runs as its own OSThread for cooperative scheduling.
 *
 * This is a standalone component that uses global service/router for sending.
 */
class TextMessageSender : private concurrency::OSThread
{
  public:
    /**
     * Initialize the message sender
     * @param portNum The port number to send messages on (e.g. TEXT_MESSAGE_APP)
     */
    TextMessageSender(meshtastic_PortNum portNum);

    /**
     * Queue a message to be sent to a node
     * Automatically splits into fragments that fit within payload limit
     *
     * @param nodeId Destination node
     * @param message Full message text to send
     */
    void send(uint32_t nodeId, const std::string &message);

    /**
     * Clear all pending messages
     */
    void clear();

  protected:
    /**
     * OSThread callback - sends queued fragments cooperatively
     */
    virtual int32_t runOnce() override;

  private:
    meshtastic_PortNum portNum;

    // Fragment for a single message
    struct MessageFragment {
        std::string text;
        size_t fragmentIndex;
        size_t totalFragments;
    };

    // Task containing all messages for one node
    struct MessageTask {
        uint32_t nodeId;
        std::queue<MessageFragment> messages;
    };

    // Queue of tasks (one per node with pending messages)
    // Using deque so we can search for existing tasks when appending messages
    std::deque<MessageTask> tasks;

    // Constants
    static constexpr size_t MAX_PAYLOAD = 233;
    static constexpr size_t PAGINATION_OVERHEAD = 10; // " [999/999]"

    /**
     * Split a message into fragments that fit within payload limit
     * Splits on whitespace boundaries and returns fully-formed MessageFragment objects
     */
    std::vector<MessageFragment> splitMessage(const std::string &message);
};
