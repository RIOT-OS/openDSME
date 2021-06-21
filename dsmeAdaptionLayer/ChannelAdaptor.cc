
#include "./ChannelAdaptor.h"
#include "../interfaces/IDSMEPlatform.h"
#include "../mac_services/pib/MAC_PIB.h"
#include "../../dsme_platform.h"
#include "../dsmeLayer/DSMELayer.h"

namespace dsme {

ChannelAdaptor::ChannelAdaptor(DSMEAdaptionLayer &dsmeAdaptionLayer) : dsmeAdaptionLayer{dsmeAdaptionLayer} {
}

uint8_t ChannelAdaptor::selectChannel(uint8_t slotId) {
    LOG_INFO("SELECTING CHANNEL");
    printChannelStatusList();

    // TODO implement

    // return random channel for now ...
    return dsmeAdaptionLayer.getRandom() % dsmeAdaptionLayer.getMAC_PIB().helper.getNumChannels();
}

bool ChannelAdaptor::checkDeallocateGTS(uint8_t channel) {
    //TODO implement
    return false;
}

void ChannelAdaptor::signalTransmissionStatus(uint8_t channel, uint8_t attempts, bool success) {
    // update prr of all channels -> not of interest
    auto it = std::find_if(channelStatusList.begin(), channelStatusList.end(), [channel](const std::tuple<uint8_t, uint32_t, uint32_t>& e) {return std::get<0>(e) == channel;});
    if(it != channelStatusList.end()) {
        std::get<1>(*it) += attempts;
        std::get<2>(*it) += success ? 1 : 0;
    } else {
        channelStatusList.push_back(std::tuple<uint8_t, uint32_t, uint32_t>(channel, attempts, success));
    }
}

void ChannelAdaptor::printChannelStatusList() {
    LOG_INFO("CHANNEL STATUS LIST");
    for(auto &channelStatus : channelStatusList) {
        LOG_INFO("ch: " << (int)std::get<0>(channelStatus) << "  tx: " << (long)std::get<1>(channelStatus) << "  success: " << (long)std::get<2>(channelStatus));
    }
}

} /* namespace dsme */
