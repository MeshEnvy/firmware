#pragma once

#include "concurrency/OSThread.h"
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <functional>

/**
 * TextMessageSender - Handles splitting and sending large text messages
 * 
 * Splits messages on whitespace boundaries, adds pagination markers [n/m],
 * and round-robins between multiple recipients for fair sending.
 * Runs as its own OSThread for cooperative scheduling.
 */
class TextMessageSender : private concurrency::OSThread
{
  public:
    /**
     * Initialize the message sender
     * @param sendCallback Function to call to actually send a message
     */
    TextMessageSender(std::function<void(uint32_t nodeId, const char *message)> sendCallback);

    /**
     * Queue a message to be sent to a node
     * Automatically splits into fragments that fit within payload limit
     * 
     * @param nodeId Destination node
     * @param message Full message text to send
     */
    void send(uint32_t nodeId, const std::string &message);

    /**
     * Cancel all pending messages for a specific node
     * @param nodeId Node to cancel messages for
     */
    void cancel(uint32_t nodeId);

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
    std::function<void(uint32_t nodeId, const char *message)> sendCallback;
    // Fragment for a single node
    struct MessageFragment {
        std::string text;
        size_t fragmentIndex;
        size_t totalFragments;
    };

    // Queue of fragments per node
    std::map<uint32_t, std::queue<MessageFragment>> nodeQueues;
    
    // Round-robin state
    std::vector<uint32_t> nodeOrder;
    size_t currentNodeIndex = 0;

    // Constants
    static constexpr size_t MAX_PAYLOAD = 233;
    static constexpr size_t PAGINATION_OVERHEAD = 10; // " [999/999]"

    /**
     * Split a message into fragments that fit within payload limit
     * Splits on whitespace boundaries
     */
    std::vector<std::string> splitMessage(const std::string &message);

    /**
     * Update the round-robin node list
     */
    void updateNodeOrder();
};

