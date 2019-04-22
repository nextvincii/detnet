/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
//
// Copyright (c) 2009 INESC Porto
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation;
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// Author: Gustavo J. A. M. Carneiro  <gjc@inescporto.pt> <gjcarneiro@gmail.com>
//

#include "flow-monitor.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/double.h"
#include <fstream>
#include <sstream>

#define PERIODIC_CHECK_INTERVAL (Seconds (1))

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("FlowMonitor");

NS_OBJECT_ENSURE_REGISTERED (FlowMonitor);

TypeId 
FlowMonitor::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::FlowMonitor")
    .SetParent<Object> ()
    .SetGroupName ("FlowMonitor")
    .AddConstructor<FlowMonitor> ()
    .AddAttribute ("MaxPerHopDelay", ("The maximum per-hop delay that should be considered.  "
                                      "Packets still not received after this delay are to be considered lost."),
                   TimeValue (Seconds (10.0)),
                   MakeTimeAccessor (&FlowMonitor::m_maxPerHopDelay),
                   MakeTimeChecker ())
    .AddAttribute ("StartTime", ("The time when the monitoring starts."),
                   TimeValue (Seconds (0.0)),
                   MakeTimeAccessor (&FlowMonitor::Start),
                   MakeTimeChecker ())
    .AddAttribute ("DelayBinWidth", ("The width used in the delay histogram."),
                   DoubleValue (0.001),
                   MakeDoubleAccessor (&FlowMonitor::m_delayBinWidth),
                   MakeDoubleChecker <double> ())
    .AddAttribute ("JitterBinWidth", ("The width used in the jitter histogram."),
                   DoubleValue (0.001),
                   MakeDoubleAccessor (&FlowMonitor::m_jitterBinWidth),
                   MakeDoubleChecker <double> ())
    .AddAttribute ("PacketSizeBinWidth", ("The width used in the packetSize histogram."),
                   DoubleValue (20),
                   MakeDoubleAccessor (&FlowMonitor::m_packetSizeBinWidth),
                   MakeDoubleChecker <double> ())
    .AddAttribute ("FlowInterruptionsBinWidth", ("The width used in the flowInterruptions histogram."),
                   DoubleValue (0.250),
                   MakeDoubleAccessor (&FlowMonitor::m_flowInterruptionsBinWidth),
                   MakeDoubleChecker <double> ())
    .AddAttribute ("FlowInterruptionsMinTime", ("The minimum inter-arrival time that is considered a flow interruption."),
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&FlowMonitor::m_flowInterruptionsMinTime),
                   MakeTimeChecker ())
  ;
  return tid;
}

TypeId 
FlowMonitor::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

FlowMonitor::FlowMonitor ()
  : m_enabled (false)
{
  // m_histogramBinWidth=DEFAULT_BIN_WIDTH;
}

void
FlowMonitor::DoDispose (void)
{
  for (std::list<Ptr<FlowClassifier> >::iterator iter = m_classifiers.begin ();
      iter != m_classifiers.end ();
      iter ++)
    {
      *iter = 0;
    }
  for (uint32_t i = 0; i < m_flowProbes.size (); i++)
    {
      m_flowProbes[i]->Dispose ();
      m_flowProbes[i] = 0;
    }
  Object::DoDispose ();
}

inline FlowMonitor::FlowStats&
FlowMonitor::GetStatsForFlow (FlowId flowId)
{
  FlowStatsContainerI iter;
  iter = m_flowStats.find (flowId);
  if (iter == m_flowStats.end ())
    {
      FlowMonitor::FlowStats &ref = m_flowStats[flowId];
      ref.delaySum = Seconds (0);
      ref.jitterSum = Seconds (0);
      ref.lastDelay = Seconds (0);
      ref.txBytes = 0;
      ref.rxBytes = 0;
      ref.txPackets = 0;
      ref.rxPackets = 0;
      ref.lostPackets = 0;
      ref.timesForwarded = 0;
      ref.delayHistogram.SetDefaultBinWidth (m_delayBinWidth);
      ref.jitterHistogram.SetDefaultBinWidth (m_jitterBinWidth);
      ref.packetSizeHistogram.SetDefaultBinWidth (m_packetSizeBinWidth);
      ref.flowInterruptionsHistogram.SetDefaultBinWidth (m_flowInterruptionsBinWidth);
      return ref;
    }
  else
    {
      return iter->second;
    }
}


void
FlowMonitor::ReportFirstTx (Ptr<FlowProbe> probe, uint32_t flowId, uint32_t packetId, uint32_t packetSize)
{
  if (!m_enabled)
    {
      return;
    }
  Time now = Simulator::Now ();
  TrackedPacket &tracked = m_trackedPackets[std::make_pair (flowId, packetId)];
  tracked.firstSeenTime = now;
  tracked.lastSeenTime = tracked.firstSeenTime;
  tracked.timesForwarded = 0;
  NS_LOG_DEBUG ("ReportFirstTx: adding tracked packet (flowId=" << flowId << ", packetId=" << packetId
                                                                << ").");

  probe->AddPacketStats (flowId, packetSize, Seconds (0));

  FlowStats &stats = GetStatsForFlow (flowId);
  stats.txBytes += packetSize;
  stats.txPackets++;
  if (stats.txPackets == 1)
    {
      stats.timeFirstTxPacket = now;
    }
  stats.timeLastTxPacket = now;
}


void
FlowMonitor::ReportForwarding (Ptr<FlowProbe> probe, uint32_t flowId, uint32_t packetId, uint32_t packetSize)
{
  if (!m_enabled)
    {
      return;
    }
  std::pair<FlowId, FlowPacketId> key (flowId, packetId);
  TrackedPacketMap::iterator tracked = m_trackedPackets.find (key);
  if (tracked == m_trackedPackets.end ())
    {
      NS_LOG_WARN ("Received packet forward report (flowId=" << flowId << ", packetId=" << packetId
                                                             << ") but not known to be transmitted.");
      return;
    }

  tracked->second.timesForwarded++;
  tracked->second.lastSeenTime = Simulator::Now ();

  Time delay = (Simulator::Now () - tracked->second.firstSeenTime);
  probe->AddPacketStats (flowId, packetSize, delay);
}


void
FlowMonitor::ReportLastRx (Ptr<FlowProbe> probe, uint32_t flowId, uint32_t packetId, uint32_t packetSize)
{
  if (!m_enabled)
    {
      return;
    }
  TrackedPacketMap::iterator tracked = m_trackedPackets.find (std::make_pair (flowId, packetId));
  if (tracked == m_trackedPackets.end ())
    {
      NS_LOG_WARN ("Received packet last-tx report (flowId=" << flowId << ", packetId=" << packetId
                                                             << ") but not known to be transmitted.");
      return;
    }

  Time now = Simulator::Now ();
  Time delay = (now - tracked->second.firstSeenTime);
  probe->AddPacketStats (flowId, packetSize, delay);

  FlowStats &stats = GetStatsForFlow (flowId);
  stats.delaySum += delay;
  stats.delayHistogram.AddValue (delay.GetSeconds ());
  if (stats.rxPackets > 0 )
    {
      Time jitter = stats.lastDelay - delay;
      if (jitter > Seconds (0))
        {
          stats.jitterSum += jitter;
          stats.jitterHistogram.AddValue (jitter.GetSeconds ());
        }
      else 
        {
          stats.jitterSum -= jitter;
          stats.jitterHistogram.AddValue (-jitter.GetSeconds ());
        }
    }
  stats.lastDelay = delay;

  stats.rxBytes += packetSize;
  stats.packetSizeHistogram.AddValue ((double) packetSize);
  stats.rxPackets++;
  if (stats.rxPackets == 1)
    {
      stats.timeFirstRxPacket = now;
    }
  else
    {
      // measure possible flow interruptions
      Time interArrivalTime = now - stats.timeLastRxPacket;
      if (interArrivalTime > m_flowInterruptionsMinTime)
        {
          stats.flowInterruptionsHistogram.AddValue (interArrivalTime.GetSeconds ());
        }
    }
  stats.timeLastRxPacket = now;
  stats.timesForwarded += tracked->second.timesForwarded;

  NS_LOG_DEBUG ("ReportLastTx: removing tracked packet (flowId="
                << flowId << ", packetId=" << packetId << ").");

  m_trackedPackets.erase (tracked); // we don't need to track this packet anymore
}

void
FlowMonitor::ReportDrop (Ptr<FlowProbe> probe, uint32_t flowId, uint32_t packetId, uint32_t packetSize,
                         uint32_t reasonCode)
{
  if (!m_enabled)
    {
      return;
    }

  probe->AddPacketDropStats (flowId, packetSize, reasonCode);

  FlowStats &stats = GetStatsForFlow (flowId);
  stats.lostPackets++;
  if (stats.packetsDropped.size () < reasonCode + 1)
    {
      stats.packetsDropped.resize (reasonCode + 1, 0);
      stats.bytesDropped.resize (reasonCode + 1, 0);
    }
  ++stats.packetsDropped[reasonCode];
  stats.bytesDropped[reasonCode] += packetSize;
  NS_LOG_DEBUG ("++stats.packetsDropped[" << reasonCode<< "]; // becomes: " << stats.packetsDropped[reasonCode]);

  TrackedPacketMap::iterator tracked = m_trackedPackets.find (std::make_pair (flowId, packetId));
  if (tracked != m_trackedPackets.end ())
    {
      // we don't need to track this packet anymore
      // FIXME: this will not necessarily be true with broadcast/multicast
      NS_LOG_DEBUG ("ReportDrop: removing tracked packet (flowId="
                    << flowId << ", packetId=" << packetId << ").");
      m_trackedPackets.erase (tracked);
    }
}

const FlowMonitor::FlowStatsContainer&
FlowMonitor::GetFlowStats () const
{
  return m_flowStats;
}

/// starts
// Time     timeFirstTxPacket;

// /// Contains the absolute time when the first packet in the flow
// /// was received by an end node, i.e. the time when the flow
// /// reception starts
// Time     timeFirstRxPacket;

// /// Contains the absolute time when the last packet in the flow
// /// was transmitted, i.e. the time when the flow transmission
// /// ends
// Time     timeLastTxPacket;

// /// Contains the absolute time when the last packet in the flow
// /// was received, i.e. the time when the flow reception ends
// Time     timeLastRxPacket;

// /// Contains the sum of all end-to-end delays for all received
// /// packets of the flow.
// Time     delaySum; // delayCount == rxPackets

// /// Contains the sum of all end-to-end delay jitter (delay
// /// variation) values for all received packets of the flow.  Here
// /// we define _jitter_ of a packet as the delay variation
// /// relatively to the last packet of the stream,
// /// i.e. \f$Jitter\left\{P_N\right\} = \left|Delay\left\{P_N\right\} - Delay\left\{P_{N-1}\right\}\right|\f$.
// /// This definition is in accordance with the Type-P-One-way-ipdv
// /// as defined in IETF \RFC{3393}.
// Time     jitterSum; // jitterCount == rxPackets - 1

// /// Contains the last measured delay of a packet
// /// It is stored to measure the packet's Jitter
// Time     lastDelay;

// /// Total number of transmitted bytes for the flow
// uint64_t txBytes;
// /// Total number of received bytes for the flow
// uint64_t rxBytes;
// /// Total number of transmitted packets for the flow
// uint32_t txPackets;
// /// Total number of received packets for the flow
// uint32_t rxPackets;


void
FlowMonitor::ClearFlowStats () {
	//FlowStatsContainerI iter;
//	uint32_t s = m_flowStats.size();
	std::map<FlowId, FlowMonitor::FlowStats> stats = m_flowStats;
  uint32_t s=1;
	for (std::map<FlowId, FlowMonitor::FlowStats>::iterator i = m_flowStats.begin (); i != m_flowStats.end (); ++i){
		// i->second.rxBytes=0;
		// i->second.rxPackets=0;
		// i->second.txBytes=0;
		// i->second.txPackets=0;
    m_flowStats[s].rxBytes=0;
    m_flowStats[s].rxPackets=1;
    m_flowStats[s].txBytes=0;
    m_flowStats[s].txPackets=1;
    m_flowStats[s].delaySum=Seconds (0);//m_flowStats[s].lastDelay;
    m_flowStats[s].jitterSum = Seconds (0);
    
    //m_flowStats[s].lastDelay = Seconds (0);
//    m_flowStats[s].delayHistogram.SetDefaultBinWidth (m_delayBinWidth);
//    m_flowStats[s].jitterHistogram.SetDefaultBinWidth (m_jitterBinWidth);
//    m_flowStats[s].packetSizeHistogram.SetDefaultBinWidth (m_packetSizeBinWidth);
//    m_flowStats[s].flowInterruptionsHistogram.SetDefaultBinWidth (m_flowInterruptionsBinWidth);
		//i->second.timeFirstTxPacket=Simulator::Now();
		//i->second.timeFirstRxPacket= Simulator::Now();
s++;
	}
//	while (s){
//		m_flowStats[s-1].rxBytes=60;
//		m_flowStats[s-1].rxPackets=1;
//		m_flowStats[s-1].txBytes=60;
//		m_flowStats[s-1].txPackets=1;
//		m_flowStats[s-1].timeFirstTxPacket=Simulator::Now();
//		m_flowStats[s-1].timeFirstRxPacket= Simulator::Now();
//
//		//m_flowStats[s-1].timeLastRxPacket=Simulator::Now();
//		//m_flowStats[s].timeLastTxPacket=Simulator::Now();
//		s--;
//	}
}

      // //FlowMonitor::FlowStats &ref = m_flowStats[flowId];
      // ref.delaySum = Seconds (0);
      // ref.jitterSum = Seconds (0);
      // ref.lastDelay = Seconds (0);
      // //ref.txBytes = 0;
      // //ref.rxBytes = 0;
      // //ref.txPackets = 0;
      // //ref.rxPackets = 0;
      // ref.lostPackets = 0;
      // ref.timesForwarded = 0;
      // ref.delayHistogram.SetDefaultBinWidth (m_delayBinWidth);
      // ref.jitterHistogram.SetDefaultBinWidth (m_jitterBinWidth);
      // ref.packetSizeHistogram.SetDefaultBinWidth (m_packetSizeBinWidth);
      // ref.flowInterruptionsHistogram.SetDefaultBinWidth (m_flowInterruptionsBinWidth);


void
FlowMonitor::CheckForLostPackets (Time maxDelay)
{
  Time now = Simulator::Now ();

  for (TrackedPacketMap::iterator iter = m_trackedPackets.begin ();
       iter != m_trackedPackets.end (); )
    {
      if (now - iter->second.lastSeenTime >= maxDelay)
        {
          // packet is considered lost, add it to the loss statistics
          FlowStatsContainerI flow = m_flowStats.find (iter->first.first);
          NS_ASSERT (flow != m_flowStats.end ());
          flow->second.lostPackets++;

          // we won't track it anymore
          m_trackedPackets.erase (iter++);
        }
      else
        {
          iter++;
        }
    }
}

void
FlowMonitor::CheckForLostPackets ()
{
  CheckForLostPackets (m_maxPerHopDelay);
}

void
FlowMonitor::PeriodicCheckForLostPackets ()
{
  CheckForLostPackets ();
  Simulator::Schedule (PERIODIC_CHECK_INTERVAL, &FlowMonitor::PeriodicCheckForLostPackets, this);
}

void
FlowMonitor::NotifyConstructionCompleted ()
{
  Object::NotifyConstructionCompleted ();
  Simulator::Schedule (PERIODIC_CHECK_INTERVAL, &FlowMonitor::PeriodicCheckForLostPackets, this);
}

void
FlowMonitor::AddProbe (Ptr<FlowProbe> probe)
{
  m_flowProbes.push_back (probe);
}


const FlowMonitor::FlowProbeContainer&
FlowMonitor::GetAllProbes () const
{
  return m_flowProbes;
}


void
FlowMonitor::Start (const Time &time)
{
  if (m_enabled)
    {
      return;
    }
  Simulator::Cancel (m_startEvent);
  m_startEvent = Simulator::Schedule (time, &FlowMonitor::StartRightNow, this);
}

void
FlowMonitor::Stop (const Time &time)
{
  if (!m_enabled)
    {
      return;
    }
  Simulator::Cancel (m_stopEvent);
  m_stopEvent = Simulator::Schedule (time, &FlowMonitor::StopRightNow, this);
}


void
FlowMonitor::StartRightNow ()
{
  if (m_enabled)
    {
      return;
    }
  m_enabled = true;
}


void
FlowMonitor::StopRightNow ()
{
  if (!m_enabled)
    {
      return;
    }
  m_enabled = false;
  CheckForLostPackets ();
}

void
FlowMonitor::AddFlowClassifier (Ptr<FlowClassifier> classifier)
{
  m_classifiers.push_back (classifier);
}

void
FlowMonitor::SerializeToXmlStream (std::ostream &os, uint16_t indent, bool enableHistograms, bool enableProbes)
{
  CheckForLostPackets ();

  os << std::string ( indent, ' ' ) << "<FlowMonitor>\n";
  indent += 2;
  os << std::string ( indent, ' ' ) << "<FlowStats>\n";
  indent += 2;
  for (FlowStatsContainerCI flowI = m_flowStats.begin ();
       flowI != m_flowStats.end (); flowI++)
    {
      os << std::string ( indent, ' ' );
#define ATTRIB(name) << " " # name "=\"" << flowI->second.name << "\""
      os << "<Flow flowId=\"" << flowI->first << "\""
      ATTRIB (timeFirstTxPacket)
      ATTRIB (timeFirstRxPacket)
      ATTRIB (timeLastTxPacket)
      ATTRIB (timeLastRxPacket)
      ATTRIB (delaySum)
      ATTRIB (jitterSum)
      ATTRIB (lastDelay)
      ATTRIB (txBytes)
      ATTRIB (rxBytes)
      ATTRIB (txPackets)
      ATTRIB (rxPackets)
      ATTRIB (lostPackets)
      ATTRIB (timesForwarded)
      << ">\n";
#undef ATTRIB

      indent += 2;
      for (uint32_t reasonCode = 0; reasonCode < flowI->second.packetsDropped.size (); reasonCode++)
        {
          os << std::string ( indent, ' ' );
          os << "<packetsDropped reasonCode=\"" << reasonCode << "\""
          << " number=\"" << flowI->second.packetsDropped[reasonCode]
          << "\" />\n";
        }
      for (uint32_t reasonCode = 0; reasonCode < flowI->second.bytesDropped.size (); reasonCode++)
        {
          os << std::string ( indent, ' ' );
          os << "<bytesDropped reasonCode=\"" << reasonCode << "\""
          << " bytes=\"" << flowI->second.bytesDropped[reasonCode]
          << "\" />\n";
        }
      if (enableHistograms)
        {
          flowI->second.delayHistogram.SerializeToXmlStream (os, indent, "delayHistogram");
          flowI->second.jitterHistogram.SerializeToXmlStream (os, indent, "jitterHistogram");
          flowI->second.packetSizeHistogram.SerializeToXmlStream (os, indent, "packetSizeHistogram");
          flowI->second.flowInterruptionsHistogram.SerializeToXmlStream (os, indent, "flowInterruptionsHistogram");
        }
      indent -= 2;

      os << std::string ( indent, ' ' ) << "</Flow>\n";
    }
  indent -= 2;
  os << std::string ( indent, ' ' ) << "</FlowStats>\n";

  for (std::list<Ptr<FlowClassifier> >::iterator iter = m_classifiers.begin ();
      iter != m_classifiers.end ();
      iter ++)
    {
      (*iter)->SerializeToXmlStream (os, indent);
    }

  if (enableProbes)
    {
      os << std::string ( indent, ' ' ) << "<FlowProbes>\n";
      indent += 2;
      for (uint32_t i = 0; i < m_flowProbes.size (); i++)
        {
          m_flowProbes[i]->SerializeToXmlStream (os, indent, i);
        }
      indent -= 2;
      os << std::string ( indent, ' ' ) << "</FlowProbes>\n";
    }

  indent -= 2;
  os << std::string ( indent, ' ' ) << "</FlowMonitor>\n";
}


std::string
FlowMonitor::SerializeToXmlString (uint16_t indent, bool enableHistograms, bool enableProbes)
{
  std::ostringstream os;
  SerializeToXmlStream (os, indent, enableHistograms, enableProbes);
  return os.str ();
}


void
FlowMonitor::SerializeToXmlFile (std::string fileName, bool enableHistograms, bool enableProbes)
{
  std::ofstream os (fileName.c_str (), std::ios::out|std::ios::binary);
  os << "<?xml version=\"1.0\" ?>\n";
  SerializeToXmlStream (os, 0, enableHistograms, enableProbes);
  os.close ();
}


} // namespace ns3

