// frontend.cpp
// Modifications to improve signal display accuracy and reduce erroneous readings

#include "frontend.h"
#include "eConfigManager.h"
#include <cstdio>

// Function to retrieve Signal-to-Noise Ratio (SNR)
int eDVBFrontendStatus::getSNR() const
{
    // Check if frontend exists
    if (!frontend) return 0;

    // Retrieve the slot ID for the current frontend
    int slotID = frontend->getSlotID();

    // Construct the configuration parameter name based on the slot ID
    char configName[64];
    snprintf(configName, sizeof(configName), "config.Nims.%d.show_signal_below_lock", slotID);

    // Fetch the configuration value; default to true if not set
    bool showSignalBelowLock = eConfigManager::getConfigBoolValue(configName, true);

    // Determine if the tuner is locked
    bool isLocked = (getState() & tunerStateLocked) != 0;

    // If the tuner is not locked and the configuration disallows signal display, return 0
    if (!isLocked && !showSignalBelowLock)
        return 0;

    // Read and return the signal quality data
    return frontend->readFrontendData(iFrontendInformation_ENUMS::signalQuality);
}

// Function to retrieve Bit Error Rate (BER)
int eDVBFrontendStatus::getBER() const
{
    if (!frontend) return 0;
    return frontend->readFrontendData(iFrontendInformation_ENUMS::bitErrorRate);
}

// Function to retrieve Signal Strength
int eDVBFrontendStatus::getSignalStrength() const
{
    if (!frontend) return 0;
    return frontend->readFrontendData(iFrontendInformation_ENUMS::signalStrength);
}
