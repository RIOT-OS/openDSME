/*
 * openDSME
 *
 * Implementation of the Deterministic & Synchronous Multi-channel Extension (DSME)
 * described in the IEEE 802.15.4-2015 standard
 *
 * Authors: Florian Kauer <florian.kauer@tuhh.de>
 *          Maximilian Koestler <maximilian.koestler@tuhh.de>
 *          Sandrina Backhauss <sandrina.backhauss@tuhh.de>
 *
 * Based on
 *          DSME Implementation for the INET Framework
 *          Tobias Luebkert <tobias.luebkert@tuhh.de>
 *
 * Copyright (c) 2015, Institute of Telematics, Hamburg University of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "./GTSHelper.h"

#include "../../dsme_platform.h"
#include "../dsmeLayer/DSMELayer.h" // TODO: remove cross-layer reference
#include "../helper/DSMEDelegate.h"
#include "../interfaces/IDSMEPlatform.h"
#include "../mac_services/dataStructures/DSMEAllocationCounterTable.h"
#include "../mac_services/dataStructures/DSMESABSpecification.h"
#include "../mac_services/dataStructures/DSMESlotAllocationBitmap.h"
#include "../mac_services/dataStructures/IEEE802154MacAddress.h"
#include "../mac_services/mcps_sap/MCPS_SAP.h"
#include "../mac_services/mlme_sap/DSME_GTS.h"
#include "../mac_services/mlme_sap/MLME_SAP.h"
#include "../mac_services/pib/MAC_PIB.h"
#include "../mac_services/pib/PIBHelper.h"
#include "./DSMEAdaptionLayer.h"

namespace dsme {

GTSHelper::GTSHelper(DSMEAdaptionLayer& dsmeAdaptionLayer) : dsmeAdaptionLayer(dsmeAdaptionLayer), gtsConfirmPending(false), gtsDeallocConfirmPending(false) {
}

void GTSHelper::initialize(GTSScheduling* scheduling) {
    this->gtsScheduling = scheduling;

    this->dsmeAdaptionLayer.getDSME().setStartOfCFPDelegate(DELEGATE(&GTSHelper::handleStartOfCFP, *this)); /* BAD cross-layer hack */

    this->dsmeAdaptionLayer.getMLME_SAP().getDSME_GTS().indication(DELEGATE(&GTSHelper::handleDSME_GTS_indication, *this));
    this->dsmeAdaptionLayer.getMLME_SAP().getDSME_GTS().confirm(DELEGATE(&GTSHelper::handleDSME_GTS_confirm, *this));
    this->dsmeAdaptionLayer.getMLME_SAP().getCOMM_STATUS().indication(DELEGATE(&GTSHelper::handleCOMM_STATUS_indication, *this));
    return;
}

void GTSHelper::reset() {
    this->gtsConfirmPending = false;
    this->gtsDeallocConfirmPending = false;
    this->gtsScheduling->reset();
}

uint8_t GTSHelper::indicateIncomingMessage(uint16_t address) {
    return this->gtsScheduling->registerIncomingMessage(address);
}

void GTSHelper::indicateOutgoingMessage(uint16_t address, bool success, int32_t serviceTime, uint8_t queueAtCreation) {
    this->gtsScheduling->registerOutgoingMessage(address, success, serviceTime, queueAtCreation);
}

void GTSHelper::indicateReceivedMessage(uint16_t address) {
    return this->gtsScheduling->registerReceivedMessage(address);
}

void GTSHelper::handleStartOfCFP() {
    if(this->dsmeAdaptionLayer.getDSME().getCurrentSuperframe() == 0) {
        this->gtsScheduling->multisuperframeEvent();
    }

    /* Check allocation at random superframe in multi-superframe */
    //uint8_t num_superframes = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe();
    //uint8_t random_frame = this->dsmeAdaptionLayer.getDSME().getPlatform().getRandom() % num_superframes;
    //if(this->dsmeAdaptionLayer.getDSME().getCurrentSuperframe() == random_frame) {

        // proof of concept capOn capOff. idea -> to modify decision.numSlots if preferred GTS is a GTS CAP. However a GTS CAP will be allocated doubled by default.

        performSchedulingAction(this->gtsScheduling->getNextSchedulingAction()); //schedulingAction is triggered every superframe in CFP
    //}
    return;
}

void GTSHelper::checkAllocationForPacket(uint16_t address) {
    performSchedulingAction(this->gtsScheduling->getNextSchedulingAction(address));
    return;
}

void GTSHelper::performSchedulingAction(GTSSchedulingDecision decision) {
    if(decision.numSlot == 0) {
        DSME_ASSERT(decision.deviceAddress == IEEE802154MacAddress::NO_SHORT_ADDRESS);
        return;
    }

    if(decision.managementType == ManagementType::ALLOCATION) {
        checkAndAllocateGTS(decision);
    } else if(decision.managementType == ManagementType::DEALLOCATION) {

        if (useMultipleGTSDeallocation){
            if(decision.numSlot == 1){
                checkAndDeallocateSingeleGTS(decision.deviceAddress);
                LOG_DEBUG(". Inside GTSHelper performSchedulingAction->deallocate single slot? : "<< (uint16_t)decision.numSlot);
            }

        }else{
            checkAndDeallocateSingeleGTS(decision.deviceAddress);
        }

    } else {
        DSME_ASSERT(false);
    }
    return;
}

void GTSHelper::checkAndAllocateGTS(GTSSchedulingDecision decision) {
    DSME_ATOMIC_BLOCK {
        if(gtsConfirmPending) {
            LOG_INFO("GTS allocation still active (trying with 0x" << HEXOUT << decision.deviceAddress << DECOUT << ")");
            return;
        }

        gtsConfirmPending = true;
    }

    DSMEAllocationCounterTable& macDSMEACT = this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT;
    DSMESlotAllocationBitmap& macDSMESAB = this->dsmeAdaptionLayer.getMAC_PIB().macDSMESAB;

    uint8_t numChannels = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumChannels();

    GTS preferredGTS = getNextFreeGTS(decision.preferredSuperframeId, decision.preferredSlotId);

    if(preferredGTS == GTS::UNDEFINED) {
        LOG_ERROR("No free GTS found! (trying with 0x" << HEXOUT << decision.deviceAddress << DECOUT << ")");
        gtsConfirmPending = false;
        return;
    }

    mlme_sap::DSME_GTS::request_parameters params;
    params.deviceAddress = decision.deviceAddress;
    params.managementType = ManagementType::ALLOCATION;
    params.direction = decision.direction;
    params.prioritizedChannelAccess = Priority::LOW;

    //Proof Of concept CAPON capOFf. Idea is to check if preferred slot is in CFP or CAP.
    if((preferredGTS.superframeID !=0) && (preferredGTS.slotID < 8)){
        LOG_DEBUG(". Inside CheckAndAllocateGTS. vaule of preferred slotId: " << (uint16_t)preferredGTS.slotID);
        params.numSlot = 1;
        }else{
            params.numSlot = decision.numSlot;
        }
    params.preferredSuperframeId = preferredGTS.superframeID;
    params.preferredSlotId = preferredGTS.slotID;

    params.dsmeSabSpecification.setSubBlockLengthBytes(this->dsmeAdaptionLayer.getMAC_PIB().helper.getSubBlockLengthBytes(preferredGTS.superframeID));
    params.dsmeSabSpecification.setSubBlockIndex(preferredGTS.superframeID);
    macDSMESAB.getOccupiedSubBlock(params.dsmeSabSpecification, preferredGTS.superframeID);

    LOG_INFO("ALLOCATING slot " << preferredGTS.slotID << " " << preferredGTS.superframeID << " " << (uint16_t)preferredGTS.channel << " with 0x" << HEXOUT
                                << params.deviceAddress << DECOUT << ".");

    /* mark all impossible slots that are in use in other channels, too */
    for(DSMEAllocationCounterTable::iterator it = macDSMEACT.begin(); it != macDSMEACT.end(); ++it) {
        if(it->getSuperframeID() == preferredGTS.superframeID) {
            for(uint8_t channel = 0; channel < numChannels; channel++) {
                params.dsmeSabSpecification.getSubBlock().set(it->getGTSlotID() * numChannels + channel, true);
            }
        }
    }

    this->dsmeAdaptionLayer.getMLME_SAP().getDSME_GTS().request(params);
    return;
}

void GTSHelper::checkAndDeallocateSingeleGTS(uint16_t address) {
    // Code For single GTS deallocation
    DSMEAllocationCounterTable& act = this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT;
    int16_t highestIdleCounter = -1;
    DSMEAllocationCounterTable::iterator toDeallocate = act.end();
    bool foundGTSCAP = false;

    for(auto it = act.begin(); it != act.end(); ++it) {
            if(it->getDirection() == Direction::TX && it->getAddress() == address) {
                if(it->getState() == ACTState::VALID && it->getSuperframeID()!=0 &&
                        it->getGTSlotID()<8 && it->getIdleCounter() > highestIdleCounter) {
                    highestIdleCounter = it->getIdleCounter();
                    toDeallocate = it;
                    foundGTSCAP = true;
                }
            }
        }

    if (!foundGTSCAP){
        for(auto it = act.begin(); it != act.end(); ++it) {
            if(it->getDirection() == Direction::TX && it->getAddress() == address) {
                if((it->getState() == ACTState::VALID && it->getGTSlotID()>7 && it->getGTSlotID()<15 && it->getSuperframeID()!=0
                        && it->getIdleCounter() > highestIdleCounter) || (it->getState() == ACTState::VALID && it->getSuperframeID()==0 && it->getGTSlotID()<7
                                && it->getIdleCounter() > highestIdleCounter)) {
                    highestIdleCounter = it->getIdleCounter();
                    toDeallocate = it;
                }
            }
        }
    }

    if(toDeallocate != act.end()) {
        LOG_INFO("DEALLOCATING slot " << toDeallocate->getSuperframeID() << "/" << toDeallocate->getGTSlotID() << " with 0x" << HEXOUT
                                      << toDeallocate->getAddress() << DECOUT);

        DSMESABSpecification dsmeSABSpecification;
        uint8_t subBlockLengthBytes = this->dsmeAdaptionLayer.getMAC_PIB().helper.getSubBlockLengthBytes(toDeallocate->getSuperframeID());
        dsmeSABSpecification.setSubBlockLengthBytes(subBlockLengthBytes);
        dsmeSABSpecification.setSubBlockIndex(toDeallocate->getSuperframeID());
        dsmeSABSpecification.getSubBlock().fill(false);


        dsmeSABSpecification.getSubBlock().set(
            toDeallocate->getGTSlotID() * this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumChannels() + toDeallocate->getChannel(), true);

        sendDeallocationRequest(toDeallocate->getAddress(), toDeallocate->getDirection(), dsmeSABSpecification);
    }
}

void GTSHelper::handleCOMM_STATUS_indication(mlme_sap::COMM_STATUS_indication_parameters& params) {
    LOG_INFO("COMM_STATUS indication handled.");

    // TODO should we do anything here? especially for failures?

    return;
}

void GTSHelper::handleDSME_GTS_indication(mlme_sap::DSME_GTS_indication_parameters& params) {
    if(!this->dsmeAdaptionLayer.getMAC_PIB().macAssociatedPANCoord) {
        LOG_INFO("Not associated, discarding incoming GTS request from " << params.deviceAddress << ".");
        return;
    }

    LOG_INFO("GTS request handled.");

    mlme_sap::DSME_GTS::response_parameters responseParams;
    responseParams.deviceAddress = params.deviceAddress;
    responseParams.managementType = params.managementType;
    responseParams.direction = params.direction;
    responseParams.prioritizedChannelAccess = params.prioritizedChannelAccess;

    bool sendReply = true;

    switch(params.managementType) {
        case ALLOCATION: {
            responseParams.dsmeSabSpecification.setSubBlockLengthBytes(params.dsmeSabSpecification.getSubBlockLengthBytes());
            responseParams.dsmeSabSpecification.setSubBlockIndex(params.dsmeSabSpecification.getSubBlockIndex());

            DSME_ASSERT(params.dsmeSabSpecification.getSubBlockIndex() == params.preferredSuperframeId);

            findFreeSlots(params.dsmeSabSpecification, responseParams.dsmeSabSpecification, params.numSlot, params.preferredSuperframeId,
                          params.preferredSlotId);

            responseParams.channelOffset = dsmeAdaptionLayer.getMAC_PIB().macChannelOffset;
            if(responseParams.dsmeSabSpecification.getSubBlock().isZero()) {
                LOG_ERROR("Unable to allocate GTS.");
                responseParams.status = GTSStatus::DENIED;
            } else {
                responseParams.status = GTSStatus::SUCCESS;
            }
            break;
        }
        case DUPLICATED_ALLOCATION_NOTIFICATION: {
            LOG_ERROR("DUPLICATED_ALLOCATION_NOTIFICATION");
            //ASSERT(FALSE);
            // TODO what shall we do here? With this the deallocation could be to aggressive
            uint16_t address = IEEE802154MacAddress::NO_SHORT_ADDRESS;
            Direction direction;
            responseParams.status = verifyDeallocation(params.dsmeSabSpecification, address, direction);

            if(responseParams.status == GTSStatus::SUCCESS) {
                // Now handled by the ACTUpdater this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT.setACTState(params.dsmeSABSpecification, INVALID);
            } else {
                // the deallocated slot is not allocated, so send back as DENIED
            }

            sendReply = false; // TODO correct?

            break;
        }
        case DEALLOCATION: {
            Direction directionUnused;
            responseParams.status = verifyDeallocation(params.dsmeSabSpecification, params.deviceAddress, directionUnused);

            if(responseParams.status == GTSStatus::GTS_Status::SUCCESS) {
                // TODO remove
                // only set to INVALID here and remove them not before NOTIFY
                // if anything goes wrong, the slot will be deallocated later again
                // Now handled by the ACTUpdater this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT.setACTState(params.dsmeSABSpecification, INVALID);
                //this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT.setACTState(params.dsmeSABSpecification, INVALID);// ADDED to test
            }

            responseParams.dsmeSabSpecification = params.dsmeSabSpecification;
            break;
        }
        case EXPIRATION:
            // In this implementation EXPIRATION is only issued while no confirm is pending
            // DSME_ASSERT(!gtsConfirmPending);

            // TODO is this required?
            // this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT.setACTState(params.dsmeSABSpecification, DEALLOCATED);

            sendDeallocationRequest(params.deviceAddress, params.direction, params.dsmeSabSpecification);
            sendReply = false;
            break;
        default:
            DSME_ASSERT(false);
            break;
    }

    if(sendReply) {
        this->dsmeAdaptionLayer.getMLME_SAP().getDSME_GTS().response(responseParams);
    }

    return;
}

void GTSHelper::sendDeallocationRequest(uint16_t address, Direction direction, DSMESABSpecification& sabSpecification) {
    mlme_sap::DSME_GTS::request_parameters params;
    params.deviceAddress = address;
    params.managementType = ManagementType::DEALLOCATION;
    params.direction = direction;
    params.prioritizedChannelAccess = Priority::LOW;

    params.numSlot = 0;               // ignored
    params.preferredSuperframeId = 0; // ignored
    params.preferredSlotId = 0;       // ignored

    params.dsmeSabSpecification = sabSpecification;

    LOG_INFO("Deallocating slot with " << params.deviceAddress << ".");

    this->dsmeAdaptionLayer.getMLME_SAP().getDSME_GTS().request(params);

    return;
}

const char* printStatus(GTSStatus::GTS_Status status) {
    switch(status) {
        case GTSStatus::SUCCESS:
            return "SUCCESS";
        case GTSStatus::DENIED:
            return "DENIED";
        case GTSStatus::INVALID_PARAMETER:
            return "INVALID_PARAMETER";
        case GTSStatus::NO_ACK:
            return "NO_ACK";
        case GTSStatus::NO_DATA:
            return "NO_DATA";
        case GTSStatus::CHANNEL_ACCESS_FAILURE:
            return "CHANNEL_ACCESS_FAILURE";
        case GTSStatus::NO_SHORT_ADDRESS:
            return "NO_SHORT_ADDRESS";
        case GTSStatus::TRANSACTION_OVERFLOW:
            return "TRANSACTION_OVERFLOW";
    }

    DSME_ASSERT(false);
    return "UNKNOWN";
}

void GTSHelper::handleDSME_GTS_confirm(mlme_sap::DSME_GTS_confirm_parameters& params) {
    LOG_DEBUG("GTS confirmation handled (Status: " << printStatus(params.status) << ").");

    // TODO handle channel access failure! retransmission?

    if(params.managementType == ManagementType::ALLOCATION) {
        gtsConfirmPending = false;
        LOG_DEBUG("gtsConfirmPending = false");
        if(params.status == GTSStatus::SUCCESS) {
            LOG_DEBUG(". GTS confirmed");
            this->dsmeAdaptionLayer.getMessageHelper().sendRetryBuffer();
            performSchedulingAction(this->gtsScheduling->getNextSchedulingAction()); //  line that is used to enable if consecutive GTS negotiations are triggered
        }
    }
    return;
}

GTS GTSHelper::getNextFreeGTS(uint16_t initialSuperframeID, uint8_t initialSlotID, const DSMESABSpecification* sabSpec) {
    GTS gts(0,0,0);

    if (((initialSlotID < 7) && (initialSuperframeID == 0)) || ((initialSlotID > 7) && (initialSlotID < 15) && (initialSuperframeID != 0))){ //SF=0 , slots between 0-6 are part of CFP
        gts = getNextFreeGTS_CAP_CFP(initialSuperframeID, initialSlotID, sabSpec, GTSType::GTS_CFP);
        if (gts == GTS::UNDEFINED){
            initialSlotID = 0;
            if(initialSuperframeID == 0){
                return GTS::UNDEFINED;
            }
            gts = getNextFreeGTS_CAP_CFP(initialSuperframeID, initialSlotID, sabSpec, GTSType::GTS_CAP);
            return gts;
            }
        return gts;

        }else if((initialSuperframeID !=0) && (initialSlotID < 8)){
            gts = getNextFreeGTS_CAP_CFP(initialSuperframeID, initialSlotID, sabSpec, GTSType::GTS_CAP);
            if (gts == GTS::UNDEFINED){
                initialSlotID = 8;
                gts = getNextFreeGTS_CAP_CFP(initialSuperframeID, initialSlotID, sabSpec, GTSType::GTS_CFP);
                return gts;
            }
            return gts;
    }else if (((initialSlotID > 6) && (initialSlotID < 15) && (initialSuperframeID == 0))){
        return GTS::UNDEFINED;
    }
    return GTS::UNDEFINED;
}

GTS GTSHelper::getNextFreeGTS_CAP_CFP(uint16_t initialSuperframeID, uint8_t initialSlotID, const DSMESABSpecification* sabSpec, GTSType typeOfGTS) {


    DSMEAllocationCounterTable& macDSMEACT = this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT;
    DSMESlotAllocationBitmap& macDSMESAB = this->dsmeAdaptionLayer.getMAC_PIB().macDSMESAB;

    uint8_t numChannels = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumChannels();
    uint8_t numSuperFramesPerMultiSuperframe = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe();

    uint16_t slotsToCheck = 0;
    uint8_t numGTSlots = 0;

    GTS gts(0, 0, 0);

    BitVector<MAX_CHANNELS> occupied;
    BitVector<MAX_CHANNELS> remoteOccupied; // only used if sabSpec != nullptr
    occupied.setLength(numChannels);

    if(sabSpec != nullptr) {
            DSME_ASSERT(sabSpec->getSubBlockIndex() == initialSuperframeID);
            remoteOccupied.setLength(numChannels);
        }

    if (typeOfGTS == GTSType::GTS_CFP){

        slotsToCheck = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0) +
                       (this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe() - 1) *
                       this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0);

        numGTSlots = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0); // numGTS in SF


    }else if (typeOfGTS == GTSType::GTS_CAP){

        gts = GTS(1,0,0);

        slotsToCheck = (this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe() - 1) *
                        (this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0) +1); // 8 slots per SF
        numGTSlots = (this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0)+1); // numGTS available in CAP 8 slots

        if(gts.superframeID ==0){
            LOG_DEBUG("gts undefined because there is no slots for CAP in SF 0");
            return GTS::UNDEFINED;
            }
    }

    LOG_DEBUG(". initialSFid -> " << (uint16_t) initialSuperframeID << " initial slotID -> "<< (uint16_t)initialSlotID);
    for(gts.superframeID = initialSuperframeID; slotsToCheck > 0; gts.superframeID = (gts.superframeID + 1) % numSuperFramesPerMultiSuperframe) {


        if(sabSpec != nullptr && gts.superframeID != initialSuperframeID) {
            // currently per convention a sub block holds exactly one superframe
            return GTS::UNDEFINED;
        }


        //redefinition of superframe id for gts in cfp, when superframeId ==0
        if(typeOfGTS == GTSType::GTS_CAP){
            // proof of concept CAPON OFF. idea is to select only solts number 7 to 15. for selecting only available slots in CAP when CAPreduction is on or on/off
            if(gts.superframeID ==0){
                LOG_DEBUG(". getnextFreeGTS -> superframeID==0 bypass SF = 0");
                gts.superframeID=1;
            }
        }

         LOG_INFO("Checking " <<  (uint16_t) numGTSlots << " in superframe " << gts.superframeID);

         // TODO in case the initial slot id is not in the range (8,14) is it neccesary?
         uint8_t delimiter = numGTSlots;
         if (typeOfGTS == GTSType::GTS_CFP){
             if(gts.superframeID!=0){
                 delimiter = 15; //the limit for slots in CFP for SF!=0
             }
         }

         for(gts.slotID = initialSlotID % delimiter; slotsToCheck > 0; gts.slotID = (gts.slotID + 1) % delimiter) {

             if (typeOfGTS == GTSType::GTS_CFP){
                 if(gts.superframeID!=0){
                     delimiter =15;
                     if (gts.slotID < 8){
                         gts.slotID = 8;
                     }
                 }else if(gts.superframeID==0){
                     delimiter = numGTSlots;
                 }
             }
            if(!macDSMEACT.isAllocated(gts.superframeID, gts.slotID)) {

                uint8_t startChannel = this->dsmeAdaptionLayer.getDSME().getPlatform().getRandom() % numChannels;
                macDSMESAB.getOccupiedChannels(occupied, gts.superframeID, gts.slotID);
                if(sabSpec != nullptr) {
                    remoteOccupied.copyFrom(sabSpec->getSubBlock(), gts.slotID * numChannels);
                    occupied.setOperationJoin(remoteOccupied);
                }

                gts.channel = startChannel;
                for(uint8_t i = 0; i < numChannels; i++) {
                    if(!occupied.get(gts.channel)) {
                        /* found one */
                        //PRoof of concept CAPon CAPoff. Idea-> after a GTS is found in GTS_CAP. check if the next one is available as well
                        if((typeOfGTS == GTSType::GTS_CAP)){
                            // TODO CHECK if slot is odd and THE NEXT SLOT IS AVAILABLE(?)

                            if ((gts.superframeID != 0) && (gts.slotID < 8)){
                                return gts;
                            }

                        }else if (typeOfGTS == GTSType::GTS_CFP){
                            if(((gts.superframeID==0)&&(gts.slotID<7))||((gts.superframeID!=0) && (gts.slotID>7) && (gts.slotID <15))){
                                return gts;
                            }

                        }

                    }

                    gts.channel++;
                    if(gts.channel == numChannels) {
                        gts.channel = 0;
                    }
                }
            }
            slotsToCheck--;

            // calculate the next slot id to check

            //special case is for CFP next slot, in which the next slot is not in range slotId(8,14)
            if((typeOfGTS == GTSType::GTS_CFP)){

                if(((gts.slotID+1)% 15 <8) && (gts.superframeID!=0)){
                gts.slotID = 7;
                }
                if((gts.superframeID!=0)) {
                    if ((((gts.slotID+1)%15) == initialSlotID)){
                        LOG_DEBUG(". getnextFreeGTS -> gts.slotsID+ 1 == initialSuperframeID");
                        break;
                    }
                }else if((gts.superframeID==0)) {
                    if ((gts.slotID+1)%this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0) == initialSlotID) {
                        LOG_DEBUG(". getnextFreeGTS -> gts.slotsID+ 1 == initialSuperframeID");
                        break;
                    }
                }

            }else if(typeOfGTS == GTSType::GTS_CAP){
                if(gts.superframeID !=0){
                    if((gts.slotID+1)%((this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumGTSlots(0))+1) == initialSlotID) {
                        LOG_DEBUG(". getnextFreeGTS -> gts.slotsID+ 1 == initialSuperframeID");
                        break;
                    }
                }
            }
        }
    }

    return GTS::UNDEFINED;
}


GTSStatus::GTS_Status GTSHelper::verifyDeallocation(DSMESABSpecification& requestSABSpec, uint16_t& deviceAddress, Direction& direction) {
    GTSStatus::GTS_Status result;

    DSMEAllocationCounterTable& macDSMEACT = this->dsmeAdaptionLayer.getMAC_PIB().macDSMEACT;

    uint8_t numChannels = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumChannels();

    bool foundGts = false;
    bool gtsDifferentAddresses = false;
    uint16_t foundDeviceAddress = IEEE802154MacAddress::NO_SHORT_ADDRESS;

    for(DSMEAllocationCounterTable::iterator it = macDSMEACT.begin(); it != macDSMEACT.end(); ++it) {
        abs_slot_idx_t idx = it->getGTSlotID();
        idx *= numChannels;
        idx += it->getChannel();

        if(it->getSuperframeID() != requestSABSpec.getSubBlockIndex() || !requestSABSpec.getSubBlock().get(idx)) {
            continue; // no deallocation requested
        }

        if(deviceAddress == IEEE802154MacAddress::NO_SHORT_ADDRESS) {
            deviceAddress = it->getAddress();
            direction = it->getDirection();
            if (!foundGts){
                foundGts = true;
                foundDeviceAddress = it->getAddress();
            }else if (foundDeviceAddress != deviceAddress){
                gtsDifferentAddresses = true;
            }

        } else if(deviceAddress == it->getAddress()) {
            if (!foundGts){
                foundGts = true;
                foundDeviceAddress = it->getAddress();
            }else if (foundDeviceAddress != deviceAddress){
                gtsDifferentAddresses = true;
            }
        }
    }

    if(gtsDifferentAddresses) {
        // TODO handle multiple requests or also send (INVALID_PARAMETER)?!
        // DSME_ASSERT(false); // TODO ?
        // TODO This could also mean that the slot is in use with another node, better DENIED?
        return GTSStatus::DENIED;
    }

    if(foundGts) {
        result = GTSStatus::SUCCESS;
    } else {
        result = GTSStatus::DENIED;
    }

    return result;
}

void GTSHelper::findFreeSlots(DSMESABSpecification& requestSABSpec, DSMESABSpecification& replySABSpec, uint8_t numSlots, uint16_t preferredSuperframe,
                              uint8_t preferredSlot) {
    const uint8_t numChannels = this->dsmeAdaptionLayer.getMAC_PIB().helper.getNumChannels();

    GTS gts = GTS(preferredSuperframe,preferredSlot,0);

    for(uint8_t i = 0; i < numSlots; i++) {

        //proof of concept CAPoff capOn
        if (i == 0){
            gts = getNextFreeGTS(preferredSuperframe, preferredSlot, &requestSABSpec);
        }

        if(gts == GTS::UNDEFINED) {
            break;
        }

        /* mark slot as allocated */
        replySABSpec.getSubBlock().set(gts.slotID * numChannels + gts.channel, true);

        if(i < numSlots) {
            /* mark already allocated slots as occupied for next round */
            for(uint8_t channel = 0; channel < numChannels; channel++) {
                requestSABSpec.getSubBlock().set(gts.slotID * numChannels + channel, true);
            }

            gts.slotID = gts.slotID+ 1;
        }
    }
    return;
}

void GTSHelper::setUseMultipleGTSDeallocation(bool useMultipleGTSDeallocation) {
    this->useMultipleGTSDeallocation = useMultipleGTSDeallocation;
}


} /* namespace dsme */
