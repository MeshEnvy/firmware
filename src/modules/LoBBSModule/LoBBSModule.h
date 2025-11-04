#pragma once

#include "SinglePortModule.h"

/**
 * LoBBS (Lo-Fi Bulletin Board System) Module
 * 
 * A simple BBS-style messaging system for Meshtastic.
 * Handles text messages on TEXT_MESSAGE_APP port.
 */
class LoBBSModule : public SinglePortModule
{
  public:
    LoBBSModule() : SinglePortModule("LoBBS", meshtastic_PortNum_TEXT_MESSAGE_APP) {}

  protected:
    /**
     * Handle an incoming message
     */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
};

