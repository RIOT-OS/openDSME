#ifndef CHANNELADAPTOR_H_
#define CHANNELADAPTOR_H_

#include "../mac_services/DSME_Common.h"

#include <vector>
#include <tuple>
#include <algorithm>

namespace dsme {

class DSMEAdaptionLayer;

class ChannelAdaptor {
public:
    ChannelAdaptor(DSMEAdaptionLayer&);

    /** Selects a channel for the next GTS allocation.
     *  Returns an index to a channel in the channel
     *  map, i.e., idx=0->ch=11 / idx=2->ch=13.
     *
     *  @param slotId The slot id of the GTS to allocate
     *
     *  TODO: TO BE IMPLEMENTED BY YOU!
     */
    uint8_t selectChannel(uint8_t slotId);

    /** Checks if the GTS with the given channel id
     *  shall be deallocated. True to deallocate the
     *  GTS, false to keep it. Called every Multisuperframe.
     *
     *  @param channel The channel to check
     *
     *  TODO: TO BE IMPLEMENTED BY YOU!
     */
    bool checkDeallocateGTS(uint8_t channel);

    /** Collects information about transmissions and
     *  channel quality.
     */
    void signalTransmissionStatus(uint8_t channel, uint8_t attempts, bool success);

private:
    std::vector<std::tuple<uint8_t, uint32_t, uint32_t>> channelStatusList; // Vector of tuples (channel, transmissions, successful transmissions)

    // Not of interest
    DSMEAdaptionLayer &dsmeAdaptionLayer;
    void printChannelStatusList();
};

} /* namespace dsme */

#endif /* CHANNELADAPTOR_H_ */
