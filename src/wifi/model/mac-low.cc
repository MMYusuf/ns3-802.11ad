/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2005,2006 INRIA
 * Copyright (c) 2009 MIRKO BANCHI
 * Copyright (c) 2015-2019 IMDEA Networks Institute
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 *          Mirko Banchi <mk.banchi@gmail.com>
 *          Stefano Avallone <stavallo@unina.it>
 *          Hany Assasa <hany.assasa@gmail.com>
 */

#include "ns3/simulator.h"
#include "ns3/log.h"
#include "mac-low.h"
#include "qos-txop.h"
#include "snr-tag.h"
#include "ampdu-tag.h"
#include "wifi-mac-queue.h"
#include "wifi-psdu.h"
#include "wifi-utils.h"
#include "ctrl-headers.h"
#include "mgt-headers.h"
#include "wifi-remote-station-manager.h"
#include "mpdu-aggregator.h"
#include "msdu-aggregator.h"
#include "ampdu-subframe-header.h"
#include "dmg-wifi-mac.h"
#include "dmg-wifi-phy.h"
#include "dmg-sta-wifi-mac.h"
#include "wifi-phy-listener.h"
#include "wifi-mac-trailer.h"
#include "wifi-phy.h"
#include "wifi-net-device.h"
#include "wifi-mac.h"
#include <algorithm>
#include "wifi-ack-policy-selector.h"
#include "control-trailer.h"

#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT std::clog << "[mac=" << m_self << "] "

// Time (in nanoseconds) to be added to the PSDU duration to yield the duration
// of the timer that is started when the PHY indicates the start of the reception
// of a frame and we are waiting for a response.
#define PSDU_DURATION_SAFEGUARD 400

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("MacLow");

/**
 * Listener for PHY events. Forwards to MacLow
 */
class PhyMacLowListener : public ns3::WifiPhyListener
{
public:
  /**
   * Create a PhyMacLowListener for the given MacLow.
   *
   * \param macLow
   */
  PhyMacLowListener (ns3::MacLow *macLow)
    : m_macLow (macLow)
  {
  }
  virtual ~PhyMacLowListener ()
  {
  }
  void NotifyRxStart (Time duration)
  {
  }
  void NotifyRxEndOk (void)
  {
  }
  void NotifyRxEndError (void)
  {
  }
  void NotifyTxStart (Time duration, double txPowerDbm)
  {
  }
  void NotifyMaybeCcaBusyStart (Time duration)
  {
  }
  void NotifySwitchingStart (Time duration)
  {
    m_macLow->NotifySwitchingStartNow (duration);
  }
  void NotifySleep (void)
  {
    m_macLow->NotifySleepNow ();
  }
  void NotifyOff (void)
  {
    m_macLow->NotifyOffNow ();
  }
  void NotifyWakeup (void)
  {
  }
  void NotifyOn (void)
  {
  }

private:
  ns3::MacLow *m_macLow; ///< the MAC
};


MacLow::MacLow ()
  : m_msduAggregator (0),
    m_mpduAggregator (0),
    m_normalAckTimeoutEvent (),
    m_blockAckTimeoutEvent (),
    m_ctsTimeoutEvent (),
    m_sendCtsEvent (),
    m_sendAckEvent (),
    m_sendDataEvent (),
    m_waitIfsEvent (),
    m_endTxNoAckEvent (),
    m_currentPacket (0),
    m_currentTxop (0),
    m_lastNavStart (Seconds (0)),
    m_lastNavDuration (Seconds (0)),
    m_cfpStart (Seconds (0)),
    m_lastBeacon (Seconds (0)),
    m_cfpForeshortening (Seconds (0)),
    m_promisc (false),
    m_phyMacLowListener (0),
    m_ctsToSelfSupported (false),
    m_cfAckInfo (),
    m_transmissionSuspended (false),
    m_restoredSuspendedTransmission (false),
    m_servingSLS (false),
    m_servingMimoBFT (false)
{
  NS_LOG_FUNCTION (this);
}

MacLow::~MacLow ()
{
  NS_LOG_FUNCTION (this);
}

/* static */
TypeId
MacLow::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MacLow")
    .SetParent<Object> ()
    .SetGroupName ("Wifi")
    .AddConstructor<MacLow> ()
  ;
  return tid;
}

void
MacLow::SetupPhyMacLowListener (const Ptr<WifiPhy> phy)
{
  m_phyMacLowListener = new PhyMacLowListener (this);
  phy->RegisterListener (m_phyMacLowListener);
}

void
MacLow::RemovePhyMacLowListener (Ptr<WifiPhy> phy)
{
  if (m_phyMacLowListener != 0 )
    {
      phy->UnregisterListener (m_phyMacLowListener);
      delete m_phyMacLowListener;
      m_phyMacLowListener = 0;
    }
}

void
MacLow::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_normalAckTimeoutEvent.Cancel ();
  m_blockAckTimeoutEvent.Cancel ();
  m_ctsTimeoutEvent.Cancel ();
  m_sendCtsEvent.Cancel ();
  m_sendAckEvent.Cancel ();
  m_sendDataEvent.Cancel ();
  m_waitIfsEvent.Cancel ();
  m_endTxNoAckEvent.Cancel ();
  m_msduAggregator = 0;
  m_mpduAggregator = 0;
  m_phy = 0;
  m_stationManager = 0;
  if (m_phyMacLowListener != 0)
    {
      delete m_phyMacLowListener;
      m_phyMacLowListener = 0;
    }
}

void
MacLow::CancelAllEvents (void)
{
  NS_LOG_FUNCTION (this);
  bool oneRunning = false;
  if (m_normalAckTimeoutEvent.IsRunning ())
    {
      m_normalAckTimeoutEvent.Cancel ();
      oneRunning = true;
    }
  if (m_blockAckTimeoutEvent.IsRunning ())
    {
      m_blockAckTimeoutEvent.Cancel ();
      oneRunning = true;
    }
  if (m_ctsTimeoutEvent.IsRunning ())
    {
      m_ctsTimeoutEvent.Cancel ();
      oneRunning = true;
    }
  if (m_sendCtsEvent.IsRunning ())
    {
      m_sendCtsEvent.Cancel ();
      oneRunning = true;
    }
  if (m_sendAckEvent.IsRunning ())
    {
      m_sendAckEvent.Cancel ();
      oneRunning = true;
    }
  if (m_sendDataEvent.IsRunning ())
    {
      m_sendDataEvent.Cancel ();
      oneRunning = true;
    }
  if (m_waitIfsEvent.IsRunning ())
    {
      m_waitIfsEvent.Cancel ();
      oneRunning = true;
    }
  if (m_endTxNoAckEvent.IsRunning ())
    {
      m_endTxNoAckEvent.Cancel ();
      oneRunning = true;
    }
  if (oneRunning && m_currentTxop != 0)
    {
      m_currentTxop->Cancel ();
      m_currentTxop = 0;
    }
}

void
MacLow::SetPhy (const Ptr<WifiPhy> phy)
{
  m_phy = phy;
  m_phy->TraceConnectWithoutContext ("PhyRxPayloadBegin", MakeCallback (&MacLow::RxStartIndication, this));
  m_phy->SetReceiveOkCallback (MakeCallback (&MacLow::DeaggregateAmpduAndReceive, this));
  m_phy->SetReceiveErrorCallback (MakeCallback (&MacLow::ReceiveError, this));
  SetupPhyMacLowListener (phy);
}

Ptr<WifiPhy>
MacLow::GetPhy (void) const
{
  return m_phy;
}

void
MacLow::ResetPhy (void)
{
  m_phy->TraceDisconnectWithoutContext ("PhyRxPayloadBegin", MakeCallback (&MacLow::RxStartIndication, this));
  m_phy->SetReceiveOkCallback (MakeNullCallback<void, Ptr<WifiPsdu>, double, WifiTxVector, std::vector<bool>> ());
  m_phy->SetReceiveErrorCallback (MakeNullCallback<void, Ptr<WifiPsdu>> ());
  RemovePhyMacLowListener (m_phy);
  m_phy = 0;
}

Ptr<QosTxop>
MacLow::GetEdca (uint8_t tid) const
{
  auto it = m_edca.find (QosUtilsMapTidToAc (tid));
  NS_ASSERT (it != m_edca.end ());
  return it->second;
}

void
MacLow::SetMac (const Ptr<WifiMac> mac)
{
  m_mac = mac;
}

void
MacLow::SetWifiRemoteStationManager (const Ptr<WifiRemoteStationManager> manager)
{
  m_stationManager = manager;
}

Ptr<MsduAggregator>
MacLow::GetMsduAggregator (void) const
{
  return m_msduAggregator;
}

Ptr<MpduAggregator>
MacLow::GetMpduAggregator (void) const
{
  return m_mpduAggregator;
}

void
MacLow::SetMsduAggregator (const Ptr<MsduAggregator> aggr)
{
  NS_LOG_FUNCTION (this << aggr);
  m_msduAggregator = aggr;
}

void
MacLow::SetMpduAggregator (const Ptr<MpduAggregator> aggr)
{
  NS_LOG_FUNCTION (this << aggr);
  m_mpduAggregator = aggr;
}

void
MacLow::SetAddress (Mac48Address ad)
{
  m_self = ad;
}

void
MacLow::SetAckTimeout (Time ackTimeout)
{
  m_ackTimeout = ackTimeout;
}

void
MacLow::SetBasicBlockAckTimeout (Time blockAckTimeout)
{
  m_basicBlockAckTimeout = blockAckTimeout;
}

void
MacLow::SetCompressedBlockAckTimeout (Time blockAckTimeout)
{
  m_compressedBlockAckTimeout = blockAckTimeout;
}

void
MacLow::SetCtsToSelfSupported (bool enable)
{
  m_ctsToSelfSupported = enable;
}

bool
MacLow::GetCtsToSelfSupported (void) const
{
  return m_ctsToSelfSupported;
}

void
MacLow::SetSifs (Time sifs)
{
  m_sifs = sifs;
}

//// WIGIG ////
void
MacLow::SetSbifs (Time sbifs)
{
  m_sbifs = sbifs;
}

void
MacLow::SetMbifs (Time mbifs)
{
  m_mbifs = mbifs;
}

void
MacLow::SetLbifs (Time lbifs)
{
  m_lbifs = lbifs;
}

void
MacLow::SetBrifs (Time brifs)
{
  m_brifs = brifs;
}
//// WIGIG ////

void
MacLow::SetSlotTime (Time slotTime)
{
  m_slotTime = slotTime;
}

void
MacLow::SetPifs (Time pifs)
{
  m_pifs = pifs;
}

void
MacLow::SetRifs (Time rifs)
{
  m_rifs = rifs;
}

void
MacLow::SetBeaconInterval (Time interval)
{
  m_beaconInterval = interval;
}

void
MacLow::SetCfpMaxDuration (Time cfpMaxDuration)
{
  m_cfpMaxDuration = cfpMaxDuration;
}

void
MacLow::SetBssid (Mac48Address bssid)
{
  m_bssid = bssid;
}

void
MacLow::SetPromisc (void)
{
  m_promisc = true;
}

Mac48Address
MacLow::GetAddress (void) const
{
  return m_self;
}

Time
MacLow::GetAckTimeout (void) const
{
  return m_ackTimeout;
}

Time
MacLow::GetBasicBlockAckTimeout (void) const
{
  return m_basicBlockAckTimeout;
}

Time
MacLow::GetCompressedBlockAckTimeout (void) const
{
  return m_compressedBlockAckTimeout;
}

Time
MacLow::GetSifs (void) const
{
  return m_sifs;
}

Time
MacLow::GetRifs (void) const
{
  return m_rifs;
}

Time
MacLow::GetSlotTime (void) const
{
  return m_slotTime;
}

Time
MacLow::GetPifs (void) const
{
  return m_pifs;
}

//// WIGIG ////
Time
MacLow::GetSbifs (void) const
{
  return m_sbifs;
}

Time
MacLow::GetMbifs (void) const
{
  return m_mbifs;
}

Time
MacLow::GetLbifs (void) const
{
  return m_lbifs;
}

Time
MacLow::GetBrifs (void) const
{
  return m_brifs;
}
//// WIGIG ////

Mac48Address
MacLow::GetBssid (void) const
{
  return m_bssid;
}

Time
MacLow::GetBeaconInterval (void) const
{
  return m_beaconInterval;
}

Time
MacLow::GetCfpMaxDuration (void) const
{
  return m_cfpMaxDuration;
}

bool
MacLow::IsPromisc (void) const
{
  return m_promisc;
}

void
MacLow::SetRxCallback (Callback<void, Ptr<WifiMacQueueItem>> callback)
{
  m_rxCallback = callback;
}

void
MacLow::RegisterChannelAccessManager (Ptr<ChannelAccessManager> channelAccessManager)
{
  m_channelAccessManagers.push_back (channelAccessManager);
}

//// WIGIG ////

bool
MacLow::IsCurrentAllocationEmpty (void) const
{
  return m_currentAllocation == 0;
}

void
MacLow::ResumeTransmission (Time duration, Ptr<Txop> txop)
{
  NS_LOG_FUNCTION (this << duration << txop);

  NS_ASSERT_MSG (!IsCurrentAllocationEmpty (), "Restored allocation should not be empty");

  NS_LOG_DEBUG ("IsAmpdu=" << m_currentAllocation->psdu->IsAggregate ()
                << ", PacketSize=" << m_currentAllocation->psdu->GetSize ()
                << ", seq=0x" << std::hex << m_currentAllocation->psdu->GetHeader (0).GetSequenceControl () << std::dec);

  /* Restore the vaiables associated to the current allocation */
  m_restoredSuspendedTransmission = false;
  m_currentPacket = m_currentAllocation->psdu;
  m_txParams = m_currentAllocation->txParams;
  m_currentTxVector = m_currentAllocation->txVector;

  /* Check if the remaining time is enough to resume previously suspended transmission */
  Time transactionTime = CalculateWiGigTransactionTime (m_currentPacket);
  NS_LOG_DEBUG ("TransactionTime=" << transactionTime <<
                ", RemainingTime=" << txop->GetAllocationRemaining ());

  if (transactionTime <= duration)
    {
      /* This only applies for service period */
      CancelAllEvents ();
      m_currentTxop = txop;

      if (m_txParams.MustSendRts ())
        {
          SendRtsForPacket ();
        }
      else
        {
          SendDataPacket ();
        }

      /* When this method completes, either we have taken ownership of the medium or the device switched off in the meantime. */
      NS_ASSERT (m_phy->IsStateTx () || m_phy->IsStateOff ());
    }
  else
    {
      m_transmissionSuspended = true;
    }

  /* Remove suspended allocaion related parameters as we've restored it */
  m_allocationPeriodsTable.erase (m_currentAllocationID);
}

void
MacLow::ChangeAllocationPacketsAddress (AllocationID allocationId, Mac48Address destAdd)
{
  NS_LOG_FUNCTION (this << uint16_t (allocationId) << destAdd);
  /* Find the stored parameters and packets for the provided allocation */
  AllocationPeriodsTableI it = m_allocationPeriodsTable.find (m_currentAllocationID);
  if (it != m_allocationPeriodsTable.end ())
    {
      NS_LOG_DEBUG ("Changing Receiver Address for Packets stored for AllocationID=" << uint16_t (allocationId));
//      it->second.psdu.SetAddr1 (destAdd);
    }
  else
    {
      NS_LOG_DEBUG ("No allocation parameters stored for AllocationID=" << uint16_t (allocationId));
    }
}

void
MacLow::RestoreAllocationParameters (AllocationID allocationId)
{
  NS_LOG_FUNCTION (this << static_cast<uint16_t> (allocationId));
  m_transmissionSuspended = false;  /* Transmission is not suspended anymore */
  m_currentAllocationID = allocationId;
  /* Find the stored parameters and packets for the provided allocation */
  AllocationPeriodsTableCI it = m_allocationPeriodsTable.find (m_currentAllocationID);
  if (it != m_allocationPeriodsTable.end ())
    {
      NS_LOG_DEBUG ("Restored allocation parameters for AllocationID=" << uint16_t (allocationId));
      m_currentAllocation = it->second;
      m_restoredSuspendedTransmission = true;
    }
  else
    {
      NS_LOG_DEBUG ("No allocation parameters stored for AllocationID=" << uint16_t (allocationId));
      m_restoredSuspendedTransmission = false;
      m_currentAllocation = 0;
    }
}

void
MacLow::StoreAllocationParameters (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("CurrentPacket=" << m_currentPacket);
  if (m_currentPacket != 0
      && m_currentPacket->GetHeader (0).IsQosData ())
    {
      /* Since CurrentPacket is not empty it means we've suspended an ongoing transmission */
      Ptr<AllocationParameters> allocationParams = Create<AllocationParameters> ();
      allocationParams->psdu = m_currentPacket;
      allocationParams->txParams = m_txParams;
      allocationParams->txVector = m_currentTxVector;
      allocationParams->txop = m_currentTxop;
      m_allocationPeriodsTable[m_currentAllocationID] = allocationParams;
      NS_LOG_DEBUG ("PSDU Size=" << m_currentPacket->GetSize ()
                    << ", seq=0x" << std::hex << m_currentPacket->GetHeader (0).GetSequenceControl () << std::dec
                    << ", Txop=" << m_currentTxop);
    }
  m_currentPacket = 0;
  m_currentAllocation = 0;
}

void
MacLow::EndAllocationPeriod (void)
{
  NS_LOG_FUNCTION (this);
  CancelAllEvents ();
  StoreAllocationParameters ();
  if (m_navCounterResetCtsMissed.IsRunning ())
    {
      m_navCounterResetCtsMissed.Cancel ();
    }
  m_lastNavStart = Simulator::Now ();
  m_lastNavDuration = Seconds (0);
  m_currentTxop = 0;
//  m_phy->EndAllocationPeriod ();
}

bool
MacLow::IsTransmissionSuspended (void) const
{
  return m_transmissionSuspended;
}

bool
MacLow::CompletedSuspendedPsduTransmission (Ptr<Txop> txop) const
{
  NS_LOG_FUNCTION (this << txop);
  NS_LOG_DEBUG ("Restored Suspended Transmission=" << m_restoredSuspendedTransmission);
  if (m_restoredSuspendedTransmission
      && !IsCurrentAllocationEmpty ()
      && m_currentAllocation->txop == txop)
    {
      return false;
    }
  else
    {
      return true;
    }
}
//// WIGIG ////

void
MacLow::StartTransmission (Ptr<WifiMacQueueItem> mpdu,
                           MacLowTransmissionParameters params,
                           Ptr<Txop> txop)
{
  NS_LOG_FUNCTION (this << *mpdu << params << txop);
  NS_ASSERT (!m_cfAckInfo.expectCfAck);
  if (m_phy->IsStateOff ())
    {
      NS_LOG_DEBUG ("Cannot start TX because device is OFF");
      return;
    }
  /* m_currentPacket is not NULL because someone started
   * a transmission and was interrupted before one of:
   *   - ctsTimeout
   *   - sendDataAfterCTS
   * expired. This means that one of these timers is still
   * running. They are all cancelled below anyway by the
   * call to CancelAllEvents (because of at least one
   * of these two timers) which will trigger a call to the
   * previous listener's cancel method.
   *
   * This typically happens because the high-priority
   * QapScheduler has taken access to the channel from
   * one of the EDCA of the QAP.
   */
  m_currentPacket = Create<WifiPsdu> (mpdu, false);
  const WifiMacHeader& hdr = mpdu->GetHeader ();
  CancelAllEvents ();
  m_currentTxop = txop;
  m_txParams = params;
  if (hdr.IsCtl ())
    {
      m_currentTxVector = GetRtsTxVector (mpdu);
    }
  else
    {
      m_currentTxVector = GetDataTxVector (mpdu);
    }

  /* The packet received by this function can be any of the following:
   * (a) a management frame dequeued from the Txop
   * (b) a non-QoS data frame dequeued from the Txop
   * (c) a non-broadcast QoS Data frame peeked or dequeued from a QosTxop
   * (d) a broadcast QoS data or DELBA Request frame dequeued from a QosTxop
   * (e) a BlockAckReq or ADDBA Request frame
   * (f) a fragment of non-QoS/QoS Data frame dequeued from the Txop/QosTxop
   */
  if (hdr.IsQosData () && !hdr.GetAddr1 ().IsBroadcast ()
      && !hdr.IsMoreFragments () && hdr.GetFragmentNumber () == 0)
    {
      // We get here if the received packet is a non-broadcast QoS data frame
      uint8_t tid = hdr.GetQosTid ();
      Ptr<QosTxop> qosTxop = m_edca.find (QosUtilsMapTidToAc (tid))->second;

      // if a TXOP limit exists, compute the remaining TXOP duration
      Time txopLimit = Time::Min ();
      //// WIGIG ////
      if (m_currentTxop->GetTxopLimit ().IsStrictlyPositive () || m_currentTxop->GetAllocationRemaining ().IsStrictlyPositive ())
        {
          txopLimit = m_currentTxop->GetPpduDurationLimit (mpdu, params);
          //NS_ASSERT (txopLimit.IsPositive ());
        }   
      // QosTxop may send us a peeked frame
      Ptr<const WifiMacQueueItem> tmp = qosTxop->PeekFrameForTransmission ();
      //// WIGIG ////
      bool isPeeked = (tmp != 0 && tmp->GetPacket () == mpdu->GetPacket ());

      Ptr<WifiMacQueueItem> newMpdu;
      // If the frame has been peeked, dequeue it if it meets the size and duration constraints
      if (isPeeked)
        {
          newMpdu = qosTxop->DequeuePeekedFrame (mpdu, m_currentTxVector, true, 0, txopLimit);
        }
      else if (IsWithinSizeAndTimeLimits (mpdu, m_currentTxVector, 0, txopLimit))
        {
          newMpdu = mpdu;
        }

      if (newMpdu == 0)
        {
          // if the frame has been dequeued, then there is no BA agreement with the
          // receiver (otherwise the frame would have been peeked). Hence, the frame
          // has been sent under Normal Ack policy, not acknowledged and now retransmitted.
          // If we cannot send it now, let the QosTxop retransmit it again.
          // If the frame has been just peeked, reset the current packet at QosTxop.
          if (isPeeked)
            {
              //// WIGIG ////
              m_currentPacket = 0;
              //// WIGIG ////
              qosTxop->UpdateCurrentPacket (Create<WifiMacQueueItem> (nullptr, WifiMacHeader ()));
            }
          return;
        }
      // Update the current packet at QosTxop, given that A-MSDU aggregation may have
      // been performed on the peeked frame
      qosTxop->UpdateCurrentPacket (newMpdu);

      //// WIGIG ////
      /* Since we might perform A-MPDU aggregation, update txopLimit to take into account the correct
       * size of A-MPDU aggregation */
      if (m_currentTxop->GetTxopLimit ().IsStrictlyPositive () || m_currentTxop->GetAllocationRemaining ().IsStrictlyPositive ())
        {
          /* Get temporary TransmissionLow Parameters for A-MPDU */
          MacLowTransmissionParameters tempParams = qosTxop->GetAckPolicySelector ()->GetTemporaryParams (m_currentPacket, params);
          txopLimit = m_currentTxop->GetPpduDurationLimit (mpdu, tempParams);
//          NS_ASSERT (txopLimit.IsPositive ());
        }
      //// WIGIG ////

      //Perform MPDU aggregation if possible
      std::vector<Ptr<WifiMacQueueItem>> mpduList;
      if (m_mpduAggregator != 0)
        {
          mpduList = m_mpduAggregator->GetNextAmpdu (newMpdu, m_currentTxVector, txopLimit);
        }

      if (mpduList.size () > 1)
        {
          m_currentPacket = Create<WifiPsdu> (mpduList);

          NS_LOG_DEBUG ("tx unicast A-MPDU containing " << mpduList.size () << " MPDUs");
          qosTxop->SetAmpduExist (hdr.GetAddr1 (), true);
        }
      else if (m_currentTxVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_VHT
               || m_currentTxVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_HE)
        {
          // VHT/HE single MPDU
          m_currentPacket = Create<WifiPsdu> (newMpdu, true);

          NS_LOG_DEBUG ("tx unicast S-MPDU with sequence number " << hdr.GetSequenceNumber ());
          qosTxop->SetAmpduExist (hdr.GetAddr1 (), true);
        }
      else  // HT
        {
          m_currentPacket = Create<WifiPsdu> (newMpdu, false);
        }

      // A QoS Txop must have an installed ack policy selector
      NS_ASSERT (qosTxop->GetAckPolicySelector () != 0);
      qosTxop->GetAckPolicySelector ()->UpdateTxParams (m_currentPacket, m_txParams);
      qosTxop->GetAckPolicySelector ()->SetAckPolicy (m_currentPacket, m_txParams);
    }

  NS_LOG_DEBUG ("startTx size=" << m_currentPacket->GetSize () <<
                ", to=" << m_currentPacket->GetAddr1 () << ", txop=" << m_currentTxop);

  if (m_txParams.MustSendRts ())
    {
      SendRtsForPacket ();
    }
  else
    {
      if ((m_ctsToSelfSupported || m_stationManager->GetUseNonErpProtection ()) && NeedCtsToSelf ())
        {
          SendCtsToSelf ();
        }
      else
        {
          SendDataPacket ();
        }
    }

  /* When this method completes, either we have taken ownership of the medium or the device switched off in the meantime. */
  NS_ASSERT (m_phy->IsStateTx () || m_phy->IsStateOff ());
}

//// WIGIG ////
void
MacLow::TransmitSingleFrame (Ptr<WifiMacQueueItem> mpdu,
                             MacLowTransmissionParameters params,
                             Ptr<Txop> txop)
{
  NS_LOG_FUNCTION (this << *mpdu << params << txop);
  if (m_phy->IsStateOff ())
    {
      NS_LOG_DEBUG ("Cannot start TX because device is OFF");
      return;
    }
  m_currentPacket = Create<WifiPsdu> (mpdu, false);
  CancelAllEvents ();
  m_currentTxop = txop;
  m_txParams = params;
  m_currentTxVector = GetDmgTxVector (mpdu);
  SendDataPacket ();

  /* When this method completes, either we have taken ownership of the medium or the device switched off in the meantime. */
  NS_ASSERT (m_phy->IsStateTx () || m_phy->IsStateOff ());
}

void
MacLow::StartTransmission (Ptr<WifiMacQueueItem> mpdu,
                           MacLowTransmissionParameters params,
                           TransmissionOkCallback callback)
{
  NS_LOG_FUNCTION (this << *mpdu << params);
  if (m_phy->IsStateOff ())
    {
      NS_LOG_DEBUG ("Cannot start TX because device is OFF");
      return;
    }
  m_currentPacket = Create<WifiPsdu> (mpdu, false);
  CancelAllEvents ();
  m_currentTxop = 0;
  m_transmissionCallback = callback;
  m_txParams = params;
  m_currentTxVector = GetDmgTxVector (mpdu);
  SendDataPacket ();

  /* When this method completes, either we have taken ownership of the medium or the device switched off in the meantime. */
  NS_ASSERT (m_phy->IsStateTx () || m_phy->IsStateOff ());
}

void
MacLow::StartShortSswTransmission (Ptr<WifiMacQueueItem> mpdu,
                           MacLowTransmissionParameters params,
                           TransmissionShortSswOkCallback callback)
{
  NS_LOG_FUNCTION (this << *mpdu << params);
  if (m_phy->IsStateOff ())
    {
      NS_LOG_DEBUG ("Cannot start TX because device is OFF");
      return;
    }
  m_currentPacket = Create<WifiPsdu> (mpdu, false);
  CancelAllEvents ();
  m_currentTxop = 0;
  m_transmissionShortSswCallback = callback;
  m_txParams = params;
  m_currentTxVector = GetDmgControlTxVector ();
  StartDataTxTimers (m_currentTxVector);

  NS_ASSERT (m_currentPacket->GetNMpdus ());

  NS_LOG_DEBUG ("send Short SSW, size=" << m_currentPacket->GetSize () <<
                ", mode=" << m_currentTxVector.GetMode  () <<
                ", preamble=" << m_currentTxVector.GetPreambleType ());

  NS_LOG_DEBUG ("Sending non aggregate MPDU");
  m_phy->Send (m_currentPacket, m_currentTxVector);

  /* When this method completes, either we have taken ownership of the medium or the device switched off in the meantime. */
  NS_ASSERT (m_phy->IsStateTx () || m_phy->IsStateOff ());
}

void
MacLow::SLS_Phase_Started (void)
{
  NS_LOG_FUNCTION (this);
  m_servingSLS = true;
  /* We always prioritize SLS over any data transmission, so we cancel any events. */
  if (m_normalAckTimeoutEvent.IsRunning ())
    {
      m_normalAckTimeoutEvent.Cancel ();
    }
  if (m_blockAckTimeoutEvent.IsRunning ())
    {
      m_blockAckTimeoutEvent.Cancel ();
    }
  if (m_sendAckEvent.IsRunning ())
    {
      m_sendAckEvent.Cancel ();
    }
}

void
MacLow::SLS_Phase_Ended (void)
{
  NS_LOG_FUNCTION (this);
  m_servingSLS = false;
}

bool
MacLow::Is_Performing_SLS (void) const
{
  return m_servingSLS;
}

void
MacLow::MIMO_BFT_Phase_Started (void)
{
  NS_LOG_FUNCTION (this);
  m_servingMimoBFT = true;
  /* We always prioritize MIMO BFT over any data transmission, so we cancel any events. */
  if (m_normalAckTimeoutEvent.IsRunning ())
    {
      m_normalAckTimeoutEvent.Cancel ();
    }
  if (m_blockAckTimeoutEvent.IsRunning ())
    {
      m_blockAckTimeoutEvent.Cancel ();
    }
  if (m_sendAckEvent.IsRunning ())
    {
      m_sendAckEvent.Cancel ();
    }
}

void
MacLow::MIMO_BFT_Phase_Ended (void)
{
  NS_LOG_FUNCTION (this);
  m_servingMimoBFT = false;
}

bool
MacLow::Is_Performing_MIMO_BFT (void) const
{
  return m_servingMimoBFT;
}
//// WIGIG ////

bool
MacLow::NeedCtsToSelf (void) const
{
  WifiTxVector dataTxVector = GetDataTxVector (*m_currentPacket->begin ());
  return m_stationManager->NeedCtsToSelf (dataTxVector);
}

bool
MacLow::IsWithinSizeAndTimeLimits (Ptr<const WifiMacQueueItem> mpdu, WifiTxVector txVector,
                                    uint32_t ampduSize, Time ppduDurationLimit)
{
  NS_ASSERT (mpdu != 0 && mpdu->GetHeader ().IsQosData ());

  return IsWithinSizeAndTimeLimits (mpdu->GetSize (), mpdu->GetHeader ().GetAddr1 (),
                                    mpdu->GetHeader ().GetQosTid (), txVector,
                                    ampduSize, ppduDurationLimit);
}

bool
MacLow::IsWithinSizeAndTimeLimits (uint32_t mpduSize, Mac48Address receiver, uint8_t tid,
                                    WifiTxVector txVector, uint32_t ampduSize, Time ppduDurationLimit)
{
  NS_LOG_FUNCTION (this << mpduSize << receiver << +tid << txVector << ampduSize << ppduDurationLimit);

  if (ppduDurationLimit != Time::Min () && ppduDurationLimit.IsNegative ())
    {
      return false;
    }

  WifiModulationClass modulation = txVector.GetMode ().GetModulationClass ();

  uint32_t maxAmpduSize = 0;
  if (GetMpduAggregator ())
    {
      maxAmpduSize = GetMpduAggregator ()->GetMaxAmpduSize (receiver, tid, modulation);
    }

  // If maxAmpduSize is null, then ampduSize must be null as well
  NS_ASSERT (maxAmpduSize || ampduSize == 0);

  uint32_t ppduPayloadSize = mpduSize;

  // compute the correct size for A-MPDUs and S-MPDUs
  if (ampduSize > 0 || modulation >= WIFI_MOD_CLASS_VHT)
    {
      ppduPayloadSize = GetMpduAggregator ()->GetSizeIfAggregated (mpduSize, ampduSize);
    }

  if (maxAmpduSize > 0 && ppduPayloadSize > maxAmpduSize)
    {
      NS_LOG_DEBUG ("the frame does not meet the constraint on max A-MPDU size");
      return false;
    }

  // Get the maximum PPDU Duration based on the preamble type
  Time maxPpduDuration = GetPpduMaxTime (txVector.GetPreambleType ());

  Time txTime = m_phy->CalculateTxDuration (ppduPayloadSize, txVector, m_phy->GetFrequency ());

  if ((ppduDurationLimit.IsStrictlyPositive () && txTime > ppduDurationLimit)
      || (maxPpduDuration.IsStrictlyPositive () && txTime > maxPpduDuration))
    {
      NS_LOG_DEBUG ("the frame does not meet the constraint on max PPDU duration");
      return false;
    }

  return true;
}

void
MacLow::RxStartIndication (WifiTxVector txVector, Time psduDuration)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("PSDU reception started for " << psduDuration.ToDouble (Time::US)
                << " us (txVector: " << txVector << ")");
  NS_ASSERT (psduDuration.IsStrictlyPositive ());

  if (m_normalAckTimeoutEvent.IsRunning ())
    {
      // we are waiting for a Normal Ack and something arrived
      NS_LOG_DEBUG ("Rescheduling Normal Ack timeout");
      m_normalAckTimeoutEvent.Cancel ();
      NotifyAckTimeoutResetNow ();
      m_normalAckTimeoutEvent = Simulator::Schedule (psduDuration + NanoSeconds (PSDU_DURATION_SAFEGUARD),
                                                     &MacLow::NormalAckTimeout, this);
    }
  else if (m_blockAckTimeoutEvent.IsRunning ())
    {
      // we are waiting for a BlockAck and something arrived
      NS_LOG_DEBUG ("Rescheduling Block Ack timeout");
      m_blockAckTimeoutEvent.Cancel ();
      NotifyAckTimeoutResetNow ();
      m_blockAckTimeoutEvent = Simulator::Schedule (psduDuration + NanoSeconds (PSDU_DURATION_SAFEGUARD),
                                                    &MacLow::BlockAckTimeout, this);
    }
  else if (m_ctsTimeoutEvent.IsRunning ())
    {
      // we are waiting for a CTS and something arrived
      NS_LOG_DEBUG ("Rescheduling CTS timeout");
      m_ctsTimeoutEvent.Cancel ();
      NotifyCtsTimeoutResetNow ();
      m_ctsTimeoutEvent = Simulator::Schedule (psduDuration + NanoSeconds (PSDU_DURATION_SAFEGUARD),
                                               &MacLow::CtsTimeout, this);
    }
  else if (m_navCounterResetCtsMissed.IsRunning ())
    {
      NS_LOG_DEBUG ("Cannot reset NAV");
      m_navCounterResetCtsMissed.Cancel ();
    }
}

void
MacLow::ReceiveError (Ptr<WifiPsdu> psdu)
{
  NS_LOG_FUNCTION (this << *psdu);
  NS_LOG_DEBUG ("rx failed");
  if (IsCfPeriod () && m_currentPacket->GetHeader (0).IsCfPoll ())
    {
      NS_ASSERT (m_currentTxop != 0);
      m_currentTxop->MissedCfPollResponse (m_cfAckInfo.expectCfAck);
    }
  else if (m_cfAckInfo.expectCfAck)
    {
      NS_ASSERT (m_currentTxop != 0);
      Ptr<Txop> txop = m_currentTxop;
      m_currentTxop = 0;
      txop->MissedAck ();
    }
  m_cfAckInfo.expectCfAck = false;
  return;
}

void
MacLow::NotifySwitchingStartNow (Time duration)
{
  NS_LOG_DEBUG ("switching channel. Cancelling MAC pending events");
  m_stationManager->Reset ();
  CancelAllEvents ();
  if (m_navCounterResetCtsMissed.IsRunning ())
    {
      m_navCounterResetCtsMissed.Cancel ();
    }
  m_lastNavStart = Simulator::Now ();
  m_lastNavDuration = Seconds (0);
  m_currentPacket = 0;
  m_currentTxop = 0;
}

void
MacLow::NotifySleepNow (void)
{
  NS_LOG_DEBUG ("Device in sleep mode. Cancelling MAC pending events");
  CancelAllEvents ();
  if (m_navCounterResetCtsMissed.IsRunning ())
    {
      m_navCounterResetCtsMissed.Cancel ();
    }
  m_lastNavStart = Simulator::Now ();
  m_lastNavDuration = Seconds (0);
  m_currentPacket = 0;
  m_currentTxop = 0;
}

void
MacLow::NotifyOffNow (void)
{
  NS_LOG_DEBUG ("Device is switched off. Cancelling MAC pending events");
  CancelAllEvents ();
  if (m_navCounterResetCtsMissed.IsRunning ())
    {
      m_navCounterResetCtsMissed.Cancel ();
    }
  m_lastNavStart = Simulator::Now ();
  m_lastNavDuration = Seconds (0);
  m_currentPacket = 0;
  m_currentTxop = 0;
}

void
MacLow::ReceiveShortSswOk (Ptr<WifiMacQueueItem> mpdu, double rxSnr, WifiTxVector txVector, bool ampduSubframe)
{
  NS_LOG_FUNCTION (this << *mpdu << rxSnr << txVector);
  /* An MPDU containing a Short SSW packet is received from the PHY.
   */  
  Ptr<Packet> packet = mpdu->GetPacket ()->Copy ();

  /* To do: Set the NAV to account for the duration of the whole SISO period in MU-MIMO BFT */
//  bool isPrevNavZero = IsNavZero ();
//  NS_LOG_DEBUG ("duration/id=" << hdr.GetDuration ());
//  NotifyNav (packet, hdr);

  Ptr<DmgWifiMac> wifiMac = DynamicCast<DmgWifiMac> (m_mac);
  wifiMac->ReceiveShortSswFrame (packet, rxSnr);
  return;
}

void
MacLow::ReceiveOk (Ptr<WifiMacQueueItem> mpdu, double rxSnr, WifiTxVector txVector, bool ampduSubframe)
{
  NS_LOG_FUNCTION (this << *mpdu << rxSnr << txVector);
  /* An MPDU is received from the PHY.
   * When we have handled this MPDU,
   * we handle any packet present in the
   * packet queue.
   */
  const WifiMacHeader& hdr = mpdu->GetHeader ();
  Ptr<Packet> packet = mpdu->GetPacket ()->Copy ();

  bool isPrevNavZero = IsNavZero ();
  NS_LOG_DEBUG ("duration/id=" << hdr.GetDuration ());
  NotifyNav (packet, hdr);
  if (hdr.IsRts ())
    {
      /* see section 9.2.5.7 802.11-1999
       * A STA that is addressed by an RTS frame shall transmit a CTS frame after a SIFS
       * period if the NAV at the STA receiving the RTS frame indicates that the medium is
       * idle. If the NAV at the STA receiving the RTS indicates the medium is not idle,
       * that STA shall not respond to the RTS frame.
       */
      if (ampduSubframe)
        {
          NS_FATAL_ERROR ("Received RTS as part of an A-MPDU");
        }
      else
        {
          if (isPrevNavZero
              && hdr.GetAddr1 () == m_self)
            {
              NS_LOG_DEBUG ("rx RTS from=" << hdr.GetAddr2 () << ", schedule CTS");
              NS_ASSERT (m_sendCtsEvent.IsExpired ());
              m_stationManager->ReportRxOk (hdr.GetAddr2 (), &hdr,
                                            rxSnr, txVector.GetMode ());
              if ((m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ad) || (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay))
                {
                  m_sendCtsEvent = Simulator::Schedule (GetSifs (),
                                                        &MacLow::SendDmgCtsAfterRts, this,
                                                        hdr.GetAddr2 (),
                                                        hdr.GetDuration (),
                                                        txVector,
                                                        rxSnr);
                  if (txVector.IsControlTrailerPresent ())
                    {
                      ControlTrailer ct;
                      Ptr<Packet> packet = mpdu->GetPacket ()->Copy ();
                      packet->RemoveHeader (ct);
                      if (ct.IsMimoTransmission () && (!ct.IsMuMimoTransmission ()))
                        {
                          Ptr<DmgWifiMac> wifiMac = DynamicCast<DmgWifiMac> (m_mac);
                          wifiMac->UpdateBestMimoRxAntennaConfigurationIndex (hdr.GetAddr2 (), ct.GetTxSectorCombinationIdx ());
                        }
                    }
                }
              else
                {
                  m_sendCtsEvent = Simulator::Schedule (GetSifs (),
                                                        &MacLow::SendCtsAfterRts, this,
                                                        hdr.GetAddr2 (),
                                                        hdr.GetDuration (),
                                                        txVector,
                                                        rxSnr);
                }
            }
          else
            {
              NS_LOG_DEBUG ("rx RTS from=" << hdr.GetAddr2 () << ", cannot schedule CTS");
            }
        }
    }
  else if ((hdr.IsCts () || hdr.IsDmgCts ())
           && hdr.GetAddr1 () == m_self
           && m_ctsTimeoutEvent.IsRunning ()
           && m_currentPacket != 0)
    {
      if (ampduSubframe)
        {
          NS_FATAL_ERROR ("Received CTS as part of an A-MPDU");
        }

      NS_LOG_DEBUG ("received cts from=" << m_currentPacket->GetAddr1 ());

      SnrTag tag;
      packet->RemovePacketTag (tag);
      m_stationManager->ReportRxOk (m_currentPacket->GetAddr1 (), &hdr,
                                    rxSnr, txVector.GetMode ());
      m_stationManager->ReportRtsOk (m_currentPacket->GetAddr1 (), &m_currentPacket->GetHeader (0),
                                     rxSnr, txVector.GetMode (), tag.Get ());

      m_ctsTimeoutEvent.Cancel ();
      NotifyCtsTimeoutResetNow ();
      NS_ASSERT (m_sendDataEvent.IsExpired ());
      m_sendDataEvent = Simulator::Schedule (GetSifs (),
                                             &MacLow::SendDataAfterCts, this,
                                             hdr.GetDuration ());
      if (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay)
        {
          if (txVector.IsControlTrailerPresent ())
            {
              ControlTrailer ct;
              Ptr<Packet> packet = mpdu->GetPacket ()-> Copy ();
              packet->RemoveHeader (ct);
              if (ct.IsMimoTransmission () && (!ct.IsMuMimoTransmission ()))
                {
                  Ptr<DmgWifiMac> wifiMac = DynamicCast<DmgWifiMac> (m_mac);
                  wifiMac->UpdateBestMimoRxAntennaConfigurationIndex (hdr.GetAddr2 (), ct.GetTxSectorCombinationIdx ());
                  wifiMac->SteerMimoRxAntennaToward (hdr.GetAddr2 ());
                }
            }
        }
    }
  else if (hdr.IsAck ()
           && hdr.GetAddr1 () == m_self
           && m_normalAckTimeoutEvent.IsRunning ()
           && m_txParams.MustWaitNormalAck ())
    {
      NS_LOG_DEBUG ("receive ack from=" << m_currentPacket->GetAddr1 ());
      SnrTag tag;
      packet->RemovePacketTag (tag);
      //When fragmentation is used, only update manager when the last fragment is acknowledged
      if (!m_txParams.HasNextPacket ())
        {
          m_stationManager->ReportRxOk (m_currentPacket->GetAddr1 (), &hdr,
                                        rxSnr, txVector.GetMode ());
          m_stationManager->ReportDataOk (m_currentPacket->GetAddr1 (), &m_currentPacket->GetHeader (0),
                                          rxSnr, txVector.GetMode (), tag.Get (),
                                          m_currentTxVector, m_currentPacket->GetSize ());
        }
      // cancel the Normal Ack timer
      m_normalAckTimeoutEvent.Cancel ();
      NotifyAckTimeoutResetNow ();
      m_currentTxop->GotAck ();

      if (m_txParams.HasNextPacket ())
        {
          if (m_stationManager->GetRifsPermitted ())
            {
              m_waitIfsEvent = Simulator::Schedule (GetRifs (), &MacLow::WaitIfsAfterEndTxFragment, this);
            }
          else
            {
              m_waitIfsEvent = Simulator::Schedule (GetSifs (), &MacLow::WaitIfsAfterEndTxFragment, this);
            }
        }
      else if (m_currentPacket->GetHeader (0).IsQosData () && m_currentTxop->IsQosTxop () &&
               m_currentTxop->GetTxopLimit ().IsStrictlyPositive ()
               //// WIGIG ////
               //&& m_currentTxop->GetTxopRemaining () > GetSifs ()
               && m_currentTxop->GetRemainingTimeForTransmission () > GetSifs ())
               //// WIGIG ////
        {
          if (m_stationManager->GetRifsPermitted ())
            {
              m_waitIfsEvent = Simulator::Schedule (GetRifs (), &MacLow::WaitIfsAfterEndTxPacket, this);
            }
          else
            {
              m_waitIfsEvent = Simulator::Schedule (GetSifs (), &MacLow::WaitIfsAfterEndTxPacket, this);
            }
        }
      else if (m_currentTxop->IsQosTxop ())
        {
          m_currentTxop->TerminateTxop ();
        }
      /* WIGIG: Set the current packet to zero to avoid storing it for the next access period */
      m_currentPacket = 0; //// WIGIG ////
    }
  else if (hdr.IsBlockAck () && hdr.GetAddr1 () == m_self
           && m_txParams.MustWaitBlockAck ()
           && m_blockAckTimeoutEvent.IsRunning ())
    {
      NS_LOG_DEBUG ("got block ack from " << hdr.GetAddr2 ());
      SnrTag tag;
      packet->RemovePacketTag (tag);
      CtrlBAckResponseHeader blockAck;
      packet->RemoveHeader (blockAck);
      m_blockAckTimeoutEvent.Cancel ();
      NotifyAckTimeoutResetNow ();
      m_currentTxop->GotBlockAck (&blockAck, hdr.GetAddr2 (), rxSnr, tag.Get (), m_currentTxVector);
      // start next packet if TXOP remains, otherwise contend for accessing the channel again
      if (m_currentTxop->IsQosTxop () && m_currentTxop->GetTxopLimit ().IsStrictlyPositive ()
          //// WIGIG ////
          //&& m_currentTxop->GetTxopRemaining () > GetSifs ()
          && m_currentTxop->GetRemainingTimeForTransmission () > GetSifs ())
          //// WIGIG ////
        {
          if (m_stationManager->GetRifsPermitted ())
            {
              m_waitIfsEvent = Simulator::Schedule (GetRifs (), &MacLow::WaitIfsAfterEndTxPacket, this);
            }
          else
            {
              m_waitIfsEvent = Simulator::Schedule (GetSifs (), &MacLow::WaitIfsAfterEndTxPacket, this);
            }
        }
      else if (m_currentTxop->IsQosTxop ())
        {
          m_currentTxop->TerminateTxop ();
        }
      /* WIGIG: Set the current packet to zero to avoid storing it for the next access period */
      m_currentPacket = 0; //// WIGIG ////
    }
  else if (hdr.IsBlockAckReq () && hdr.GetAddr1 () == m_self)
    {
      if (m_servingSLS)
        {
          NS_LOG_DEBUG ("We are serving SLS, so ignore BlockAckRequest frame");
          return;
        }
      if (m_servingMimoBFT)
        {
          NS_LOG_DEBUG ("We are serving MIMO BFT, so ignore BlockAckRequest frame");
          return;
        }
      CtrlBAckRequestHeader blockAckReq;
      packet->RemoveHeader (blockAckReq);
      if (!blockAckReq.IsMultiTid ())
        {
          uint8_t tid = blockAckReq.GetTidInfo ();
          AgreementsI it = m_bAckAgreements.find (std::make_pair (hdr.GetAddr2 (), tid));
          if (it != m_bAckAgreements.end ())
            {
              //Update block ack cache
              BlockAckCachesI i = m_bAckCaches.find (std::make_pair (hdr.GetAddr2 (), tid));
              NS_ASSERT (i != m_bAckCaches.end ());
              (*i).second.UpdateWithBlockAckReq (blockAckReq.GetStartingSequence ());

              NS_ASSERT (m_sendAckEvent.IsExpired ());
              m_sendAckEvent.Cancel ();
              /* See section 11.5.3 in IEEE 802.11 for mean of this timer */
              ResetBlockAckInactivityTimerIfNeeded (it->second.first);
              if ((*it).second.first.IsImmediateBlockAck ())
                {
                  NS_LOG_DEBUG ("rx blockAckRequest/sendImmediateBlockAck from=" << hdr.GetAddr2 ());
                  m_sendAckEvent = Simulator::Schedule (GetSifs (),
                                                        &MacLow::SendBlockAckAfterBlockAckRequest, this,
                                                        blockAckReq,
                                                        hdr.GetAddr2 (),
                                                        hdr.GetDuration (),
                                                        txVector.GetMode (),
                                                        rxSnr);
                }
              else
                {
                  NS_FATAL_ERROR ("Delayed block ack not supported.");
                }
            }
          else
            {
              NS_LOG_DEBUG ("There's not a valid agreement for this block ack request.");
            }
        }
      else
        {
          NS_FATAL_ERROR ("Multi-tid block ack is not supported.");
        }
    }
  //// WIGIG ////
  else if (hdr.IsDMGBeacon ())
    {
      NS_LOG_DEBUG ("Received DMG Beacon with BSSID=" << hdr.GetAddr1 ());
      m_stationManager->ReportRxOk (hdr.GetAddr1 (), &hdr, rxSnr, txVector.GetMode ());
      goto rxPacket;
    }
  else if ((hdr.GetAddr1 () == m_self) && (hdr.IsSSW () || hdr.IsSSW_FBCK () || hdr.IsSSW_ACK ()))
    {
      NS_LOG_DEBUG ("Received " << hdr.GetTypeString ());
      m_stationManager->ReportRxOk (hdr.GetAddr2 (), &hdr, rxSnr, txVector.GetMode ());
      goto rxPacket;
    }
  //// WIGIG ////
  else if (hdr.IsCtl ())
    {
      if (hdr.IsCfEnd ())
        {
          NS_LOG_DEBUG ("rx CF-END ");
          m_cfpStart = NanoSeconds (0);
          if (m_cfAckInfo.expectCfAck)
            {
              NS_ASSERT (m_currentTxop != 0);
              if (hdr.IsCfAck ())
                {
                  m_currentTxop->GotAck ();
                }
              else
                {
                  m_currentTxop->MissedAck ();
                }
            }
          if (m_currentTxop != 0)
            {
              m_currentTxop->GotCfEnd ();
            }
          m_cfAckInfo.expectCfAck = false;
        }
      else
        {
          NS_LOG_DEBUG ("rx drop " << hdr.GetTypeString ());
        }
    }
  else if (hdr.GetAddr1 () == m_self)
    {
      if (hdr.IsCfPoll ())
        {
          m_cfpStart = Simulator::Now ();
          if (m_cfAckInfo.expectCfAck && !hdr.IsCfAck ())
            {
              NS_ASSERT (m_currentTxop != 0);
              Ptr<Txop> txop = m_currentTxop;
              m_currentTxop = 0;
              txop->MissedAck ();
              m_cfAckInfo.expectCfAck = false;
            }
        }
      if (m_servingSLS)
        {
          NS_LOG_DEBUG ("We are serving SLS, so ignore any data or management frame");
          if (m_sendAckEvent.IsRunning ())
            {
              m_sendAckEvent.Cancel ();
            }
          return;
        }
      if (m_servingMimoBFT && (hdr.IsData () || hdr.IsBlockAck ()))
        {
          NS_LOG_DEBUG ("We are serving MIMO BFT, so ignore any data or management frame that's not part of it");
          if (m_sendAckEvent.IsRunning ())
            {
              m_sendAckEvent.Cancel ();
            }
          return;
        }
      m_stationManager->ReportRxOk (hdr.GetAddr2 (), &hdr,
                                    rxSnr, txVector.GetMode ());
      if (hdr.IsActionNoAck ())
        {
          NS_LOG_DEBUG ("Received Action No ACK Frame");
          goto rxPacket;
        }
      else if (hdr.IsQosData () && ReceiveMpdu (mpdu))
        {
          /* From section 9.10.4 in IEEE 802.11:
             Upon the receipt of a QoS data frame from the originator for which
             the block ack agreement exists, the recipient shall buffer the MSDU
             regardless of the value of the Ack Policy subfield within the
             QoS Control field of the QoS data frame. */
          if (hdr.IsQosAck () && !ampduSubframe)
            {
              NS_LOG_DEBUG ("rx QoS unicast/sendAck from=" << hdr.GetAddr2 ());
              AgreementsI it = m_bAckAgreements.find (std::make_pair (hdr.GetAddr2 (), hdr.GetQosTid ()));

              RxCompleteBufferedPacketsWithSmallerSequence (it->second.first.GetStartingSequenceControl (),
                                                            hdr.GetAddr2 (), hdr.GetQosTid ());
              RxCompleteBufferedPacketsUntilFirstLost (hdr.GetAddr2 (), hdr.GetQosTid ());
              NS_ASSERT (m_sendAckEvent.IsExpired ()); //// WIGIG ////
//// WIGIG ////
             // if (m_sendAckEvent.IsRunning ())
             //   {
             //    m_sendAckEvent.Cancel ();
             //  }
//// WIGIG ////
              m_sendAckEvent = Simulator::Schedule (GetSifs (),
                                                    &MacLow::SendAckAfterData, this,
                                                    hdr.GetAddr2 (),
                                                    hdr.GetDuration (),
                                                    txVector.GetMode (),
                                                    rxSnr);
            }
          else if (hdr.IsQosBlockAck ())
            {
              AgreementsI it = m_bAckAgreements.find (std::make_pair (hdr.GetAddr2 (), hdr.GetQosTid ()));
              /* See section 11.5.3 in IEEE 802.11 for mean of this timer */
              ResetBlockAckInactivityTimerIfNeeded (it->second.first);
            }
          return;
        }
      else if (hdr.IsQosData () && hdr.IsQosBlockAck ())
        {
          /* This happens if a packet with ack policy Block Ack is received and a block ack
             agreement for that packet doesn't exist.

             From section 11.5.3 in IEEE 802.11e:
             When a recipient does not have an active block ack for a TID, but receives
             data MPDUs with the Ack Policy subfield set to Block Ack, it shall discard
             them and shall send a DELBA frame using the normal access
             mechanisms. */
          AcIndex ac = QosUtilsMapTidToAc (hdr.GetQosTid ());
          m_edca[ac]->SendDelbaFrame (hdr.GetAddr2 (), hdr.GetQosTid (), false);
          return;
        }
      else if (hdr.IsQosData () && hdr.IsQosNoAck ())
        {
          if (ampduSubframe)
            {
              NS_LOG_DEBUG ("rx Ampdu with No Ack Policy from=" << hdr.GetAddr2 ());
            }
          else
            {
              NS_LOG_DEBUG ("rx unicast/noAck from=" << hdr.GetAddr2 ());
            }
        }
      else if (hdr.IsData () || hdr.IsMgt ())
        {
          if (hdr.IsProbeResp ())
            {
              // Apply SNR tag for probe response quality measurements
              SnrTag tag;
              tag.Set (rxSnr);
              packet->AddPacketTag (tag);
              mpdu = Create<WifiMacQueueItem> (packet, hdr);
            }
          if (hdr.IsMgt () && ampduSubframe)
            {
              NS_FATAL_ERROR ("Received management packet as part of an A-MPDU");
            }
          else
            {
              if (IsCfPeriod ())
                {
                  if (hdr.HasData ())
                    {
                      m_cfAckInfo.appendCfAck = true;
                      m_cfAckInfo.address = hdr.GetAddr2 ();
                    }
                }
              else
                {
                  NS_LOG_DEBUG ("rx unicast/sendAck from=" << hdr.GetAddr2 ());
                  NS_ASSERT (m_sendAckEvent.IsExpired ());
                  m_sendAckEvent = Simulator::Schedule (GetSifs (),
                                                        &MacLow::SendAckAfterData, this,
                                                        hdr.GetAddr2 (),
                                                        hdr.GetDuration (),
                                                        txVector.GetMode (),
                                                        rxSnr);
                }
              //else  //// WIGIG ////
              //  {
              //    return;
              //  }
            }
        }
      goto rxPacket;
    }
  else if (hdr.GetAddr1 ().IsGroup ())
    {
      if (ampduSubframe)
        {
          NS_FATAL_ERROR ("Received group addressed packet as part of an A-MPDU");
        }
      else
        {
          if (hdr.IsData () || hdr.IsMgt ())
            {
              NS_LOG_DEBUG ("rx group from=" << hdr.GetAddr2 ());
              if (hdr.IsBeacon ())
                {
                  // Apply SNR tag for beacon quality measurements
                  SnrTag tag;
                  tag.Set (rxSnr);
                  packet->AddPacketTag (tag);
                  mpdu = Create<WifiMacQueueItem> (packet, hdr);
                }
              goto rxPacket;
            }
        }
    }
  else if (m_promisc)
    {
      NS_ASSERT (hdr.GetAddr1 () != m_self);
      if (hdr.IsData ())
        {
          goto rxPacket;
        }
    }
  else
    {
      if (m_cfAckInfo.expectCfAck && hdr.IsCfAck ())
        {
          m_cfAckInfo.expectCfAck = false;
          NS_ASSERT (m_currentTxop != 0);
          m_currentTxop->GotAck ();
        }
      else if (m_servingMimoBFT && hdr.GetAddr1 () == hdr.GetAddr2 ())
        {
          NS_LOG_INFO ("During MIMO BF Training phase of MU-MIMO BFT the Initiator sets the TA and RA fields to his own address");
          goto rxPacket;
        }
      NS_LOG_DEBUG ("rx not for me from=" << hdr.GetAddr2 ());
    }
  return;
rxPacket:
  if (m_cfAckInfo.expectCfAck && hdr.IsCfAck ())
    {
      m_cfAckInfo.expectCfAck = false;
      NS_ASSERT (m_currentTxop != 0);
      m_currentTxop->GotAck ();
    }
  m_rxCallback (mpdu);
  return;
}

uint32_t
MacLow::GetCfEndSize (void) const
{
  WifiMacHeader cfEnd;
  if (m_cfAckInfo.expectCfAck || m_cfAckInfo.appendCfAck)
    {
      cfEnd.SetType (WIFI_MAC_CTL_END_ACK);
    }
  else
    {
      cfEnd.SetType (WIFI_MAC_CTL_END);
    }
  return cfEnd.GetSize () + 4;
}

Time
MacLow::GetAckDuration (Mac48Address to, WifiTxVector dataTxVector) const
{
  WifiTxVector ackTxVector = GetAckTxVectorForData (to, dataTxVector.GetMode ());
  return GetAckDuration (ackTxVector);
}

Time
MacLow::GetAckDuration (WifiTxVector ackTxVector) const
{
  NS_ASSERT (ackTxVector.GetMode ().GetModulationClass () != WIFI_MOD_CLASS_HT); //Ack should always use non-HT PPDU (HT PPDU cases not supported yet)
  return m_phy->CalculateTxDuration (GetAckSize (), ackTxVector, m_phy->GetFrequency ());
}

Time
MacLow::GetBlockAckDuration (WifiTxVector blockAckReqTxVector, BlockAckType type) const
{
  /*
   * For immediate Basic BlockAck we should transmit the frame with the same WifiMode
   * as the BlockAckReq.
   */
  return m_phy->CalculateTxDuration (GetBlockAckSize (type), blockAckReqTxVector, m_phy->GetFrequency ());
}

Time
MacLow::GetBlockAckRequestDuration (WifiTxVector blockAckReqTxVector, BlockAckType type) const
{
  return m_phy->CalculateTxDuration (GetBlockAckRequestSize (type), blockAckReqTxVector, m_phy->GetFrequency ());
}

Time
MacLow::GetCtsDuration (Mac48Address to, WifiTxVector rtsTxVector, bool addControlTrailer) const
{
  //// WIGIG ////
  if ((rtsTxVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_DMG_CTRL || rtsTxVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_EDMG_CTRL))
    {
      return GetDmgCtsDuration (addControlTrailer);
    }
  else
    //// WIGIG ////
    {
      WifiTxVector ctsTxVector = GetCtsTxVectorForRts (to, rtsTxVector.GetMode ());
      return GetCtsDuration (ctsTxVector);
    }
}

Time
MacLow::GetCtsDuration (WifiTxVector ctsTxVector) const
{
  NS_ASSERT (ctsTxVector.GetMode ().GetModulationClass () != WIFI_MOD_CLASS_HT); //CTS should always use non-HT PPDU (HT PPDU cases not supported yet)
  return m_phy->CalculateTxDuration (GetCtsSize (), ctsTxVector, m_phy->GetFrequency ());
}

//// WIGIG ////
Time
MacLow::GetDmgControlDuration (WifiTxVector txVector, uint32_t payloadSize) const
{
  NS_ASSERT (txVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_DMG_CTRL || txVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_EDMG_CTRL);
  return m_phy->CalculateTxDuration (payloadSize, txVector, m_phy->GetFrequency ());
}

Time
MacLow::GetDmgCtsDuration (bool addControlTrailer) const
{
  WifiTxVector ctsTxVector = GetDmgControlTxVector ();
  NS_ASSERT (ctsTxVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_DMG_CTRL || ctsTxVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_EDMG_CTRL);
  return m_phy->CalculateTxDuration (GetDmgCtsSize (addControlTrailer), ctsTxVector, m_phy->GetFrequency ());
}

uint32_t
MacLow::GetDmgCtsSize (bool addControlTrailer)
{
  WifiMacHeader cts;
  cts.SetType (WIFI_MAC_CTL_DMG_CTS);
  uint32_t dmgCtsSize = cts.GetSize () + 4;
  if (addControlTrailer)
    dmgCtsSize+= 18;
  return dmgCtsSize;
}

WifiTxVector
MacLow::GetDmgTxVector (Ptr<const WifiMacQueueItem> item) const
{
  Mac48Address to = item->GetHeader ().GetAddr1 ();
  return m_stationManager->GetDmgTxVector (to, &item->GetHeader (), item->GetPacket ());
}
//// WIGIG ////

WifiTxVector
MacLow::GetRtsTxVector (Ptr<const WifiMacQueueItem> item) const
{
  return m_stationManager->GetRtsTxVector (item->GetHeader ().GetAddr1 ());
}

WifiTxVector
MacLow::GetDataTxVector (Ptr<const WifiMacQueueItem> item) const
{
  return m_stationManager->GetDataTxVector (item->GetHeader ());
}

Time
MacLow::GetResponseDuration (const MacLowTransmissionParameters& params, WifiTxVector dataTxVector,
                             Mac48Address receiver) const
{
  NS_LOG_FUNCTION (this << receiver << dataTxVector << params);

  Time duration = Seconds (0);
  if (params.MustWaitNormalAck ())
    {
      duration += GetSifs ();
      duration += GetAckDuration (receiver, dataTxVector);
    }
  else if (params.MustWaitBlockAck ())
    {
      duration += GetSifs ();
      WifiTxVector blockAckReqTxVector = GetBlockAckTxVector (m_self, dataTxVector.GetMode ());
      duration += GetBlockAckDuration (blockAckReqTxVector, params.GetBlockAckType ());
    }
  else if (params.MustSendBlockAckRequest ())
    {
      duration += 2 * GetSifs ();
      WifiTxVector blockAckReqTxVector = GetBlockAckTxVector (m_self, dataTxVector.GetMode ());
      duration += GetBlockAckRequestDuration (blockAckReqTxVector, params.GetBlockAckRequestType ());
      duration += GetBlockAckDuration (blockAckReqTxVector, params.GetBlockAckRequestType ());
    }
  return duration;
}

WifiMode
MacLow::GetControlAnswerMode (WifiMode reqMode) const
{
  NS_LOG_FUNCTION (this << reqMode);
  WifiMode mode = m_stationManager->GetDefaultMode ();
  bool found = false;
  if (m_stationManager->HasDmgSupported () || m_stationManager->HasEdmgSupported ())   //// WIGIG ////
    {
      /**
       * Rules for selecting a control response rate from IEEE 802.11ad-2012,
       * Section 9.7.5a Multirate support for DMG STAs:
       */
      WifiMode thismode;
      /* We start from SC PHY Rates, This is for transmitting an ACK frame or a BA frame */
      for (uint32_t idx = 0; idx < m_phy->GetNModes (); idx++)
        {
          thismode = m_phy->GetMode (idx);
          if (thismode.IsMandatory () && (thismode.GetDataRate () <= reqMode.GetDataRate ()))
            {
              mode = thismode;
              found = true;
            }
          else
            {
              break;
            }
        }
    }
  else
    {
      /**
       * The standard has relatively unambiguous rules for selecting a
       * control response rate (the below is quoted from IEEE 802.11-2012,
       * Section 9.7):
       *
       * To allow the transmitting STA to calculate the contents of the
       * Duration/ID field, a STA responding to a received frame shall
       * transmit its Control Response frame (either CTS or Ack), other
       * than the BlockAck control frame, at the highest rate in the
       * BSSBasicRateSet parameter that is less than or equal to the
       * rate of the immediately previous frame in the frame exchange
       * sequence (as defined in Annex G) and that is of the same
       * modulation class (see Section 9.7.8) as the received frame...
       */
      //First, search the BSS Basic Rate set
      for (uint8_t i = 0; i < m_stationManager->GetNBasicModes (); i++)
        {
          WifiMode testMode = m_stationManager->GetBasicMode (i);
          if ((!found || testMode.IsHigherDataRate (mode))
              && (!testMode.IsHigherDataRate (reqMode))
              && (IsAllowedControlAnswerModulationClass (reqMode.GetModulationClass (), testMode.GetModulationClass ())))
            {
              mode = testMode;
              //We've found a potentially-suitable transmit rate, but we
              //need to continue and consider all the basic rates before
              //we can be sure we've got the right one.
              found = true;
            }
        }
      if (m_stationManager->GetHtSupported ())
        {
          if (!found)
            {
              mode = m_stationManager->GetDefaultMcs ();
              for (uint8_t i = 0; i != m_stationManager->GetNBasicMcs (); i++)
                {
                  WifiMode testMode = m_stationManager->GetBasicMcs (i);
                  if ((!found || testMode.IsHigherDataRate (mode))
                      && (!testMode.IsHigherDataRate (reqMode))
                      && (testMode.GetModulationClass () == reqMode.GetModulationClass ()))
                    {
                      mode = testMode;
                      //We've found a potentially-suitable transmit rate, but we
                      //need to continue and consider all the basic rates before
                      //we can be sure we've got the right one.
                      found = true;
                    }
                }
            }
        }
      //If we found a suitable rate in the BSSBasicRateSet, then we are
      //done and can return that mode.
      if (found)
        {
          NS_LOG_DEBUG ("MacLow::GetControlAnswerMode returning " << mode);
          return mode;
        }

      /**
       * If no suitable basic rate was found, we search the mandatory
       * rates. The standard (IEEE 802.11-2007, Section 9.6) says:
       *
       *   ...If no rate contained in the BSSBasicRateSet parameter meets
       *   these conditions, then the control frame sent in response to a
       *   received frame shall be transmitted at the highest mandatory
       *   rate of the PHY that is less than or equal to the rate of the
       *   received frame, and that is of the same modulation class as the
       *   received frame. In addition, the Control Response frame shall
       *   be sent using the same PHY options as the received frame,
       *   unless they conflict with the requirement to use the
       *   BSSBasicRateSet parameter.
       *
       * \todo Note that we're ignoring the last sentence for now, because
       * there is not yet any manipulation here of PHY options.
       */
      for (uint8_t idx = 0; idx < m_phy->GetNModes (); idx++)
        {
          WifiMode thismode = m_phy->GetMode (idx);
          /* If the rate:
           *
           *  - is a mandatory rate for the PHY, and
           *  - is equal to or faster than our current best choice, and
           *  - is less than or equal to the rate of the received frame, and
           *  - is of the same modulation class as the received frame
           *
           * ...then it's our best choice so far.
           */
          if (thismode.IsMandatory ()
              && (!found || thismode.IsHigherDataRate (mode))
              && (!thismode.IsHigherDataRate (reqMode))
              && (IsAllowedControlAnswerModulationClass (reqMode.GetModulationClass (), thismode.GetModulationClass ())))
            {
              mode = thismode;
              //As above; we've found a potentially-suitable transmit
              //rate, but we need to continue and consider all the
              //mandatory rates before we can be sure we've got the right one.
              found = true;
            }
        }
      if (m_stationManager->GetHtSupported () )
        {
          for (uint8_t idx = 0; idx < m_phy->GetNMcs (); idx++)
            {
              WifiMode thismode = m_phy->GetMcs (idx);
              if (thismode.IsMandatory ()
                  && (!found || thismode.IsHigherDataRate (mode))
                  && (!thismode.IsHigherCodeRate (reqMode))
                  && (thismode.GetModulationClass () == reqMode.GetModulationClass ()))
                {
                  mode = thismode;
                  //As above; we've found a potentially-suitable transmit
                  //rate, but we need to continue and consider all the
                  //mandatory rates before we can be sure we've got the right one.
                  found = true;
                }
            }
        }
    }

  /**
   * If we still haven't found a suitable rate for the response then
   * someone has messed up the simulation configuration. This probably means
   * that the WifiPhyStandard is not set correctly, or that a rate that
   * is not supported by the PHY has been explicitly requested.
   *
   * Either way, it is serious - we can either disobey the standard or
   * fail, and I have chosen to do the latter...
   */
  if (!found)
    {
      NS_FATAL_ERROR ("Can't find response rate for " << reqMode);
    }

  NS_LOG_DEBUG ("MacLow::GetControlAnswerMode returning " << mode);
  return mode;
}

WifiTxVector
MacLow::GetCtsTxVector (Mac48Address to, WifiMode rtsTxMode) const
{
  NS_ASSERT (!to.IsGroup ());
  WifiMode ctsMode = GetControlAnswerMode (rtsTxMode);
  WifiTxVector v;
  v.SetMode (ctsMode);
  v.SetPreambleType (GetPreambleForTransmission (ctsMode.GetModulationClass (), m_stationManager->GetShortPreambleEnabled (), m_stationManager->UseGreenfieldForDestination (to)));
  v.SetTxPowerLevel (m_stationManager->GetDefaultTxPowerLevel ());
  v.SetChannelWidth (GetChannelWidthForTransmission (ctsMode, m_phy->GetChannelWidth ()));
   uint16_t ctsTxGuardInterval = ConvertGuardIntervalToNanoSeconds (ctsMode, m_phy->GetShortGuardInterval (), m_phy->GetGuardInterval ());
  v.SetGuardInterval (ctsTxGuardInterval);
  v.SetNss (1);
  if (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay)
    {
      v.SetChBandwidth (StaticCast<DmgWifiPhy> (m_phy)->GetChannelConfiguration ());
    }
  return v;
}

WifiTxVector
MacLow::GetAckTxVector (Mac48Address to, WifiMode dataTxMode) const
{
  NS_ASSERT (!to.IsGroup ());
  WifiMode ackMode = GetControlAnswerMode (dataTxMode);
  WifiTxVector v;
  v.SetMode (ackMode);
  v.SetPreambleType (GetPreambleForTransmission (ackMode.GetModulationClass (), m_stationManager->GetShortPreambleEnabled (), m_stationManager->UseGreenfieldForDestination (to)));
  v.SetTxPowerLevel (m_stationManager->GetDefaultTxPowerLevel ());
  v.SetChannelWidth (GetChannelWidthForTransmission (ackMode, m_phy->GetChannelWidth ()));
   uint16_t ackTxGuardInterval = ConvertGuardIntervalToNanoSeconds (ackMode, m_phy->GetShortGuardInterval (), m_phy->GetGuardInterval ());
  v.SetGuardInterval (ackTxGuardInterval);
  v.SetNss (1);
  //// WIGIG ////
  if (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay)
    {
      v.SetChBandwidth (StaticCast<DmgWifiPhy> (m_phy)->GetChannelConfiguration ());
    }
  //// WIGIG ////
  return v;
}

//// WIGIG ////
WifiTxVector
MacLow::GetDmgControlTxVector (void) const
{
  return m_stationManager->GetDmgControlTxVector ();
}
//// WIGIG ////

WifiTxVector
MacLow::GetBlockAckTxVector (Mac48Address to, WifiMode dataTxMode) const
{
  NS_ASSERT (!to.IsGroup ());
  WifiMode blockAckMode = GetControlAnswerMode (dataTxMode);
  WifiTxVector v;
  v.SetMode (blockAckMode);
  v.SetPreambleType (GetPreambleForTransmission (blockAckMode.GetModulationClass (), m_stationManager->GetShortPreambleEnabled (), m_stationManager->UseGreenfieldForDestination (to)));
  v.SetTxPowerLevel (m_stationManager->GetDefaultTxPowerLevel ());
  v.SetChannelWidth (GetChannelWidthForTransmission (blockAckMode, m_phy->GetChannelWidth ()));
  uint16_t blockAckTxGuardInterval = ConvertGuardIntervalToNanoSeconds (blockAckMode, m_phy->GetShortGuardInterval (), m_phy->GetGuardInterval ());
  v.SetGuardInterval (blockAckTxGuardInterval);
  v.SetNss (1);
  //// WIGIG ////
  if (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay)
    {
      v.SetChBandwidth (StaticCast<DmgWifiPhy> (m_phy)->GetChannelConfiguration ());
    }
  //// WIGIG ////
  return v;
}

WifiTxVector
MacLow::GetCtsTxVectorForRts (Mac48Address to, WifiMode rtsTxMode) const
{
  return GetCtsTxVector (to, rtsTxMode);
}

WifiTxVector
MacLow::GetAckTxVectorForData (Mac48Address to, WifiMode dataTxMode) const
{
  return GetAckTxVector (to, dataTxMode);
}

Time
MacLow::CalculateOverallTxTime (Ptr<const Packet> packet,
                                const WifiMacHeader* hdr,
                                const MacLowTransmissionParameters& params,
                                uint32_t fragmentSize) const
{
  Ptr<const WifiMacQueueItem> item = Create<const WifiMacQueueItem> (packet, *hdr);
  Time txTime = CalculateOverheadTxTime (item, params);
  uint32_t dataSize;
  if (fragmentSize > 0)
    {
      Ptr<const Packet> fragment = Create<Packet> (fragmentSize);
      dataSize = GetSize (fragment, hdr, m_currentPacket && m_currentPacket->IsAggregate ());
    }
  else
    {
      dataSize = GetSize (packet, hdr, m_currentPacket && m_currentPacket->IsAggregate ());
    }
  txTime += m_phy->CalculateTxDuration (dataSize, GetDataTxVector (item), m_phy->GetFrequency ());
  return txTime;
}

Time
MacLow::CalculateOverheadTxTime (Ptr<const WifiMacQueueItem> item,
                                 const MacLowTransmissionParameters& params) const
{
  Time txTime = Seconds (0);
  if (params.MustSendRts ())
    {
      WifiTxVector rtsTxVector = GetRtsTxVector (item);
      txTime += m_phy->CalculateTxDuration (GetRtsSize (), rtsTxVector, m_phy->GetFrequency ());
      txTime += GetCtsDuration (item->GetHeader ().GetAddr1 (), rtsTxVector);
      txTime += Time (GetSifs () * 2);
    }
  txTime += GetResponseDuration (params, GetDataTxVector (item), item->GetHeader ().GetAddr1 ());

  return txTime;
}

Time
MacLow::CalculateTransmissionTime (Ptr<const Packet> packet,
                                   const WifiMacHeader* hdr,
                                   const MacLowTransmissionParameters& params) const
{
  Time txTime = CalculateOverallTxTime (packet, hdr, params);
  if (params.HasNextPacket ())
    {
      WifiTxVector dataTxVector = GetDataTxVector (Create<const WifiMacQueueItem> (packet, *hdr));
      txTime += GetSifs ();
      txTime += m_phy->CalculateTxDuration (params.GetNextPacketSize (), dataTxVector, m_phy->GetFrequency ());
    }
  return txTime;
}

//// WIGIG ////
Time
MacLow::CalculateWiGigTransactionTime (Ptr<WifiPsdu> psdu)
{
  NS_LOG_FUNCTION (this << psdu);
  Time txTime = m_phy->CalculateTxDuration (psdu->GetSize (), m_currentTxVector, m_phy->GetFrequency ());
  /* Calculate overhead duration */
  if (m_txParams.MustSendRts ())
    {
      WifiTxVector rtsTxVector = GetDmgControlTxVector ();
      txTime += m_phy->CalculateTxDuration (GetRtsSize (), rtsTxVector, m_phy->GetFrequency ());
      txTime += GetCtsDuration (psdu->GetAddr1 (), rtsTxVector);
      txTime += Time (GetSifs () * 2);
    }
  txTime += GetResponseDuration (m_txParams, m_currentTxVector, psdu->GetAddr1 ());
  /* Convert to MicroSeconds since the duration in the headers are in MicroSeconds */
  return MicroSeconds (ceil ((double) txTime.GetNanoSeconds () / 1000));
  return Seconds (0);
}
//// WIGIG ////

void
MacLow::NotifyNav (Ptr<const Packet> packet, const WifiMacHeader &hdr)
{
  NS_ASSERT (m_lastNavStart <= Simulator::Now ());
  if (hdr.GetRawDuration () > 32767)
    {
      //All stations process Duration field values less than or equal to 32 767 from valid data frames
      //to update their NAV settings as appropriate under the coordination function rules.
      return;
    }
  Time duration = hdr.GetDuration ();
  if (hdr.IsCfPoll () && hdr.GetAddr2 () == m_bssid)
    {
      //see section 9.3.2.2 802.11-1999
      DoNavResetNow (duration);
      return;
    }
  else if (hdr.IsCfEnd () && hdr.GetAddr2 () == m_bssid)
    {
      //see section 9.3.2.2 802.11-1999
      DoNavResetNow (Seconds (0));
      return;
    }
  else if (hdr.GetAddr1 () != m_self)
    {
      if (hdr.IsGrantFrame ())
        {
          // see section 9.33.7.3 802.11ad-2012
          Ptr<Packet> newPacket = packet->Copy ();
          CtrlDMG_Grant grant;
          newPacket->RemoveHeader (grant);
          Ptr<DmgStaWifiMac> highMac = DynamicCast<DmgStaWifiMac> (m_mac);
          if (grant.GetDynamicAllocationInfo ().GetSourceAID () == highMac->GetAssociationID () ||
              grant.GetDynamicAllocationInfo ().GetDestinationAID () == highMac->GetAssociationID ())
            {
              return;
            }
        }
      // see section 9.2.5.4 802.11-1999
      bool navUpdated = DoNavStartNow (duration);
      if (hdr.IsRts () && navUpdated)
        {
          /**
           * A STA that used information from an RTS frame as the most recent basis to update its NAV setting
           * is permitted to reset its NAV if no PHY-RXSTART.indication is detected from the PHY during a
           * period with a duration of (2 * aSIFSTime) + (CTS_Time) + aRxPHYStartDelay + (2 * aSlotTime)
           * starting at the PHY-RXEND.indication corresponding to the detection of the RTS frame. The
           * “CTS_Time” shall be calculated using the length of the CTS frame and the data rate at which
           * the RTS frame used for the most recent NAV update was received.
           */
          WifiMacHeader cts;
          cts.SetType (WIFI_MAC_CTL_CTS);
          WifiTxVector txVector = GetRtsTxVector (Create<const WifiMacQueueItem> (packet, hdr));
          Time navCounterResetCtsMissedDelay =
            m_phy->CalculateTxDuration (cts.GetSerializedSize (), txVector, m_phy->GetFrequency ()) +
            Time (2 * GetSifs ()) + Time (2 * GetSlotTime ()) +
            m_phy->CalculatePhyPreambleAndHeaderDuration (txVector);
          m_navCounterResetCtsMissed = Simulator::Schedule (navCounterResetCtsMissedDelay,
                                                            &MacLow::DoNavResetNow, this,
                                                            Seconds (0));
        }
    }
}

void
MacLow::DoNavResetNow (Time duration)
{
  NS_LOG_FUNCTION (this << duration);
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      (*i)->NotifyNavResetNow (duration);
    }
  m_lastNavStart = Simulator::Now ();
  m_lastNavDuration = duration;
}

bool
MacLow::DoNavStartNow (Time duration)
{
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      (*i)->NotifyNavStartNow (duration);
    }
  Time newNavEnd = Simulator::Now () + duration;
  Time oldNavEnd = m_lastNavStart + m_lastNavDuration;
  if (newNavEnd > oldNavEnd)
    {
      m_lastNavStart = Simulator::Now ();
      m_lastNavDuration = duration;
      return true;
    }
  return false;
}

void
MacLow::NotifyAckTimeoutStartNow (Time duration)
{
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      (*i)->NotifyAckTimeoutStartNow (duration);
    }
}

void
MacLow::NotifyAckTimeoutResetNow (void)
{
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      (*i)->NotifyAckTimeoutResetNow ();
    }
}

void
MacLow::NotifyCtsTimeoutStartNow (Time duration)
{
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      (*i)->NotifyCtsTimeoutStartNow (duration);
    }
}

void
MacLow::NotifyCtsTimeoutResetNow (void)
{
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      (*i)->NotifyCtsTimeoutResetNow ();
    }
}

void
MacLow::ForwardDown (Ptr<const WifiPsdu> psdu, WifiTxVector txVector)
{
  NS_LOG_FUNCTION (this << psdu << txVector);

  NS_ASSERT (psdu->GetNMpdus ());
  const WifiMacHeader& hdr = (*psdu->begin ())->GetHeader ();

  NS_LOG_DEBUG ("send " << hdr.GetTypeString () <<
                ", to=" << hdr.GetAddr1 () <<
                ", size=" << psdu->GetSize () <<
                ", mode=" << txVector.GetMode  () <<
                ", preamble=" << txVector.GetPreambleType () <<
                ", duration=" << hdr.GetDuration () <<
                ", seq=0x" << std::hex << hdr.GetSequenceControl () << std::dec);

  //// WIGIG ////
  /* Antenna steering */
  if ((m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ad) || (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay))
    {
      Ptr<DmgWifiMac> wifiMac = DynamicCast<DmgWifiMac> (m_mac);
      /* Change antenna configuration */
      if (((wifiMac->GetCurrentAccessPeriod () == CHANNEL_ACCESS_DTI) && (wifiMac->GetCurrentAllocation () == CBAP_ALLOCATION))
          || (wifiMac->GetCurrentAccessPeriod () == CHANNEL_ACCESS_ATI))
        {
          if ((wifiMac->GetTypeOfStation () == DMG_AP) && (hdr.IsAck () || hdr.IsBlockAck ()))
            {
              wifiMac->SteerTxAntennaToward (hdr.GetAddr1 (), (false));
            }
          else if (!(hdr.IsSSW () || hdr.IsSSW_ACK () || hdr.IsSSW_FBCK () || m_servingMimoBFT)) /* Special case to handle TXSS CBAP*/
            {
              wifiMac->SteerAntennaToward (hdr.GetAddr1 (), hdr.IsData ());
            }
        }
      else if (wifiMac->GetTypeOfStation () == DMG_ADHOC)
        {
          if ((hdr.IsAck () || hdr.IsBlockAck ()))
            {
              wifiMac->SteerTxAntennaToward (hdr.GetAddr1 (), true);
            }
          else
            {
              wifiMac->SteerAntennaToward (hdr.GetAddr1 (), (hdr.IsData () || hdr.IsAck () || hdr.IsBlockAck ()));
            }
        }
    }
  //// WIGIG ////

  if (hdr.IsCfPoll () && m_stationManager->GetPcfSupported ())
    {
      Simulator::Schedule (GetPifs () + m_phy->CalculateTxDuration (psdu->GetSize (), txVector, m_phy->GetFrequency ()), &MacLow::CfPollTimeout, this);
    }
  if (hdr.IsBeacon () && m_stationManager->GetPcfSupported ())
    {
      if (Simulator::Now () > m_lastBeacon + m_beaconInterval)
        {
          m_cfpForeshortening = (Simulator::Now () - m_lastBeacon - m_beaconInterval);
        }
      m_lastBeacon = Simulator::Now ();
    }
  else if (hdr.IsCfEnd () && m_stationManager->GetPcfSupported ())
    {
      m_cfpStart = NanoSeconds (0);
      m_cfpForeshortening = NanoSeconds (0);
      m_cfAckInfo.appendCfAck = false;
      m_cfAckInfo.expectCfAck = false;
    }
  else if (IsCfPeriod () && hdr.HasData ())
    {
      m_cfAckInfo.expectCfAck = true;
    }

  if (psdu->IsSingle ())
    {
      txVector.SetAggregation (true);
      NS_LOG_DEBUG ("Sending S-MPDU");
    }
  else if (psdu->IsAggregate ())
    {
      txVector.SetAggregation (true);
      NS_LOG_DEBUG ("Sending A-MPDU");
    }
  else
    {
      NS_LOG_DEBUG ("Sending non aggregate MPDU");
    }

  for (auto& mpdu : *PeekPointer (psdu))
    {
      if (mpdu->GetHeader ().IsQosData ())
        {
          auto edcaIt = m_edca.find (QosUtilsMapTidToAc (mpdu->GetHeader ().GetQosTid ()));
          edcaIt->second->CompleteMpduTx (mpdu);
        }
    }
  m_phy->Send (psdu, txVector);
}

void
MacLow::CfPollTimeout (void)
{
  NS_LOG_FUNCTION (this);
  //to be reworked
  bool busy = false;
  for (ChannelAccessManagersCI i = m_channelAccessManagers.begin (); i != m_channelAccessManagers.end (); i++)
    {
      busy = (*i)->IsBusy ();
    }
  if (!busy)
    {
      NS_ASSERT (m_currentTxop != 0);
      m_currentTxop->MissedCfPollResponse (m_cfAckInfo.expectCfAck);
      m_cfAckInfo.expectCfAck = false;
    }
}

void
MacLow::CtsTimeout (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("cts timeout");
  /// \todo should check that there was no RX start before now.
  /// we should restart a new CTS timeout now until the expected
  /// end of RX if there was a RX start before now.
  m_stationManager->ReportRtsFailed (m_currentPacket->GetAddr1 (), &m_currentPacket->GetHeader (0));

  Ptr<QosTxop> qosTxop = DynamicCast<QosTxop> (m_currentTxop);
  if (qosTxop != 0)
    {
      qosTxop->NotifyMissedCts (std::list<Ptr<WifiMacQueueItem>> (m_currentPacket->begin (), m_currentPacket->end ()));
    }
  else
    {
      m_currentTxop->MissedCts ();
    }
  m_currentTxop = 0;
  //// WIGIG ////
  m_currentPacket = 0;
}

void
MacLow::NormalAckTimeout (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("normal ack timeout");
  /// \todo should check that there was no RX start before now.
  /// we should restart a new ack timeout now until the expected
  /// end of RX if there was a RX start before now.
  Ptr<Txop> txop = m_currentTxop;
  m_currentTxop = 0;
  txop->MissedAck ();
  //// WIGIG ////
  m_currentPacket = 0;
}

void
MacLow::BlockAckTimeout (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("block ack timeout");
  Ptr<Txop> txop = m_currentTxop;
  m_currentTxop = 0;
  txop->MissedBlockAck (m_currentPacket->GetNMpdus ());
  //// WIGIG ////
  m_currentPacket = 0;
}

void
MacLow::SendRtsForPacket (void)
{
  NS_LOG_FUNCTION (this);
  /* send an RTS for this packet. */
  WifiMacHeader rts;
  rts.SetType (WIFI_MAC_CTL_RTS);
  rts.SetDsNotFrom ();
  rts.SetDsNotTo ();
  rts.SetNoRetry ();
  rts.SetNoMoreFragments ();
  rts.SetAddr1 (m_currentPacket->GetAddr1 ());
  rts.SetAddr2 (m_self);
  WifiTxVector rtsTxVector = GetRtsTxVector (*m_currentPacket->begin ());

  /* Check if a control trailer needs to be added to the RTS packet - for RTS packets before MIMO transmissions */
  bool addControlTrailer = false;
  DATA_COMMUNICATION_MODE dataMode = DATA_MODE_SISO;
  Ptr<DmgWifiMac> wifiMac = DynamicCast<DmgWifiMac> (m_mac);
  if (m_phy->GetStandard () == WIFI_PHY_STANDARD_80211ay)
    {
      dataMode = wifiMac->GetStationDataCommunicationMode (rts.GetAddr1 ());
      addControlTrailer = (dataMode != DATA_MODE_SISO);
      rtsTxVector.SetControlTrailerPresent (addControlTrailer);
    }

  Time duration = Seconds (0);

  duration += GetSifs ();
  duration += GetCtsDuration (m_currentPacket->GetAddr1 (), rtsTxVector, addControlTrailer);
  duration += GetSifs ();
  duration += m_phy->CalculateTxDuration (m_currentPacket->GetSize (),
                                          m_currentTxVector, m_phy->GetFrequency ());
  duration += GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
  if (m_txParams.HasNextPacket ())
    {
      duration += m_phy->CalculateTxDuration (m_txParams.GetNextPacketSize (),
                                              m_currentTxVector, m_phy->GetFrequency ());
      duration += GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
    }
  rts.SetDuration (duration);


  Time txDuration = m_phy->CalculateTxDuration (GetRtsSize (addControlTrailer), rtsTxVector, m_phy->GetFrequency ());
  // After transmitting an RTS frame, the STA shall wait for a CTSTimeout interval with
  // a value of aSIFSTime + aSlotTime + aRxPHYStartDelay (IEEE 802.11-2016 sec. 10.3.2.7).
  // aRxPHYStartDelay equals the time to transmit the PHY header.
  Time timerDelay = txDuration + GetSifs () + GetSlotTime ()
                    + m_phy->CalculatePhyPreambleAndHeaderDuration (rtsTxVector);
  NS_ASSERT (m_ctsTimeoutEvent.IsExpired ());
  NotifyCtsTimeoutStartNow (timerDelay);
  m_ctsTimeoutEvent = Simulator::Schedule (timerDelay, &MacLow::CtsTimeout, this);

  Ptr<Packet> packet = Create<Packet> ();
  if (addControlTrailer)
    {
      ControlTrailer ct;
      ct.SetControlTrailerFormatType (CT_TYPE_GRANT_RTS_CTS2SELF);
      ct.SetAsMimoTransmission (true);
      if (dataMode == DATA_MODE_SU_MIMO)
        {
          // For now we use the top combination as reported by the SU_MIMO BFT protocol for MIMO communication.
          ct.SetTxSectorCombinationIdx (0);
          wifiMac->UpdateBestMimoTxAntennaConfigurationIndex (rts.GetAddr1 (), 0);
        }
      else
        {
          ct.SetAsMuMimoTransmission (true);
          // To do: Set the other parameters regarding MU-MIMO RTS
        }
      packet->AddHeader (ct);
    }
  ForwardDown (Create<const WifiPsdu> (packet, rts), rtsTxVector);
}

void
MacLow::StartDataTxTimers (WifiTxVector dataTxVector)
{
  NS_LOG_FUNCTION (this);
  Time txDuration = m_phy->CalculateTxDuration (m_currentPacket->GetSize (), dataTxVector, m_phy->GetFrequency ());
  if (m_txParams.MustWaitNormalAck () && !IsCfPeriod ())
    {
      // the timeout duration is "aSIFSTime + aSlotTime + aRxPHYStartDelay, starting
      // at the PHY-TXEND.confirm primitive" (section 10.3.2.9 or 10.22.2.2 of 802.11-2016).
      // aRxPHYStartDelay equals the time to transmit the PHY header.
      WifiTxVector ackTxVector = GetAckTxVector (m_currentPacket->GetAddr1 (),
                                                 dataTxVector.GetMode ());
      Time timerDelay = txDuration + GetSifs () + GetSlotTime ()
                        + m_phy->CalculatePhyPreambleAndHeaderDuration (ackTxVector);
      NS_ASSERT (m_normalAckTimeoutEvent.IsExpired ());
      NotifyAckTimeoutStartNow (timerDelay);
      m_normalAckTimeoutEvent = Simulator::Schedule (timerDelay, &MacLow::NormalAckTimeout, this);
    }
  else if (m_txParams.MustWaitBlockAck ())
    {
      // the timeout duration is "aSIFSTime + aSlotTime + aRxPHYStartDelay, starting
      // at the PHY-TXEND.confirm primitive" (section 10.3.2.9 or 10.22.2.2 of 802.11-2016).
      // aRxPHYStartDelay equals the time to transmit the PHY header.
      WifiTxVector blockAckTxVector = GetBlockAckTxVector (m_currentPacket->GetAddr1 (),
                                                           dataTxVector.GetMode ());
      Time timerDelay = txDuration + GetSifs () + GetSlotTime ()
                        + m_phy->CalculatePhyPreambleAndHeaderDuration (blockAckTxVector);
      NS_ASSERT (m_blockAckTimeoutEvent.IsExpired ());
      NotifyAckTimeoutStartNow (timerDelay);
      m_blockAckTimeoutEvent = Simulator::Schedule (timerDelay, &MacLow::BlockAckTimeout, this);
    }
  else if (m_txParams.HasNextPacket ())
    {
      NS_ASSERT (m_waitIfsEvent.IsExpired ());
      Time delay = txDuration;
      if (m_stationManager->GetRifsPermitted ())
        {
          delay += GetRifs ();
        }
      else
        {
          delay += GetSifs ();
        }
      m_waitIfsEvent = Simulator::Schedule (delay, &MacLow::WaitIfsAfterEndTxFragment, this);
    }
  else if (m_currentPacket->GetHeader (0).IsQosData () && m_currentTxop->IsQosTxop () &&
           m_currentTxop->GetTxopLimit ().IsStrictlyPositive ()
           //// WIGIG ////
           //&& m_currentTxop->GetTxopRemaining () > GetSifs ()
           && m_currentTxop->GetRemainingTimeForTransmission () > GetSifs ())
           //// WIGIG ////
    {
      Time delay = txDuration;
      if (m_stationManager->GetRifsPermitted ())
        {
          delay += GetRifs ();
        }
      else
        {
          delay += GetSifs ();
        }
      m_waitIfsEvent = Simulator::Schedule (delay, &MacLow::WaitIfsAfterEndTxPacket, this);
    }
  else
    {
      // since we do not expect any timer to be triggered.
      m_endTxNoAckEvent = Simulator::Schedule (txDuration, &MacLow::EndTxNoAck, this);
    }
}

void
MacLow::SendDataPacket (void)
{
  NS_LOG_FUNCTION (this);
  /* send this packet directly. No RTS is needed. */
  StartDataTxTimers (m_currentTxVector);
  //// WIGIG ////
  if (m_txParams.HasDurationId ()) //// (Modification)
    {
      m_currentPacket->SetDuration (m_txParams.GetDurationId ());
    }
  //// WIGIG ////
  else if (!IsCfPeriod ())
    {
      Time duration = GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
      if (m_txParams.HasNextPacket ())
        {
          if (m_stationManager->GetRifsPermitted ())
            {
              duration += GetRifs ();
            }
          else
            {
              duration += GetSifs ();
            }
          duration += m_phy->CalculateTxDuration (m_txParams.GetNextPacketSize (),
                                                  m_currentTxVector, m_phy->GetFrequency ());
          duration += GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
        }
      m_currentPacket->SetDuration (duration);
    }
  else
    {
      if (m_currentPacket->GetHeader (0).IsCfEnd ())
        {
          m_currentPacket->GetHeader (0).SetRawDuration (0);
        }
      else
        {
          m_currentPacket->GetHeader (0).SetRawDuration (32768);
        }
    }

  if (!m_currentPacket->IsAggregate ())
    {
      if (m_cfAckInfo.appendCfAck)
        {
          switch (m_currentPacket->GetHeader (0).GetType ())
            {
            case WIFI_MAC_DATA:
              m_currentPacket->GetHeader (0).SetType (WIFI_MAC_DATA_CFACK, false);
              break;
            case WIFI_MAC_DATA_CFPOLL:
              m_currentPacket->GetHeader (0).SetType (WIFI_MAC_DATA_CFACK_CFPOLL, false);
              break;
            case WIFI_MAC_DATA_NULL:
              m_currentPacket->GetHeader (0).SetType (WIFI_MAC_DATA_NULL_CFACK, false);
              break;
            case WIFI_MAC_DATA_NULL_CFPOLL:
              m_currentPacket->GetHeader (0).SetType (WIFI_MAC_DATA_NULL_CFACK_CFPOLL, false);
              break;
            case WIFI_MAC_CTL_END:
              m_currentPacket->GetHeader (0).SetType (WIFI_MAC_CTL_END_ACK, false);
              break;
            default:
              NS_ASSERT (false);
              break;
            }
          NS_ASSERT (m_cfAckInfo.address != Mac48Address ());
          //Standard says that, for frames of type Data+CF-Ack, Data+CF-Poll+CF-Ack, and CF-Poll+CF-Ack,
          //the rate chosen to transmit the frame must be supported by both the addressed recipient STA and the STA to which the Ack is intended.
          //This ideally requires the rate manager to handle this case, but this requires to update all rate manager classes.
          //Instead, we simply fetch two TxVector and we select the one with the lowest data rate.
          //This should be later changed, at the latest once HCCA is implemented for HT/VHT/HE stations.
          WifiMacHeader tmpHdr = m_currentPacket->GetHeader (0);
          tmpHdr.SetAddr1 (m_cfAckInfo.address);
          WifiTxVector tmpTxVector = GetDataTxVector (Create<const WifiMacQueueItem> (m_currentPacket->GetPayload (0), tmpHdr));
          if (tmpTxVector.GetMode ().GetDataRate (tmpTxVector) < m_currentTxVector.GetMode ().GetDataRate (m_currentTxVector))
            {
              m_currentTxVector = tmpTxVector;
            }
          m_cfAckInfo.appendCfAck = false;
          m_cfAckInfo.address = Mac48Address ();
        }
    }
  if (m_txParams.MustSendBlockAckRequest ())
    {
      Ptr<QosTxop> qosTxop = DynamicCast<QosTxop> (m_currentTxop);
      NS_ASSERT (qosTxop != 0);
      auto bar = qosTxop->PrepareBlockAckRequest (m_currentPacket->GetAddr1 (), *m_currentPacket->GetTids ().begin ());
      qosTxop->ScheduleBar (bar);
    }
  ForwardDown (m_currentPacket, m_currentTxVector);
}

bool
MacLow::IsNavZero (void) const
{
  return (m_lastNavStart + m_lastNavDuration < Simulator::Now ());
}

void
MacLow::SendCtsToSelf (void)
{
  WifiMacHeader cts;
  cts.SetType (WIFI_MAC_CTL_CTS);
  cts.SetDsNotFrom ();
  cts.SetDsNotTo ();
  cts.SetNoMoreFragments ();
  cts.SetNoRetry ();
  cts.SetAddr1 (m_self);

  WifiTxVector ctsTxVector = GetRtsTxVector (*m_currentPacket->begin ());
  Time duration = Seconds (0);

  duration += GetSifs ();
  duration += m_phy->CalculateTxDuration (m_currentPacket->GetSize (),
                                          m_currentTxVector, m_phy->GetFrequency ());
  duration += GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
  if (m_txParams.HasNextPacket ())
    {
      duration += GetSifs ();
      duration += m_phy->CalculateTxDuration (m_txParams.GetNextPacketSize (),
                                              m_currentTxVector, m_phy->GetFrequency ());
      duration += GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
    }

  cts.SetDuration (duration);

  ForwardDown (Create<const WifiPsdu> (Create<Packet> (), cts), ctsTxVector);

  Time txDuration = m_phy->CalculateTxDuration (GetCtsSize (), ctsTxVector, m_phy->GetFrequency ());
  txDuration += GetSifs ();
  NS_ASSERT (m_sendDataEvent.IsExpired ());

  m_sendDataEvent = Simulator::Schedule (txDuration,
                                         &MacLow::SendDataAfterCts, this,
                                         duration);
}

void
MacLow::SendCtsAfterRts (Mac48Address source, Time duration, WifiTxVector rtsTxVector, double rtsSnr)
{
  NS_LOG_FUNCTION (this << source << duration << rtsTxVector.GetMode () << rtsSnr);
  /* send a CTS when you receive a RTS
   * right after SIFS.
   */
  WifiTxVector ctsTxVector = GetCtsTxVector (source, rtsTxVector.GetMode ());
  WifiMacHeader cts;
  cts.SetType (WIFI_MAC_CTL_CTS);
  cts.SetDsNotFrom ();
  cts.SetDsNotTo ();
  cts.SetNoMoreFragments ();
  cts.SetNoRetry ();
  cts.SetAddr1 (source);
  duration -= GetCtsDuration (source, rtsTxVector);
  duration -= GetSifs ();
  NS_ASSERT (duration.IsPositive ());
  cts.SetDuration (duration);

  Ptr<Packet> packet = Create<Packet> ();

  SnrTag tag;
  tag.Set (rtsSnr);
  packet->AddPacketTag (tag);

  //CTS should always use non-HT PPDU (HT PPDU cases not supported yet)
  ForwardDown (Create<const WifiPsdu> (packet, cts), ctsTxVector);
}

//// WIGIG ////
void
MacLow::SendDmgCtsAfterRts (Mac48Address source, Time duration, WifiTxVector rtsTxVector, double rtsSnr)
{
  NS_LOG_FUNCTION (this << source << duration << rtsTxVector.GetMode () << rtsSnr);
  /* Send a DMG CTS when we receive a RTS right after SIFS. */
  WifiTxVector ctsTxVector = GetDmgControlTxVector ();
  WifiMacHeader cts;
  cts.SetType (WIFI_MAC_CTL_DMG_CTS);
  cts.SetDsNotFrom ();
  cts.SetDsNotTo ();
  cts.SetNoMoreFragments ();
  cts.SetNoRetry ();
  cts.SetAddr1 (source);
  cts.SetAddr2 (GetAddress ());

  /* Check if a control trailer needs to be added to the DMG CTS - for CTS before a MIMO transmission */
  Ptr<DmgWifiMac> wifiMac = DynamicCast<DmgWifiMac> (m_mac);
  DATA_COMMUNICATION_MODE dataMode = wifiMac->GetStationDataCommunicationMode (source);
  bool addControlTrailer = (dataMode != DATA_MODE_SISO);
  ctsTxVector.SetControlTrailerPresent (addControlTrailer);

  /* Set duration field */
  duration -= GetDmgCtsDuration (addControlTrailer);
  duration -= GetSifs ();
  NS_ASSERT (duration.IsPositive ());
  cts.SetDuration (duration);

  Ptr<Packet> packet = Create<Packet> ();

  SnrTag tag;
  tag.Set (rtsSnr);
  packet->AddPacketTag (tag);

  /* When using RTS/CTS for channel access for MIMO communication a control trailer needs to be added to the packet */
  if (addControlTrailer)
    {
      ControlTrailer ct;
      ct.SetControlTrailerFormatType (CT_TYPE_CTS_DTS);
      ct.SetAsMimoTransmission (false);
      /* For now only the station transmitting the data transmits in MIMO while the receiver responds in SISO mode */
//      if (dataMode == DATA_MODE_SU_MIMO)
//        {
//          // For now we use the top combination as reported by the SU_MIMO BFT protocol for MIMO communication.
//          ct.SetTxSectorCombinationIdx (0);
//          wifiMac->UpdateBestMimoTxAntennaConfigurationIndex (source, 0);
//        }
//      else
//        {
//          ct.SetAsMuMimoTransmission (true);
//          // To do: Set the other parameters regarding MU-MIMO RTS
//        }
      packet->AddHeader (ct);
      /* After sending the CTS set up the receive antennas in the comfiguration needed for MIMO reception */
      Simulator::Schedule (GetDmgCtsDuration (addControlTrailer), &DmgWifiMac::SteerMimoRxAntennaToward, wifiMac, source);
    }

  ForwardDown (Create<const WifiPsdu> (packet, cts), ctsTxVector);
}
//// WIGIG ////

void
MacLow::SendDataAfterCts (Time duration)
{
  NS_LOG_FUNCTION (this);
  /* send the third step in a
   * RTS/CTS/Data/Ack handshake
   */
  NS_ASSERT (m_currentPacket != 0);

  StartDataTxTimers (m_currentTxVector);
  Time newDuration = GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
  if (m_txParams.HasNextPacket ())
    {
      if (m_stationManager->GetRifsPermitted ())
        {
          newDuration += GetRifs ();
        }
      else
        {
          newDuration += GetSifs ();
        }
      newDuration += m_phy->CalculateTxDuration (m_txParams.GetNextPacketSize (), m_currentTxVector, m_phy->GetFrequency ());
      newDuration += GetResponseDuration (m_txParams, m_currentTxVector, m_currentPacket->GetAddr1 ());
    }

  Time txDuration = m_phy->CalculateTxDuration (m_currentPacket->GetSize (), m_currentTxVector, m_phy->GetFrequency ());
  duration -= txDuration;
  duration -= GetSifs ();

  duration = std::max (duration, newDuration);
  NS_ASSERT (duration.IsPositive ());
  m_currentPacket->SetDuration (duration);
  if (m_txParams.MustSendBlockAckRequest ())
    {
      Ptr<QosTxop> qosTxop = DynamicCast<QosTxop> (m_currentTxop);
      NS_ASSERT (qosTxop != 0);
      auto bar = qosTxop->PrepareBlockAckRequest (m_currentPacket->GetAddr1 (), *m_currentPacket->GetTids ().begin ());
      qosTxop->ScheduleBar (bar);
    }
  ForwardDown (m_currentPacket, m_currentTxVector);
}

void
MacLow::WaitIfsAfterEndTxFragment (void)
{
  NS_LOG_FUNCTION (this);
  m_currentTxop->StartNextFragment ();
}

void
MacLow::WaitIfsAfterEndTxPacket (void)
{
  NS_LOG_FUNCTION (this);
  m_currentTxop->StartNextPacket ();
}

void
MacLow::EndTxNoAck (void)
{
  NS_LOG_FUNCTION (this);
  if (m_currentTxop != 0)
    {
      if (m_currentPacket->GetHeader (0).IsBeacon () && m_stationManager->GetPcfSupported ())
        {
          m_cfpStart = Simulator::Now ();
        }
      if (!m_cfAckInfo.expectCfAck)
        {
          Ptr<Txop> txop = m_currentTxop;
          txop->EndTxNoAck ();
        }
      if (!IsCfPeriod ())
        {
          m_currentTxop = 0;
        }
    }
  else
    {
      if (m_currentPacket->IsShortSSW ())
        m_transmissionShortSswCallback ();
      else
        m_transmissionCallback (m_currentPacket->GetHeader (0));
    }
  //// WIGIG ////
  m_currentPacket = 0;
  //// WIGIG ////
}

void
MacLow::SendAckAfterData (Mac48Address source, Time duration, WifiMode dataTxMode, double dataSnr)
{
  NS_LOG_FUNCTION (this);
  if (!m_phy->IsStateTx () && !m_phy->IsStateSwitching ()) //// WIGIG //// (Modification)
    {
      // send an Ack, after SIFS, when you receive a packet
      WifiTxVector ackTxVector = GetAckTxVector (source, dataTxMode);
      WifiMacHeader ack;
      ack.SetType (WIFI_MAC_CTL_ACK);
      ack.SetDsNotFrom ();
      ack.SetDsNotTo ();
      ack.SetNoRetry ();
      ack.SetNoMoreFragments ();
      ack.SetAddr1 (source);
      // 802.11-2012, Section 8.3.1.4:  Duration/ID is received duration value
      // minus the time to transmit the Ack frame and its SIFS interval
      duration -= GetAckDuration (ackTxVector);
      duration -= GetSifs ();
      NS_ASSERT_MSG (duration.IsPositive (), "Please provide test case to maintainers if this assert is hit.");
      ack.SetDuration (duration);

      Ptr<Packet> packet = Create<Packet> ();

      SnrTag tag;
      tag.Set (dataSnr);
      packet->AddPacketTag (tag);

      //Ack should always use non-HT PPDU (HT PPDU cases not supported yet)
      ForwardDown (Create<const WifiPsdu> (packet, ack), ackTxVector);
    }
  else
    {
      NS_LOG_DEBUG ("Skip ack after data");
    }
}

bool
MacLow::ReceiveMpdu (Ptr<WifiMacQueueItem> mpdu)
{
  const WifiMacHeader& hdr = mpdu->GetHeader ();

  if (m_stationManager->GetHtSupported ()
      || m_stationManager->GetVhtSupported ()
      || m_stationManager->GetHeSupported ()
      //// WIGIG ////
      || m_stationManager->HasDmgSupported ()
      || m_stationManager->HasEdmgSupported ())
      //// WIGIG ////
    {
      Mac48Address originator = hdr.GetAddr2 ();
      uint8_t tid = 0;
      if (hdr.IsQosData ())
        {
          tid = hdr.GetQosTid ();
        }
      uint16_t seqNumber = hdr.GetSequenceNumber ();
      AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, tid));
      if (it != m_bAckAgreements.end ())
        {
          //Implement HT immediate BlockAck support for HT Delayed BlockAck is not added yet
          if (!QosUtilsIsOldPacket ((*it).second.first.GetStartingSequence (), seqNumber))
            {
              StoreMpduIfNeeded (mpdu);
              if (!IsInWindow (hdr.GetSequenceNumber (), (*it).second.first.GetStartingSequence (), (*it).second.first.GetBufferSize ()))
                {
                  uint16_t delta = (seqNumber - (*it).second.first.GetWinEnd () + 4096) % 4096;
                  NS_ASSERT (delta > 0);
                  uint16_t bufferSize = (*it).second.first.GetBufferSize ();
                  uint16_t startingSeq = (seqNumber - bufferSize + 1 + 4096) % 4096;
                  (*it).second.first.SetStartingSequence (startingSeq);
                  RxCompleteBufferedPacketsWithSmallerSequence ((*it).second.first.GetStartingSequenceControl (), originator, tid);
                }
              RxCompleteBufferedPacketsUntilFirstLost (originator, tid); //forwards up packets starting from winstart and set winstart to last +1
            }
          return true;
        }
      return false;
    }
  return StoreMpduIfNeeded (mpdu);
}

bool
MacLow::StoreMpduIfNeeded (Ptr<WifiMacQueueItem> mpdu)
{
  const WifiMacHeader& hdr = mpdu->GetHeader ();

  AgreementsI it = m_bAckAgreements.find (std::make_pair (hdr.GetAddr2 (), hdr.GetQosTid ()));
  if (it != m_bAckAgreements.end ())
    {
      uint16_t endSequence = ((*it).second.first.GetStartingSequence () + 2047) % 4096;
      uint32_t mappedSeqControl = QosUtilsMapSeqControlToUniqueInteger (hdr.GetSequenceControl (), endSequence);

      BufferedPacketI i = (*it).second.second.begin ();
      for (; i != (*it).second.second.end ()
           && QosUtilsMapSeqControlToUniqueInteger ((*i)->GetHeader ().GetSequenceControl (), endSequence) < mappedSeqControl; i++)
        {
        }
      (*it).second.second.insert (i, mpdu);

      //Update block ack cache
      BlockAckCachesI j = m_bAckCaches.find (std::make_pair (hdr.GetAddr2 (), hdr.GetQosTid ()));
      NS_ASSERT (j != m_bAckCaches.end ());
      (*j).second.UpdateWithMpdu (&hdr);
      return true;
    }
  return false;
}

void
MacLow::CreateBlockAckAgreement (const MgtAddBaResponseHeader *respHdr, Mac48Address originator,
                                 uint16_t startingSeq)
{
  NS_LOG_FUNCTION (this);
  uint8_t tid = respHdr->GetTid ();
  BlockAckAgreement agreement (originator, tid);
  if (respHdr->IsImmediateBlockAck ())
    {
      agreement.SetImmediateBlockAck ();
    }
  else
    {
      agreement.SetDelayedBlockAck ();
    }
  agreement.SetAmsduSupport (respHdr->IsAmsduSupported ());
  agreement.SetBufferSize (respHdr->GetBufferSize () + 1);
  agreement.SetTimeout (respHdr->GetTimeout ());
  agreement.SetStartingSequence (startingSeq);

  std::list<Ptr<WifiMacQueueItem>> buffer (0);
  AgreementKey key (originator, respHdr->GetTid ());
  AgreementValue value (agreement, buffer);
  m_bAckAgreements.insert (std::make_pair (key, value));

  BlockAckCache cache;
  cache.Init (startingSeq, respHdr->GetBufferSize () + 1);
  m_bAckCaches.insert (std::make_pair (key, cache));

  if (respHdr->GetTimeout () != 0)
    {
      AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, respHdr->GetTid ()));
      Time timeout = MicroSeconds (1024 * agreement.GetTimeout ());

      AcIndex ac = QosUtilsMapTidToAc (agreement.GetTid ());

      it->second.first.m_inactivityEvent = Simulator::Schedule (timeout,
                                                                &QosTxop::SendDelbaFrame,
                                                                m_edca[ac], originator, tid, false);
    }
}

void
MacLow::DestroyBlockAckAgreement (Mac48Address originator, uint8_t tid)
{
  NS_LOG_FUNCTION (this);
  AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, tid));
  if (it != m_bAckAgreements.end ())
    {
      RxCompleteBufferedPacketsWithSmallerSequence (it->second.first.GetStartingSequenceControl (), originator, tid);
      RxCompleteBufferedPacketsUntilFirstLost (originator, tid);
      m_bAckAgreements.erase (it);
      BlockAckCachesI i = m_bAckCaches.find (std::make_pair (originator, tid));
      NS_ASSERT (i != m_bAckCaches.end ());
      m_bAckCaches.erase (i);
    }
}

void
MacLow::RxCompleteBufferedPacketsWithSmallerSequence (uint16_t seq, Mac48Address originator, uint8_t tid)
{
  AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, tid));
  if (it != m_bAckAgreements.end ())
    {
      uint16_t endSequence = ((*it).second.first.GetStartingSequence () + 2047) % 4096;
      uint32_t mappedStart = QosUtilsMapSeqControlToUniqueInteger (seq, endSequence);
      BufferedPacketI last = (*it).second.second.begin ();
      uint16_t guard = 0;
      if (last != (*it).second.second.end ())
        {
          guard = (*(*it).second.second.begin ())->GetHeader ().GetSequenceControl ();
        }
      BufferedPacketI i = (*it).second.second.begin ();
      for (; i != (*it).second.second.end ()
           && QosUtilsMapSeqControlToUniqueInteger ((*i)->GetHeader ().GetSequenceControl (), endSequence) < mappedStart; )
        {
          if (guard == (*i)->GetHeader ().GetSequenceControl ())
            {
              if (!(*i)->GetHeader ().IsMoreFragments ())
                {
                  while (last != i)
                    {
                      m_rxCallback (*last);
                      last++;
                    }
                  m_rxCallback (*last);
                  last++;
                  /* go to next packet */
                  while (i != (*it).second.second.end () && guard == (*i)->GetHeader ().GetSequenceControl ())
                    {
                      i++;
                    }
                  if (i != (*it).second.second.end ())
                    {
                      guard = (*i)->GetHeader ().GetSequenceControl ();
                      last = i;
                    }
                }
              else
                {
                  guard++;
                }
            }
          else
            {
              /* go to next packet */
              while (i != (*it).second.second.end () && guard == (*i)->GetHeader ().GetSequenceControl ())
                {
                  i++;
                }
              if (i != (*it).second.second.end ())
                {
                  guard = (*i)->GetHeader ().GetSequenceControl ();
                  last = i;
                }
            }
        }
      (*it).second.second.erase ((*it).second.second.begin (), i);
    }
}

void
MacLow::RxCompleteBufferedPacketsUntilFirstLost (Mac48Address originator, uint8_t tid)
{
  AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, tid));
  if (it != m_bAckAgreements.end ())
    {
      uint16_t guard = (*it).second.first.GetStartingSequenceControl ();
      BufferedPacketI lastComplete = (*it).second.second.begin ();
      BufferedPacketI i = (*it).second.second.begin ();
      for (; i != (*it).second.second.end () && guard == (*i)->GetHeader ().GetSequenceControl (); i++)
        {
          if (!(*i)->GetHeader ().IsMoreFragments ())
            {
              while (lastComplete != i)
                {
                  m_rxCallback (*lastComplete);
                  lastComplete++;
                }
              m_rxCallback (*lastComplete);
              lastComplete++;
            }
          guard = (*i)->GetHeader ().IsMoreFragments () ? (guard + 1) : ((guard + 16) & 0xfff0);
        }
      (*it).second.first.SetStartingSequenceControl (guard);
      /* All packets already forwarded to WifiMac must be removed from buffer:
      [begin (), lastComplete) */
      (*it).second.second.erase ((*it).second.second.begin (), lastComplete);
    }
}

void
MacLow::SendBlockAckResponse (const CtrlBAckResponseHeader* blockAck, Mac48Address originator, bool immediate,
                              Time duration, WifiMode blockAckReqTxMode, double rxSnr)
{
  NS_LOG_FUNCTION (this);
  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (*blockAck);

  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_BACKRESP);
  hdr.SetAddr1 (originator);
  hdr.SetAddr2 (GetAddress ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  hdr.SetNoRetry ();
  hdr.SetNoMoreFragments ();

  WifiTxVector blockAckReqTxVector = GetBlockAckTxVector (originator, blockAckReqTxMode);

  if (immediate)
    {
      m_txParams.DisableAck ();
      duration -= GetSifs ();
      duration -= GetBlockAckDuration (blockAckReqTxVector, blockAck->GetType ());
    }
  else
    {
      m_txParams.EnableAck ();
      duration += GetSifs ();
      duration += GetAckDuration (originator, blockAckReqTxVector);
    }
  m_txParams.DisableNextData ();

  if (!immediate)
    {
      StartDataTxTimers (blockAckReqTxVector);
    }

  NS_ASSERT (duration.IsPositive ());
  hdr.SetDuration (duration);
  //here should be present a control about immediate or delayed BlockAck
  //for now we assume immediate
  SnrTag tag;
  tag.Set (rxSnr);
  packet->AddPacketTag (tag);
  ForwardDown (Create<const WifiPsdu> (packet, hdr), blockAckReqTxVector);
}

void
MacLow::SendBlockAckAfterAmpdu (uint8_t tid, Mac48Address originator, Time duration, WifiTxVector blockAckReqTxVector, double rxSnr)
{
  NS_LOG_FUNCTION (this);
  if (!m_phy->IsStateTx () && !m_phy->IsStateSwitching ()) //// WIGIG //// (Modification)
    {
      NS_LOG_FUNCTION (this << +tid << originator << duration.As (Time::S) << blockAckReqTxVector << rxSnr);
      CtrlBAckResponseHeader blockAck;
      uint16_t seqNumber = 0;
      BlockAckCachesI i = m_bAckCaches.find (std::make_pair (originator, tid));
      NS_ASSERT (i != m_bAckCaches.end ());
      seqNumber = (*i).second.GetWinStart ();

      bool immediate = true;
      AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, tid));
      blockAck.SetStartingSequence (seqNumber);
      blockAck.SetTidInfo (tid);
      immediate = (*it).second.first.IsImmediateBlockAck ();
      //// WIGIG ////
      if (m_stationManager->HasEdmgSupported ())
        {
          blockAck.SetType (BlockAckType::EDMG_COMPRESSED_BLOCK_ACK);
        }
      else
      //// WIGIG ////
        {
          if ((*it).second.first.GetBufferSize () > 64)
            {
              blockAck.SetType (EXTENDED_COMPRESSED_BLOCK_ACK);
            }
          else
            {
              blockAck.SetType (COMPRESSED_BLOCK_ACK);
            }
        }
      NS_LOG_DEBUG ("Got Implicit block Ack Req with seq " << seqNumber);
      (*i).second.FillBlockAckBitmap (&blockAck);

      WifiTxVector blockAckTxVector = GetBlockAckTxVector (originator, blockAckReqTxVector.GetMode ());

      SendBlockAckResponse (&blockAck, originator, immediate, duration, blockAckTxVector.GetMode (), rxSnr);
    }
  else
    {
      NS_LOG_DEBUG ("Skip block ack response!");
    }
}

void
MacLow::SendBlockAckAfterBlockAckRequest (const CtrlBAckRequestHeader reqHdr, Mac48Address originator,
                                          Time duration, WifiMode blockAckReqTxMode, double rxSnr)
{
  NS_LOG_FUNCTION (this);
  if (!m_phy->IsStateTx () && !m_phy->IsStateSwitching ()) //// WIGIG //// (Modification)
    {
      CtrlBAckResponseHeader blockAck;
      uint8_t tid = 0;
      bool immediate = false;
      if (!reqHdr.IsMultiTid ())
        {
          tid = reqHdr.GetTidInfo ();
          AgreementsI it = m_bAckAgreements.find (std::make_pair (originator, tid));
          if (it != m_bAckAgreements.end ())
            {
              blockAck.SetStartingSequence (reqHdr.GetStartingSequence ());
              blockAck.SetTidInfo (tid);
              immediate = (*it).second.first.IsImmediateBlockAck ();
              //// WIGIG ////
              if (m_stationManager->HasEdmgSupported ())
                {
                  blockAck.SetType (EDMG_COMPRESSED_BLOCK_ACK);
                }
              else
              //// WIGIG ////
                {
                  if (reqHdr.IsBasic ())
                    {
                      blockAck.SetType (BASIC_BLOCK_ACK);
                    }
                  else if (reqHdr.IsCompressed ())
                    {
                      blockAck.SetType (COMPRESSED_BLOCK_ACK);
                    }
                  else if (reqHdr.IsExtendedCompressed ())
                    {
                      blockAck.SetType (EXTENDED_COMPRESSED_BLOCK_ACK);
                    }
                }
              BlockAckCachesI i = m_bAckCaches.find (std::make_pair (originator, tid));
              NS_ASSERT (i != m_bAckCaches.end ());
              (*i).second.FillBlockAckBitmap (&blockAck);
              NS_LOG_DEBUG ("Got block Ack Req with seq " << reqHdr.GetStartingSequence ());

              if (!m_stationManager->GetHtSupported ()
                  && !m_stationManager->GetVhtSupported ()
                  && !m_stationManager->GetHeSupported ()
                  //// WIGIG ////
                  && !m_stationManager->HasDmgSupported ()
                  && !m_stationManager->HasEdmgSupported ())
                //// WIGIG ////
                {
                  /* All packets with smaller sequence than starting sequence control must be passed up to WifiMac
                           * See 9.10.3 in IEEE 802.11e standard.
                           */
                  RxCompleteBufferedPacketsWithSmallerSequence (reqHdr.GetStartingSequenceControl (), originator, tid);
                  RxCompleteBufferedPacketsUntilFirstLost (originator, tid);
                }
              else
                {
                  if (!QosUtilsIsOldPacket ((*it).second.first.GetStartingSequence (), reqHdr.GetStartingSequence ()))
                    {
                      (*it).second.first.SetStartingSequence (reqHdr.GetStartingSequence ());
                      RxCompleteBufferedPacketsWithSmallerSequence (reqHdr.GetStartingSequenceControl (), originator, tid);
                      RxCompleteBufferedPacketsUntilFirstLost (originator, tid);
                    }
                }
            }
          else
            {
              NS_LOG_DEBUG ("there's not a valid block ack agreement with " << originator);
            }
        }
      else
        {
          NS_FATAL_ERROR ("Multi-tid block ack is not supported.");
        }
      SendBlockAckResponse (&blockAck, originator, immediate, duration, blockAckReqTxMode, rxSnr);
    }
  else
    {
      NS_LOG_DEBUG ("Skip block ack response!");
    }
}

void
MacLow::ResetBlockAckInactivityTimerIfNeeded (BlockAckAgreement &agreement)
{
  if (agreement.GetTimeout () != 0)
    {
      NS_ASSERT (agreement.m_inactivityEvent.IsRunning ());
      agreement.m_inactivityEvent.Cancel ();
      Time timeout = MicroSeconds (1024 * agreement.GetTimeout ());
      AcIndex ac = QosUtilsMapTidToAc (agreement.GetTid ());
      agreement.m_inactivityEvent = Simulator::Schedule (timeout,
                                                         &QosTxop::SendDelbaFrame,
                                                         m_edca[ac], agreement.GetPeer (),
                                                         agreement.GetTid (), false);
    }
}

void
MacLow::RegisterEdcaForAc (AcIndex ac, Ptr<QosTxop> edca)
{
  m_edca.insert (std::make_pair (ac, edca));
}

void
MacLow::DeaggregateAmpduAndReceive (Ptr<WifiPsdu> psdu, double rxSnr, WifiTxVector txVector, std::vector<bool> statusPerMpdu)
{
  NS_LOG_FUNCTION (this);
  bool normalAck = false;
  bool ampduSubframe = false; //flag indicating the packet belongs to an A-MPDU and is not a VHT/HE single MPDU
  if (txVector.IsAggregation ())
    {
      NS_ASSERT (psdu->IsAggregate ());

      ampduSubframe = true;
      auto n = psdu->begin ();
      auto status = statusPerMpdu.begin ();
      NS_ABORT_MSG_IF (psdu->GetNMpdus () != statusPerMpdu.size (), "Should have one receive status per MPDU");

      WifiMacHeader firsthdr = (*n)->GetHeader ();

      //// WIGIG ////
      /* No need to continue processing the received A-MPDU, if we are performing SLS. */
      if (m_servingSLS)
        {
          NS_LOG_DEBUG ("Perfomring SLS BFT, so ignoe the received A-MPDU from " << firsthdr.GetAddr2 () <<
                        " with sequence=" << firsthdr.GetSequenceNumber ());
          return;
        }
      if (m_servingMimoBFT)
        {
          NS_LOG_DEBUG ("Perfomring MIMO BFT, so ignoe the received A-MPDU from " << firsthdr.GetAddr2 () <<
                        " with sequence=" << firsthdr.GetSequenceNumber ());
          return;
        }
      //// WIGIG ////

      NS_LOG_DEBUG ("duration/id=" << firsthdr.GetDuration ());
      NotifyNav ((*n)->GetPacket (), firsthdr);

      if (firsthdr.GetAddr1 () == m_self)
        {
          //Iterate over all MPDUs and notify reception only if status OK
          for (; n != psdu->end (); ++n, ++status)
            {
              firsthdr = (*n)->GetHeader ();
              NS_ABORT_MSG_IF (firsthdr.GetAddr1 () != m_self, "All MPDUs of A-MPDU should have the same destination address");
              if (*status) //PER and thus CRC check succeeded
                {
                  if (psdu->IsSingle ())
                    {
                      //If the MPDU is sent as a VHT/HE single MPDU (EOF=1 in A-MPDU subframe header), then the responder sends an Ack.
                      NS_LOG_DEBUG ("Receive S-MPDU");
                      ampduSubframe = false;
                    }
                  else if (!m_sendAckEvent.IsRunning () && firsthdr.IsQosAck ()) // Implicit BAR Ack Policy
                    {
                      m_sendAckEvent = Simulator::Schedule (GetSifs (),
                                                            &MacLow::SendBlockAckAfterAmpdu, this,
                                                            firsthdr.GetQosTid (),
                                                            firsthdr.GetAddr2 (),
                                                            firsthdr.GetDuration (),
                                                            txVector, rxSnr);
                    }

                  if (firsthdr.IsAck () || firsthdr.IsBlockAck () || firsthdr.IsBlockAckReq ())
                    {
                      ReceiveOk ((*n), rxSnr, txVector, ampduSubframe);
                    }
                  else if (firsthdr.IsData () || firsthdr.IsQosData ())
                    {
                      NS_LOG_DEBUG ("Deaggregate packet from " << firsthdr.GetAddr2 () << " with sequence=" << firsthdr.GetSequenceNumber ());
                      ReceiveOk ((*n), rxSnr, txVector, ampduSubframe);
                      if (firsthdr.IsQosAck ())
                        {
                          NS_LOG_DEBUG ("Normal Ack");
                          normalAck = true;
                        }
                    }
                  else
                    {
                      NS_FATAL_ERROR ("Received A-MPDU with invalid first MPDU type");
                    }

                  if (!psdu->IsSingle ())
                    {
                      if (normalAck)
                        {
                          //send BlockAck
                          if (firsthdr.IsBlockAckReq ())
                            {
                              NS_FATAL_ERROR ("Sending a BlockAckReq with QosPolicy equal to Normal Ack");
                            }
                          uint8_t tid = firsthdr.GetQosTid ();
                          AgreementsI it = m_bAckAgreements.find (std::make_pair (firsthdr.GetAddr2 (), tid));
                          if (it != m_bAckAgreements.end ())
                            {
                              /* See section 11.5.3 in IEEE 802.11 for the definition of this timer */
                              ResetBlockAckInactivityTimerIfNeeded (it->second.first);
                              NS_LOG_DEBUG ("rx A-MPDU/sendImmediateBlockAck from=" << firsthdr.GetAddr2 ());
                              NS_ASSERT (m_sendAckEvent.IsRunning ());
                            }
                          else
                            {
                              NS_LOG_DEBUG ("There's not a valid agreement for this block ack request.");
                            }
                        }
                    }
                }
            }
        }
    }
  else
    {
      /* Simple MPDU */
      NS_ASSERT (!psdu->IsAggregate ());
      /* Check if the MPDU contains a Short SSW packet */
      if (txVector.GetMode ().GetModulationClass () == WIFI_MOD_CLASS_EDMG_CTRL && psdu->GetSize () == 6)
        ReceiveShortSswOk ((*psdu->begin ()), rxSnr, txVector, ampduSubframe);
      else
        ReceiveOk ((*psdu->begin ()), rxSnr, txVector, ampduSubframe);
    }
}

Time
MacLow::GetRemainingCfpDuration (void) const
{
  NS_LOG_FUNCTION (this);
  Time remainingCfpDuration = std::min (m_cfpStart, m_cfpStart + m_cfpMaxDuration - Simulator::Now () - m_cfpForeshortening);
  NS_ASSERT (remainingCfpDuration.IsPositive ());
  return remainingCfpDuration;
}

bool
MacLow::IsCfPeriod (void) const
{
  return (m_stationManager->GetPcfSupported () && m_cfpStart.IsStrictlyPositive ());
}

bool
MacLow::CanTransmitNextCfFrame (void) const
{
  NS_LOG_FUNCTION (this);
  if (!IsCfPeriod ())
    {
      return false;
    }
  NS_ASSERT (GetRemainingCfpDuration ().IsPositive ());
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_DATA);
  WifiMacTrailer fcs;
  uint32_t maxMacFrameSize = MAX_MSDU_SIZE + hdr.GetSerializedSize () + fcs.GetSerializedSize ();
  Time nextTransmission = 2 * m_phy->CalculateTxDuration (maxMacFrameSize, m_currentTxVector, m_phy->GetFrequency ()) + 3 * GetSifs () + m_phy->CalculateTxDuration (GetCfEndSize (), m_currentTxVector, m_phy->GetFrequency ());
  return ((GetRemainingCfpDuration () - nextTransmission).IsPositive ());
}

} //namespace ns3
