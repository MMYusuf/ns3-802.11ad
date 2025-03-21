
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/wifi-module.h"
#include "common-functions.h"
#include <iomanip>
#include <sstream>

/**
 * Simulation Output:
 * The simulation generates the following traces:
 * 1. PCAP traces for each station.
 * 2. SNR data for all the packets.
 * 3. Beamforming Traces.
 */

NS_LOG_COMPONENT_DEFINE ("Mobility");

using namespace ns3;
using namespace std;

/**  Application Variables **/
string applicationType = "bulk";          /* Type of the Tx application */
uint64_t totalRx = 0;
double throughput = 0;
double thr_update = 0.5;          /* throughput schedule time in seconds*/
Ptr<PacketSink> packetSink;
Ptr<OnOffApplication> onoff;
Ptr<BulkSendApplication> bulk;

/* Network Nodes */
Ptr<WifiNetDevice> apWifiNetDevice, staWifiNetDevice;
Ptr<DmgApWifiMac> apWifiMac;
Ptr<DmgStaWifiMac> staWifiMac;
Ptr<DmgWifiPhy> apWifiPhy, staWifiPhy;
Ptr<WifiRemoteStationManager> apRemoteStationManager, staRemoteStationManager;
NetDeviceContainer staDevices;

/*** Beamforming TXSS Schedulling ***/
uint16_t biThreshold = 5;                /* BI Threshold to trigger TXSS TXOP, 10=1s */
uint16_t biCounter;                       /* Number of beacon intervals that have passed. */

/* Flow monitor */
Ptr<FlowMonitor> monitor;

/* Statistics */
uint64_t macTxDataFailed = 0;
uint64_t transmittedPackets = 0;
uint64_t droppedPackets = 0;
uint64_t receivedPackets = 0;
bool csv = false;                         /* Enable CSV output. */

/* Tracing */
Ptr<QdPropagationEngine> qdPropagationEngine; /* Q-D Propagation Engine. */

void
CalculateThroughput (void)
{
  double thr = 0.1/thr_update * CalculateSingleStreamThroughput (packetSink, totalRx, throughput);
  if (!csv)
    {
      string duration = to_string_with_precision<double> (Simulator::Now ().GetSeconds () - thr_update, 1)
                      + " - " + to_string_with_precision<double> (Simulator::Now ().GetSeconds (), 1);
      std::cout << std::left << std::setw (12) << duration
                << std::left << std::setw (12) << thr
                << std::left << std::setw (12) << qdPropagationEngine->GetCurrentTraceIndex () << std::endl;
    }
  else
    {
      std::cout << std::setw (12) << to_string_with_precision<double> (Simulator::Now ().GetSeconds (), 1) 
                << thr << std::endl;
    }
  Simulator::Schedule (Seconds (thr_update), &CalculateThroughput);
}

void
SLSCompleted (Ptr<OutputStreamWrapper> stream, Ptr<SLS_PARAMETERS> parameters, SlsCompletionAttrbitutes attributes)
{
  *stream->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                        << qdPropagationEngine->GetCurrentTraceIndex () << ","
                        << uint16_t (attributes.sectorID) << "," << uint16_t (attributes.antennaID)  << ","
                        << parameters->wifiMac->GetTypeOfStation ()  << ","
                        << apWifiNetDevice->GetNode ()->GetId () + 1  << ","
                        << Simulator::Now ().GetNanoSeconds () << std::endl;
  if (!csv)
    {
      std::cout << "DMG STA " << parameters->wifiMac->GetAddress ()
                << " completed SLS phase with DMG STA " << attributes.peerStation << std::endl;
      std::cout << "Best Tx Antenna Configuration: AntennaID=" << uint16_t (attributes.antennaID)
                << ", SectorID=" << uint16_t (attributes.sectorID) << std::endl;
    }
}

void
MacRxOk (Ptr<OutputStreamWrapper> stream, WifiMacType, Mac48Address, double snrValue)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds () << "," << snrValue << std::endl;
}

void
StationAssoicated (Ptr<DmgWifiMac> staWifiMac, Mac48Address address, uint16_t aid)
{
  if (!csv)
    {
      std::cout << "DMG STA " << staWifiMac->GetAddress () << " associated with DMG PCP/AP " << address
                << ", Association ID (AID) = " << aid << std::endl;
    }
  if (applicationType == "onoff")
    {
      onoff->StartApplication ();
    }
  else
    {
      bulk->StartApplication ();
    }
}

void
DataTransmissionIntervalStarted (Ptr<DmgApWifiMac> apWifiMac, Ptr<DmgStaWifiMac> staWifiMac, Mac48Address address, Time)
{
  if (apWifiMac->GetWifiRemoteStationManager ()->IsAssociated (staWifiMac->GetAddress ()) > 0)
    {
      biCounter++;
      if (biCounter == biThreshold)
        {
          staWifiMac->Perform_TXSS_TXOP (address);
          biCounter = 0;
        }
    }
}

void
MacTxDataFailed (Mac48Address)
{
  macTxDataFailed++;
}

void
PhyTxEnd (Ptr<const Packet>)
{
  transmittedPackets++;
}

void
PhyRxDrop (Ptr<const Packet>, WifiPhyRxfailureReason)
{
  droppedPackets++;
}

void
PhyRxEnd (Ptr<const Packet>)
{
  receivedPackets++;
}

int
main (int argc, char *argv[])
{
  bool activateApp = true;                        /* Flag to indicate whether we activate OnOff/Bulk Application. */
  string socketType = "ns3::TcpSocketFactory";    /* Socket type (TCP/UDP). */
  uint32_t packetSize = 1448;                     /* Application payload size in bytes. */
  string dataRate = "300Mbps";                    /* Application data rate. */
  string tcpVariant = "NewReno";                  /* TCP Variant Type. */
  uint32_t bufferSize = 131072;                   /* TCP Send/Receive Buffer Size. */
  uint32_t maxPackets = 0;                        /* Maximum Number of Packets */
  string msduAggSize = "max";                     /* The maximum aggregation size for A-MSDU in Bytes. */
  string mpduAggSize = "max";                     /* The maximum aggregation size for A-MPDU in Bytes. */
  bool enableRts = false;                         /* Flag to indicate if RTS/CTS handskahre is enabled or disabled. */
  uint32_t rtsThreshold = 0;                      /* RTS/CTS handshare threshold. */
  string queueSize = "4000p";                     /* Wifi MAC Queue Size. */
  string phyMode = "DMG_MCS9";                   /* Type of the DMG physical layer. */
  bool enableMobility = true;                     /* Enable mobility. */
  bool verbose = false;                           /* Print logging information. */
  double simulationTime = 2.5;                     /* Simulation time in seconds. */
  double trace_int = 500;                         /* QD trace time interval in milliseconds*/
  double power_dBm = 39.0;                       /* Tx power in dBm */
  string directory = "";                          /* Path to the directory where to store the results. */
  bool pcapTracing = false;                       /* Flag to indicate if PCAP tracing is enabled or not. */
  string arrayConfig = "_SWIFT";                      /* Phased antenna array configuration. */

  /* Command line argument parser setup. */
  CommandLine cmd;
  cmd.AddValue ("thr_update", "Throughput schedule time in seconds", thr_update);
  cmd.AddValue ("activateApp", "Whether to activate data transmission or not", activateApp);
  cmd.AddValue ("applicationType", "Type of the Tx Application: onoff or bulk", applicationType);
  cmd.AddValue ("packetSize", "Application packet size in bytes", packetSize);
  cmd.AddValue ("dataRate", "Application data rate", dataRate);
  cmd.AddValue ("maxPackets", "Maximum number of packets to send", maxPackets);
  cmd.AddValue ("tcpVariant", TCP_VARIANTS_NAMES, tcpVariant);
  cmd.AddValue ("socketType", "Type of the Socket (ns3::TcpSocketFactory, ns3::UdpSocketFactory)", socketType);
  cmd.AddValue ("bufferSize", "TCP Buffer Size (Send/Receive) in Bytes", bufferSize);
  cmd.AddValue ("msduAggSize", "The maximum aggregation size for A-MSDU in Bytes", msduAggSize);
  cmd.AddValue ("mpduAggSize", "The maximum aggregation size for A-MPDU in Bytes", mpduAggSize);
  cmd.AddValue ("enableRts", "Enable or disable RTS/CTS handshake", enableRts);
  cmd.AddValue ("rtsThreshold", "The RTS/CTS threshold value", rtsThreshold);
  cmd.AddValue ("queueSize", "The maximum size of the Wifi MAC Queue", queueSize);
  cmd.AddValue ("phyMode", "802.11ad PHY Modse", phyMode);
  cmd.AddValue ("biThreshold", "BI Threshold to trigger beamforming training", biThreshold);
  cmd.AddValue ("power_dBm", "Tx powr in dBm", power_dBm);
  cmd.AddValue ("enableMobility", "Whether to enable mobility or simulate static scenario", enableMobility);
  cmd.AddValue ("verbose", "Turn on all WifiNetDevice log components", verbose);
  cmd.AddValue ("simulationTime", "Simulation time in seconds", simulationTime);
  cmd.AddValue ("trace_int", "QD trace simulation time interval in milliseconds", trace_int);
  cmd.AddValue ("directory", "Path to the directory where we store the results", directory);
  cmd.AddValue ("pcap", "Enable PCAP Tracing", pcapTracing);
  cmd.AddValue ("arrayConfig", "Antenna array configuration", arrayConfig);
  cmd.AddValue ("csv", "Enable CSV output instead of plain text.", csv);
  cmd.Parse (argc, argv);

  /* Validate A-MSDU and A-MPDU values */
  ValidateFrameAggregationAttributes (msduAggSize, mpduAggSize);
  /* Configure RTS/CTS and Fragmentation */
  ConfigureRtsCtsAndFragmenatation (enableRts, rtsThreshold);
  /* Wifi MAC Queue Parameters */
  ChangeQueueSize (queueSize);

  /*** Configure TCP Options ***/
  ConfigureTcpOptions (tcpVariant, packetSize, bufferSize);

  /**** DmgWifiHelper is a meta-helper ****/
  DmgWifiHelper wifi;

  /* Basic setup */
  wifi.SetStandard (WIFI_PHY_STANDARD_80211ad);

  /* Turn on logging */
  if (verbose)
    {
      wifi.EnableLogComponents ();
      LogComponentEnable ("Mobility", LOG_LEVEL_ALL);
    }

  /**** Setup mmWave Q-D Channel ****/
  Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel> ();
  qdPropagationEngine = CreateObject<QdPropagationEngine> ();
  qdPropagationEngine->SetAttribute ("QDModelFolder", StringValue ("DmgFiles/QdChannel/StreetCanyon/"));
  Ptr<QdPropagationLossModel> lossModelRaytracing = CreateObject<QdPropagationLossModel> (qdPropagationEngine);
  Ptr<QdPropagationDelayModel> propagationDelayRayTracing = CreateObject<QdPropagationDelayModel> (qdPropagationEngine);
  spectrumChannel->AddSpectrumPropagationLossModel (lossModelRaytracing);
  spectrumChannel->SetPropagationDelayModel (propagationDelayRayTracing);
  if (enableMobility)
    {
      qdPropagationEngine->SetAttribute ("Interval", TimeValue (MilliSeconds (trace_int)));
    }

  /**** Setup physical layer ****/
  SpectrumDmgWifiPhyHelper spectrumWifiPhy = SpectrumDmgWifiPhyHelper::Default ();
  spectrumWifiPhy.SetChannel (spectrumChannel);
  /* All nodes transmit at 39 dBm , no adaptation */
  spectrumWifiPhy.Set ("TxPowerStart", DoubleValue (power_dBm));
  spectrumWifiPhy.Set ("TxPowerEnd", DoubleValue (power_dBm));
  spectrumWifiPhy.Set ("TxPowerLevels", UintegerValue (1));
  /* Set the operational channel */
  spectrumWifiPhy.Set ("ChannelNumber", UintegerValue (2));
  /* Set default algorithm for all nodes to be constant rate */
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager", "DataMode", StringValue (phyMode));
  /* Make two nodes and set them up with the phy and the mac */
  NodeContainer wifiNodes;
  wifiNodes.Create (2);
  Ptr<Node> apWifiNode = wifiNodes.Get (0);
  Ptr<Node> staWifiNode = wifiNodes.Get (1);

  /* Add a DMG upper mac */
  DmgWifiMacHelper wifiMac = DmgWifiMacHelper::Default ();

  /* Install DMG PCP/AP Node */
  Ssid ssid = Ssid ("Mobility");
  wifiMac.SetType ("ns3::DmgApWifiMac",
                   "Ssid", SsidValue (ssid),
                   "BE_MaxAmpduSize", StringValue (mpduAggSize),
                   "BE_MaxAmsduSize", StringValue (msduAggSize),
                   "SSSlotsPerABFT", UintegerValue (8), "SSFramesPerSlot", UintegerValue (16),
                   "BeaconInterval", TimeValue (MicroSeconds (102400)));

  /* Set Parametric Codebook for the DMG AP */
wifi.SetCodebook ("ns3::CodebookParametric",
                    "FileName", StringValue ("DmgFiles/Codebook/URA_AP_63.txt"));

  /* Create Wifi Network Devices (WifiNetDevice) */
  NetDeviceContainer apDevice;
  apDevice = wifi.Install (spectrumWifiPhy, wifiMac, apWifiNode);

  wifiMac.SetType ("ns3::DmgStaWifiMac",
                   "Ssid", SsidValue (ssid), "ActiveProbing", BooleanValue (false),
                   "BE_MaxAmpduSize", StringValue (mpduAggSize),
                   "BE_MaxAmsduSize", StringValue (msduAggSize));

  /* Set Parametric Codebook for the DMG STA */
wifi.SetCodebook ("ns3::CodebookParametric",
                    "FileName", StringValue ("DmgFiles/Codebook/URA_STA_63.txt"));

  staDevices = wifi.Install (spectrumWifiPhy, wifiMac, staWifiNode);

  /* Setting mobility model */
  MobilityHelper mobility;
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (wifiNodes);

  /* Internet stack*/
  InternetStackHelper stack;
  stack.Install (wifiNodes);

  Ipv4AddressHelper address;
  address.SetBase ("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apInterface;
  apInterface = address.Assign (apDevice);
  Ipv4InterfaceContainer staInterfaces;
  staInterfaces = address.Assign (staDevices);

  /* We do not want any ARP packets */
  PopulateArpCache ();

  if (activateApp)
    {
      /* Install Simple Server on the DMG AP */
      PacketSinkHelper sinkHelper (socketType, InetSocketAddress (Ipv4Address::GetAny (), 9999));
      ApplicationContainer sinkApp = sinkHelper.Install (apWifiNode);
      packetSink = StaticCast<PacketSink> (sinkApp.Get (0));
      sinkApp.Start (Seconds (0.0));

      /* Install TCP/UDP Transmitter on the DMG STA */
      Address dest (InetSocketAddress (apInterface.GetAddress (0), 9999));
      ApplicationContainer srcApp;
      if (applicationType == "onoff")
        {
          OnOffHelper src (socketType, dest);
          src.SetAttribute ("MaxPackets", UintegerValue (maxPackets));
          src.SetAttribute ("PacketSize", UintegerValue (packetSize));
          src.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1e6]"));
          src.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
          src.SetAttribute ("DataRate", DataRateValue (DataRate (dataRate)));
          srcApp = src.Install (staWifiNode);
          onoff = StaticCast<OnOffApplication> (srcApp.Get (0));
        }
      else if (applicationType == "bulk")
        {
          BulkSendHelper src (socketType, dest);
          srcApp= src.Install (staWifiNode);
          bulk = StaticCast<BulkSendApplication> (srcApp.Get (0));
        }
      srcApp.Start (Seconds (simulationTime + 1));          ////////////////////////////////////////////////////
      srcApp.Stop (Seconds (simulationTime));
    }

  /* Enable Traces */
  if (pcapTracing)
    {
      spectrumWifiPhy.SetPcapDataLinkType (YansWifiPhyHelper::DLT_IEEE802_11_RADIO);
      spectrumWifiPhy.SetSnapshotLength (120);
      spectrumWifiPhy.EnablePcap ("Traces/AccessPoint", apDevice, false);
      spectrumWifiPhy.EnablePcap ("Traces/StaNode", staDevices.Get (0), false);
    }

  /* Stations */
  apWifiNetDevice = StaticCast<WifiNetDevice> (apDevice.Get (0));
  staWifiNetDevice = StaticCast<WifiNetDevice> (staDevices.Get (0));
  apRemoteStationManager = StaticCast<WifiRemoteStationManager> (apWifiNetDevice->GetRemoteStationManager ());
  apWifiMac = StaticCast<DmgApWifiMac> (apWifiNetDevice->GetMac ());
  staWifiMac = StaticCast<DmgStaWifiMac> (staWifiNetDevice->GetMac ());
  apWifiPhy = StaticCast<DmgWifiPhy> (apWifiNetDevice->GetPhy ());
  staWifiPhy = StaticCast<DmgWifiPhy> (staWifiNetDevice->GetPhy ());
  staRemoteStationManager = StaticCast<WifiRemoteStationManager> (staWifiNetDevice->GetRemoteStationManager ());

  /** Connect Traces **/
  Ptr<OutputStreamWrapper> outputSlsPhase = CreateSlsTraceStream (directory + "slsResults" + arrayConfig);

  /* DMG AP Straces */
  Ptr<SLS_PARAMETERS> parametersAp = Create<SLS_PARAMETERS> ();
  parametersAp->srcNodeID = apWifiNetDevice->GetNode ()->GetId ();
  parametersAp->dstNodeID = staWifiNetDevice->GetNode ()->GetId ();
  parametersAp->wifiMac = apWifiMac;
  apWifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("DTIStarted", MakeBoundCallback (&DataTransmissionIntervalStarted,
                                                                          apWifiMac, staWifiMac));
  apWifiPhy->TraceConnectWithoutContext ("PhyRxEnd", MakeCallback (&PhyRxEnd));
  apWifiPhy->TraceConnectWithoutContext ("PhyRxDrop", MakeCallback (&PhyRxDrop));

  /* DMG STA Straces */
  Ptr<SLS_PARAMETERS> parametersSta = Create<SLS_PARAMETERS> ();
  parametersSta->srcNodeID = staWifiNetDevice->GetNode ()->GetId ();
  parametersSta->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  parametersSta->wifiMac = staWifiMac;
  staWifiMac->TraceConnectWithoutContext ("Assoc", MakeBoundCallback (&StationAssoicated, staWifiMac));
  staWifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersSta));
  staWifiPhy->TraceConnectWithoutContext ("PhyTxEnd", MakeCallback (&PhyTxEnd));
  staRemoteStationManager->TraceConnectWithoutContext ("MacTxDataFailed", MakeCallback (&MacTxDataFailed));

  /* Get SNR Traces */
  AsciiTraceHelper ascii;
  Ptr<OutputStreamWrapper> snrStream = ascii.CreateFileStream (directory + "snrValues.csv");
  apRemoteStationManager->TraceConnectWithoutContext ("MacRxOK", MakeBoundCallback (&MacRxOk, snrStream));

  FlowMonitorHelper flowmon;
  if (activateApp)
    {
      /* Install FlowMonitor on all nodes */
      monitor = flowmon.InstallAll ();

      /* Print Output */
//      if (!csv)
//        {
          std::cout << std::left << std::setw (12) << "Time [s]"
                    << std::left << std::setw (12) << "Throughput [Mbps]" << std::endl;
//        }

      /* Schedule Throughput Calulcations */
      Simulator::Schedule (Seconds (thr_update), &CalculateThroughput);
    }

  Simulator::Stop (Seconds (simulationTime + 0.101));
  Simulator::Run ();
  Simulator::Destroy ();

  if (!csv)
    {
      if (activateApp)
        {
          /* Print Flow-Monitor Statistics */
          PrintFlowMonitorStatistics (flowmon, monitor, simulationTime);

          /* Print Application Layer Results Summary */
          std::cout << "\nApplication Layer Statistics:" << std::endl;;
          if (applicationType == "onoff")
            {
              std::cout << "  Tx Packets: " << onoff->GetTotalTxPackets () << std::endl;
              std::cout << "  Tx Bytes:   " << onoff->GetTotalTxBytes () << std::endl;
            }
          else
            {
              std::cout << "  Tx Packets: " << bulk->GetTotalTxPackets () << std::endl;
              std::cout << "  Tx Bytes:   " << bulk->GetTotalTxBytes () << std::endl;
            }
        }

      std::cout << "  Rx Packets: " << packetSink->GetTotalReceivedPackets () << std::endl;
      std::cout << "  Rx Bytes:   " << packetSink->GetTotalRx () << std::endl;
      std::cout << "  Throughput: " << packetSink->GetTotalRx () * 8.0 / (simulationTime * 1e6) << " Mbps" << std::endl;

      /* Print MAC Layer Statistics */
      std::cout << "\nMAC Layer Statistics:" << std::endl;;
      std::cout << "  Number of Failed Tx Data Packets:  " << macTxDataFailed << std::endl;

      /* Print PHY Layer Statistics */
      std::cout << "\nPHY Layer Statistics:" << std::endl;;
      std::cout << "  Number of Tx Packets:         " << transmittedPackets << std::endl;
      std::cout << "  Number of Rx Packets:         " << receivedPackets << std::endl;
      std::cout << "  Number of Rx Dropped Packets: " << droppedPackets << std::endl;
    }

  return 0;
}
