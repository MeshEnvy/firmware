#include "TextMessageSender.h"
#include "configuration.h"
#include "airtime.h"
#include <cstring>
#include <algorithm>

TextMessageSender::TextMessageSender(std::function<void(uint32_t nodeId, const char *message)> sendCallback)
    : concurrency::OSThread("TextMessageSender"), sendCallback(sendCallback)
{
}

std::vector<std::string> TextMessageSender::splitMessage(const std::string &message)
{
    std::vector<std::string> fragments;
    
    if (message.empty()) {
        return fragments;
    }
    
    // Calculate max length per fragment (accounting for " [n/m]" suffix)
    const size_t maxFragmentLength = MAX_PAYLOAD - PAGINATION_OVERHEAD;
    
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
                while (pos + fragmentSize < message.length() && 
                       isspace(message[pos + fragmentSize])) {
                    fragmentSize++;
                }
            }
        }
        
        fragments.push_back(message.substr(pos, fragmentSize));
        pos += fragmentSize;
    }
    
    return fragments;
}

void TextMessageSender::send(uint32_t nodeId, const std::string &message)
{
    // Split message into fragments
    std::vector<std::string> fragments = splitMessage(message);
    
    if (fragments.empty()) {
        LOG_DEBUG("TextMessageSender: Empty message, nothing to send");
        return;
    }
    
    // Clear any existing queue for this node (new request replaces old)
    if (nodeQueues.find(nodeId) != nodeQueues.end()) {
        LOG_DEBUG("TextMessageSender: Replacing existing queue for node 0x%0x", nodeId);
        nodeQueues.erase(nodeId);
    }
    
    // Create queue for this node
    std::queue<MessageFragment> &queue = nodeQueues[nodeId];
    size_t totalFragments = fragments.size();
    
    for (size_t i = 0; i < fragments.size(); i++) {
        MessageFragment frag;
        
        // Add pagination marker if more than one fragment
        if (totalFragments > 1) {
            char fragText[MAX_PAYLOAD + 1];
            snprintf(fragText, sizeof(fragText), "%s [%u/%u]", 
                    fragments[i].c_str(), (unsigned int)(i + 1), (unsigned int)totalFragments);
            frag.text = fragText;
        } else {
            frag.text = fragments[i];
        }
        
        frag.fragmentIndex = i;
        frag.totalFragments = totalFragments;
        queue.push(frag);
    }
    
    updateNodeOrder();
    
    LOG_INFO("TextMessageSender: Queued %u fragments for node 0x%0x", (unsigned int)totalFragments, nodeId);
    
    // Wake up our thread to start sending
    enabled = true;
    setInterval(0);
}

int32_t TextMessageSender::runOnce()
{
    if (nodeQueues.empty()) {
        // No messages to send, go to sleep
        LOG_DEBUG("TextMessageSender: No pending messages, going to sleep");
        return disable();
    }
    
    LOG_DEBUG("TextMessageSender: runOnce() - %u nodes with pending messages", (unsigned int)nodeQueues.size());
    
    // Check if we can send (channel utilization)
    if (!airTime->isTxAllowedChannelUtil(true)) {
        // Channel busy, try again soon
        LOG_DEBUG("TextMessageSender: Channel busy, waiting 100ms");
        return 100; // Check again in 100ms
    }
    
    // Round-robin through nodes to send next fragment
    size_t attempts = 0;
    while (attempts < nodeOrder.size()) {
        uint32_t nodeId = nodeOrder[currentNodeIndex];
        
        // Move to next node for next call
        currentNodeIndex = (currentNodeIndex + 1) % nodeOrder.size();
        
        // Check if this node still has messages
        auto it = nodeQueues.find(nodeId);
        if (it != nodeQueues.end() && !it->second.empty()) {
            // Get next fragment for this node
            MessageFragment &frag = it->second.front();
            
            // Send using callback
            sendCallback(nodeId, frag.text.c_str());
            
            // Remove from queue
            it->second.pop();
            
            LOG_DEBUG("TextMessageSender: Sent fragment [%u/%u] to node 0x%0x", 
                     (unsigned int)(frag.fragmentIndex + 1), (unsigned int)frag.totalFragments, nodeId);
            
            // If this node's queue is now empty, remove it
            if (it->second.empty()) {
                LOG_INFO("TextMessageSender: Completed all fragments for node 0x%0x", nodeId);
                nodeQueues.erase(it);
                updateNodeOrder();
            }
            
            // Successfully sent, yield immediately
            return 100;
        }
        
        attempts++;
    }
    
    // No messages in queues (shouldn't happen), go to sleep
    return disable();
}

void TextMessageSender::cancel(uint32_t nodeId)
{
    auto it = nodeQueues.find(nodeId);
    if (it != nodeQueues.end()) {
        size_t remaining = it->second.size();
        nodeQueues.erase(it);
        updateNodeOrder();
        LOG_INFO("TextMessageSender: Cancelled %u fragments for node 0x%0x", (unsigned int)remaining, nodeId);
    }
}

void TextMessageSender::clear()
{
    nodeQueues.clear();
    nodeOrder.clear();
    currentNodeIndex = 0;
    LOG_INFO("TextMessageSender: Cleared all queues");
}

void TextMessageSender::updateNodeOrder()
{
    nodeOrder.clear();
    for (const auto &pair : nodeQueues) {
        if (!pair.second.empty()) {
            nodeOrder.push_back(pair.first);
        }
    }
    
    // Reset index if it's out of bounds
    if (currentNodeIndex >= nodeOrder.size()) {
        currentNodeIndex = 0;
    }
}

