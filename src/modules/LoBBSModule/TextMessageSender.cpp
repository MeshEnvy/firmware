#include "TextMessageSender.h"
#include "MeshService.h"
#include "Router.h"
#include "airtime.h"
#include "configuration.h"
#include <algorithm>
#include <cstring>

TextMessageSender::TextMessageSender(meshtastic_PortNum portNum) : concurrency::OSThread("TextMessageSender"), portNum(portNum) {}

std::vector<TextMessageSender::MessageFragment> TextMessageSender::splitMessage(const std::string &message)
{
    std::vector<MessageFragment> fragments;

    if (message.empty()) {
        return fragments;
    }

    // If entire message fits in one payload, don't split
    if (message.length() <= MAX_PAYLOAD) {
        MessageFragment frag;
        frag.text = message;
        frag.fragmentIndex = 0;
        frag.totalFragments = 1;
        fragments.push_back(frag);
        return fragments;
    }

    // Message needs splitting - calculate max length per fragment (accounting for " [n/m]" suffix)
    const size_t maxFragmentLength = MAX_PAYLOAD - PAGINATION_OVERHEAD;

    // First pass: split into raw text fragments
    std::vector<std::string> rawFragments;
    size_t pos = 0;
    while (pos < message.length()) {
        size_t remaining = message.length() - pos;
        size_t fragmentSize = std::min(remaining, maxFragmentLength);

        // If this isn't the last fragment, try to break on whitespace
        if (fragmentSize < remaining) {
            // Look backwards from fragmentSize to find last whitespace
            size_t breakPoint = fragmentSize;
            while (breakPoint > 0 && !isspace(message[pos + breakPoint])) {
                breakPoint--;
            }

            // If we found a whitespace, use it; otherwise break at max length
            if (breakPoint > 0) {
                fragmentSize = breakPoint;
                // Skip the whitespace character
                while (pos + fragmentSize < message.length() && isspace(message[pos + fragmentSize])) {
                    fragmentSize++;
                }
            }
        }

        rawFragments.push_back(message.substr(pos, fragmentSize));
        pos += fragmentSize;
    }

    // Second pass: create MessageFragment objects with pagination markers
    size_t totalFragments = rawFragments.size();
    for (size_t i = 0; i < rawFragments.size(); i++) {
        MessageFragment frag;
        char fragText[MAX_PAYLOAD + 1];
        snprintf(fragText, sizeof(fragText), "%s [%u/%u]", rawFragments[i].c_str(), (unsigned int)(i + 1),
                 (unsigned int)totalFragments);
        frag.text = fragText;
        frag.fragmentIndex = i;
        frag.totalFragments = totalFragments;
        fragments.push_back(frag);
    }

    return fragments;
}

void TextMessageSender::send(uint32_t nodeId, const std::string &message)
{
    // Split message into fully-formed fragments
    std::vector<MessageFragment> fragments = splitMessage(message);

    if (fragments.empty()) {
        LOG_DEBUG("TextMessageSender: Empty message, nothing to send");
        return;
    }

    LOG_DEBUG("TextMessageSender: Sending %u fragments for node 0x%0x", (unsigned int)fragments.size(), nodeId);

    // Find existing task for this node, or create a new one
    MessageTask *task = nullptr;
    for (auto &t : tasks) {
        if (t.nodeId == nodeId) {
            LOG_DEBUG("TextMessageSender: Found existing task for node 0x%0x", nodeId);
            task = &t;
            break;
        }
    }

    // If no existing task, create new one
    if (task == nullptr) {
        LOG_DEBUG("TextMessageSender: No existing task, creating new one");
        tasks.emplace_back();
        task = &tasks.back();
        task->nodeId = nodeId;
    }

    // Add all fragments to this task's message queue
    for (const auto &frag : fragments) {
        task->messages.push(frag);
    }

    LOG_INFO("TextMessageSender: Queued %u fragments for node 0x%0x", (unsigned int)fragments.size(), nodeId);

    // Wake up our thread to start sending
    enable(100);
}

int32_t TextMessageSender::runOnce()
{
    LOG_DEBUG("TextMessageSender: runOnce()");
    if (tasks.empty()) {
        // No messages to send, go to sleep
        LOG_DEBUG("TextMessageSender: No pending messages, going to sleep");
        return disable();
    }

    LOG_DEBUG("TextMessageSender: runOnce() - %u nodes with pending messages", (unsigned int)tasks.size());

    // Check if we can send (channel utilization)
    if (!airTime->isTxAllowedChannelUtil(true)) {
        // Channel busy, try again soon
        LOG_DEBUG("TextMessageSender: Channel busy, waiting 100ms");
        return 100; // Check again in 100ms
    }

    // 1. Pop the first task from the front
    MessageTask task = std::move(tasks.front());
    tasks.pop_front();

    // 2. Pop the first message from that task's message queue
    MessageFragment frag = std::move(task.messages.front());
    task.messages.pop();

    // Allocate and send the message
    auto msg = router->allocForSending();
    msg->decoded.portnum = portNum;
    msg->decoded.payload.size = strlen(frag.text.c_str());
    memcpy(msg->decoded.payload.bytes, frag.text.c_str(), msg->decoded.payload.size);
    msg->to = task.nodeId;
    msg->decoded.want_response = false;

    LOG_INFO("Sending out-of-band message to 0x%0x: %s", task.nodeId, frag.text.c_str());
    service->sendToMesh(msg);

    LOG_DEBUG("TextMessageSender: Sent fragment [%u/%u] to node 0x%0x", (unsigned int)(frag.fragmentIndex + 1),
              (unsigned int)frag.totalFragments, task.nodeId);

    // 3. If there are more messages in the queue, re-push the task to the end
    if (!task.messages.empty()) {
        tasks.push_back(std::move(task));
    } else {
        LOG_INFO("TextMessageSender: Completed all messages for node 0x%0x", task.nodeId);
    }

    // Successfully sent, yield immediately
    return 100;
}

void TextMessageSender::clear()
{
    tasks.clear();
    LOG_INFO("TextMessageSender: Cleared all queues");
}
