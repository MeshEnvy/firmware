#pragma once

#include "SinglePortModule.h"
#include "LoBBSDb.h"

/**
 * LoBBS (Lo-Fi Bulletin Board System) Module
 * 
 * A simple BBS-style messaging system for Meshtastic.
 * Handles text messages on TEXT_MESSAGE_APP port.
 */
class LoBBSModule : public SinglePortModule
{
  public:
    LoBBSModule();

  protected:
    /**
     * Handle an incoming message
     */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    LoBBSDb *db;

    /**
     * Validate username format
     */
    bool isValidUsername(const char *username);

    /**
     * Validate password format
     */
    bool isValidPassword(const char *password);

    /**
     * Send a text reply to a node
     */
    void sendTextReply(uint32_t toNode, const char *message);
};

