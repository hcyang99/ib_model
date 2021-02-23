///////////////////////////////////////////////////////////////////////////
//
//         InfiniBand FLIT (Credit) Level OMNet++ Simulation Model
//
// Copyright (c) 2004-2013 Mellanox Technologies, Ltd. All rights reserved.
// This software is available to you under the terms of the GNU
// General Public License (GPL) Version 2, available from the file
// COPYING in the main directory of this source tree.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
///////////////////////////////////////////////////////////////////////////
//
// The IBSink implements an IB endport FIFO that is filled by the push 
// message and drained by the pop self-message.
// To simulate hiccups in PCI Exp bus, we schedule self-messages of type hiccup
// hiccup message alternate between an ON and OFF state. During ON state any
// drain message is ignored. On transition to OFF a new drain message can
// be generated
// 
#include "ib_m.h"
#include "sink.h"

Define_Module( IBSink );

void IBSink::initialize()
{
  waitStats.setName("Waiting time statistics");
  hiccupStats.setName("Hiccup Statistics");
  flags.setName("FECN/BECN flags");
  on_cc = par("on_cc");
  on_newcc = par("on_newcc");
  maxVL = par("maxVL");
  startStatCol_sec = par("startStatCol");
  lid = getParentModule()->par("srcLid");
  PakcetFabricTime.setName("Packet Fabric Time");
  PakcetFabricTime.setRangeAutoUpper(0, 10, 1.5);
  FECNRecvPackets.setName("Received FECN");
  VictimRecvPackets.setName("Received victim");
  RecvRateRecord.setName("Received Rate");

  // calculate the drain rate
  flitSize = par("flitSize");
  popDlyPerByte_ns = par("popDlyPerByte"); // PCIe drain rate
  WATCH(popDlyPerByte_ns);
 
  repFirstPackets = par("repFirstPackets");

  // we will allocate a drain message only on the first flit getting in
  // which is consumed immediately...
  p_drainMsg = new omnetpp::cMessage("pop", IB_POP_MSG);
  //p_pushFECNmsg = new IBPushFECNMsg("pushFECNmsg", IB_PUSHFECN_MSG);
  constexpr int MAX_LID = 1024;
  AccBytesRcv = 0;
  BECNRecv = 0;
  FECNRecv.resize(MAX_LID,0);
  //VictimRecv.resize(100,0);
  Recv.resize(MAX_LID,0);
  RecvRate = 0.0;
  PktRecvTime = 0;
  LastRecvTime = 0;
  FECNRecvTime.resize(MAX_LID,0);
  FirstRecvTime.resize(MAX_LID,0);
  temp.resize(100,1);

  
  duringHiccup = 0;
  WATCH(duringHiccup);
  
  p_hiccupMsg = new omnetpp::cMessage("pop");
  p_hiccupMsg->setKind(IB_HICCUP_MSG);
  scheduleAt(omnetpp::simTime()+1e-9, p_hiccupMsg);
  
  // we track number of packets per VL:
  VlFlits.resize(maxVL+1,0);
  WATCH_VECTOR(VlFlits);

  totOOOPackets = 0;
  totIOPackets = 0;
  totOOPackets = 0;
  oooPackets.setName("OOO-Packets");
  oooWindow.setName("OOO-Window-Pkts");
  msgLatency.setName("Msg-Network-Latency");
  msgF2FLatency.setName("Msg-First2First-Network-Latency");
  enoughPktsLatency.setName("Enough-Pkts-Network-Latency");
  enoughToLastPktLatencyStat.setName("Last-to-Enough-Pkt-Arrival");
  latency.setName("Latency of each message");
  largelatency.setName("latency of Large message");

  sinktimerMsg.resize(65,NULL);
  for (int i= 0; i < 65; i++)
  {
    sinktimerMsg.at(i) = new IBSinkTimerMsg("sink receive timeout", IB_SINKTIMER_MSG);
  } 
  periodT = 8.192;//us

  Recv_throughput = 0;
  throughput.setName("sink throughput");
  timeStep_us = par("timeStep_us");
  on_utilization = par("on_utilization");
  FirstSendCNPTime = 0;
  SendCNPTime = 0;
  cnpsent = 0;  
}

// Init a new drain message and schedule it after delay
void IBSink::newDrainMessage(double delay_us) 
{
  // we track the start time so we can hiccup left over...
  p_drainMsg->setTimestamp(omnetpp::simTime());
  scheduleAt(omnetpp::simTime()+delay_us*1e-6, p_drainMsg);
}

// track consumed messages and send "sent" event to the IBUF
void IBSink::consumeDataMsg(IBDataMsg *p_msg)
{
  if(PktRecvTime == 0)
  {
    PktRecvTime = omnetpp::simTime();
    recordScalar("first receive packet for Rtt", PktRecvTime);
  }
  EV << "-I- " << getFullPath() << " consumed data:" << p_msg->getName() << omnetpp::endl;

  // track the absolute time this packet was consumed
  lastConsumedPakcet = omnetpp::simTime();

  // track the time this flit waited in the HCA
  if (omnetpp::simTime() > startStatCol_sec) 
  {
	  waitStats.collect(lastConsumedPakcet - p_msg->getTimestamp());
	 
	 // track the time this flit spent on the wire...
	  if (p_msg->getFlitSn() == (p_msg->getPacketLength() -1))
    {
      PakcetFabricTime.collect(omnetpp::simTime() - p_msg->getTimestamp());
	  }
  }

  VlFlits.at(p_msg->getVL())++;

  IBSentMsg *p_sentMsg = new IBSentMsg("hca_sent", IB_SENT_MSG);
  p_sentMsg->setVL(p_msg->getVL());
  p_sentMsg->setWasLast(p_msg->getPacketLength() == p_msg->getFlitSn() + 1);
  if (!p_msg->getIsBECN())
  {
    EV << "-I- " << "finished packet" << p_msg->getMsgIdx() << ":" << packet_counter_ << omnetpp::endl;
  }
    
  if (p_sentMsg->getWasLast() && !p_msg->getIsBECN() && ++packet_counter_ == p_msg->getMsgLen())
  {
    packet_counter_ = 0;
    IBDoneMsg* d_msg = new IBDoneMsg(nullptr, IB_DONE_MSG);
    d_msg->setAppIdx(p_msg->getAppIdx());
    // if (p_msg->getDstLid() == 307)
    //   error("H[306] received msg "); 
    send(d_msg, "out");
  }
  send(p_sentMsg, "sent");
  delete p_msg;
}

void IBSink::handleData(IBDataMsg *p_msg)
{
  double delay_us;

  // make sure was correctly received (no routing bug)
  if (p_msg->getDstLid() != (int)lid) 
  {
    cModule* module = p_msg->getSenderModule();
    // cModule* hca = module->getParentModule(); variable declared not used
    //cModule* parent = this->getParentModule(); variable declared not used
	  error("-E- Received packet to %d while self lid is %d\nSender module: %s",
		p_msg->getDstLid() , lid, module->getFullName());
  }

  /* for congestion control
  1. if is BECN, generate PushBECN message to gen module
  2. delete the BECN data message

  for test now: just delete the BECN data message

  */

  if((on_cc || on_newcc) && p_msg->getIsBECN() && !p_msg->getIsFECN())
  {
    BECNRecv++;
    //latency.record(omnetpp::simTime() - p_msg->getInjectionTime());
    
    EV << "-I- " << getFullPath() << " received data with BECN mark:" << p_msg->getName() << omnetpp::endl;
    IBPushBECNMsg* p_pushBECNmsg = new IBPushBECNMsg("pushBECNmsg", IB_PUSHBECN_MSG);
    p_pushBECNmsg->setSrcLid(p_msg->getSrcLid());
    p_pushBECNmsg->setDstLid(p_msg->getDstLid());
    p_pushBECNmsg->setSL(p_msg->getSL());
    p_pushBECNmsg->setMsgIdx(p_msg->getMsgIdx());
    p_pushBECNmsg->setAppIdx(p_msg->getAppIdx());
    p_pushBECNmsg->setRecvRate(p_msg->getRecvRate());
    p_pushBECNmsg->setBECNValue(p_msg->getIsBECN());
    send(p_pushBECNmsg, "pushBECN");
    
    consumeDataMsg(p_msg);            // here !!! do not forget to consume data, or it will not free ibuf!!!!!!!!!
    return;
  }
  
  // for head of packet calculate out of order
  if (!p_msg->getFlitSn()) 
  {
	  if (lastPktSnPerSrc.find(p_msg->getSrcLid()) != lastPktSnPerSrc.end()) 
    {
		  unsigned int curSn = lastPktSnPerSrc.at(p_msg->getSrcLid());
		  if (p_msg->getPacketSn() == 1+curSn) 
      {
			  // OK case
			  lastPktSnPerSrc.at(p_msg->getSrcLid())++;
			  totIOPackets++;
		  } 
      else if (p_msg->getPacketSn() > 1+curSn) 
      {
			  // OOO was received
			  totOOPackets += 1 + p_msg->getPacketSn() - curSn;
			  //oooPackets.record(totOOOPackets);
			  lastPktSnPerSrc.at(p_msg->getSrcLid()) = p_msg->getPacketSn();
			  oooWindow.collect(p_msg->getPacketSn()-curSn);
		  } 
      else if (p_msg->getPacketSn() == curSn) 
      {
			  // this is a BUG! modified by yiran
		    if(!p_msg->getIsBECN())
		    {
		      error("-E- Received packet to %d from %d with PacketSn %d equal to previous Sn",
		      p_msg->getDstLid() , p_msg->getSrcLid(), p_msg->getPacketSn());
		    }
		  } 
      else 
      {
			  // Could not get here - A bug
			  error("BUG: IBSink::handleData unexpected relation of curSn %d and PacketSn %d", curSn, p_msg->getPacketSn());
		  }
	  } 
    else 
    {
		  lastPktSnPerSrc.insert({p_msg->getSrcLid(), p_msg->getPacketSn()});
		  totIOPackets++;
	  }
  }

  // calculate message latency - we track the "first" N packets of the message
  // we clean only all of them are received
  //std::map<MsgTupple, class OutstandingMsgData, MsgTuppleLess>::iterator mI;
  std::map<MsgTupple, class OutstandingMsgData/*, MsgTuppleLess*/>::iterator mI;


  // for first flits
  if (!p_msg->getFlitSn() && !p_msg->getIsBECN()) 
  {
	  MsgTupple mt(p_msg->getSrcLid(), p_msg->getAppIdx(), p_msg->getMsgIdx());
	  mI = outstandingMsgsData.find(mt);
	  if (mI == outstandingMsgsData.end()) 
    {
		  EV << "-I- " << getFullPath() << " received first flit of new message from src: "
			   <<  p_msg->getSrcLid() << " app:" << p_msg->getAppIdx() << " msg: " << p_msg->getMsgIdx() << omnetpp::endl;
		  outstandingMsgsData[mt].firstFlitTime = p_msg->getInjectionTime();
	  }

	  // first flit of the last packet
	  if (outstandingMsgsData.at(mt).numPktsReceived + 1 == (unsigned int)p_msg->getMsgLen()) 
    {
	    msgF2FLatency.collect((double)omnetpp::simTime().dbl() -  outstandingMsgsData.at(mt).firstFlitTime.dbl());
	  }
  }

  // can not use else here as we want to handle single flit packets
  if (p_msg->getFlitSn() == p_msg->getPacketLength() - 1 && !p_msg->getIsBECN()) 
  {
	  // last flit of a packet
	  MsgTupple mt(p_msg->getSrcLid(), p_msg->getAppIdx(), p_msg->getMsgIdx());
	  mI = outstandingMsgsData.find(mt);
	  if (mI == outstandingMsgsData.end()) 
    {
		  error("-E- Received last flit of packet from %d with no corresponding message record", p_msg->getSrcLid());
	  }
	  (*mI).second.numPktsReceived++;
	  EV << "-I- " << getFullPath() << " received last flit of packet: " << (*mI).second.numPktsReceived << " from src: "
	     <<  p_msg->getSrcLid() << " app:" << p_msg->getAppIdx() << " msg: " << p_msg->getMsgIdx() << omnetpp::endl;

	  // track the latency of the first num pkts of message
	  if (repFirstPackets) 
    {
		  if ( (*mI).second.numPktsReceived == repFirstPackets) 
      {
			  EV << "-I- " << getFullPath() << " received enough (" << repFirstPackets << ") packets for message from src: "
					 <<  p_msg->getSrcLid() << " app:" << p_msg->getAppIdx() << " msg: " << p_msg->getMsgIdx() << omnetpp::endl;
			  enoughPktsLatency.collect(omnetpp::simTime() - (*mI).second.firstFlitTime);
			  (*mI).second.enoughPktsLastFlitTime = omnetpp::simTime();
		  }
	  }

	  // clean completed messages
	  if ((*mI).second.numPktsReceived == (unsigned int)p_msg->getMsgLen()) 
    {
		  if (repFirstPackets) 
      {
			  enoughToLastPktLatencyStat.collect(omnetpp::simTime() - (*mI).second.enoughPktsLastFlitTime);
		  }
		  msgLatency.collect(omnetpp::simTime() - (*mI).second.firstFlitTime);
		  //yiran:
		  //std::cout<<p_msg->getSrcLid() <<" "<<p_msg->getFlitSn()<<" "<<omnetpp::simTime() - (*mI).second.firstFlitTime<<omnetpp::endl;
      if ((unsigned int)p_msg->getMsgLen() <= 2 )
      {
        latency.record(omnetpp::simTime() - (*mI).second.firstFlitTime);
      }
      if ((unsigned int)p_msg->getMsgLen() > 2)
      {
        largelatency.record(omnetpp::simTime() - (*mI).second.firstFlitTime);  
      }      
		  EV << "-I- " << getFullPath() << " received last flit of message from src: "
				 <<  p_msg->getSrcLid() << " app:" << p_msg->getAppIdx() << " msg: " << p_msg->getMsgIdx() << omnetpp::endl;
      //std::cout<<p_msg->getSrcLid() <<" "<<p_msg->getFlitSn()<<" "<<omnetpp::simTime() - (*mI).second.firstFlitTime<<omnetpp::endl;
		  outstandingMsgsData.erase(mt);
	  }
  }

  // for iBW calculations
  if (omnetpp::simTime() >= startStatCol_sec) 
  {
    AccBytesRcv += p_msg->getByteLength(); // p_msg->getBitLength()/8;
    LastRecvTime = omnetpp::simTime();
  }

  // we might be arriving on empty buffer:
  if (!p_drainMsg->isScheduled()) 
  {
    EV << "-I- " << getFullPath() << " data:" << p_msg->getName() << " arrived on empty FIFO" << omnetpp::endl;
    // this credit should take this time consume:
    delay_us = p_msg->getByteLength() * popDlyPerByte_ns*1e-3;
    newDrainMessage(delay_us);
  }

  EV << "-I- " << getFullPath() << " queued data:" << p_msg->getName() << omnetpp::endl;
  queue.insert(p_msg);

  //for congestion control
  /*
  1. check if cc is enabled and id FECN is set. if both yes, goto 2
  2. generate a pushFECN message to gen immediately, carry the srclid, dstlid, vl
  */

  bool sendRecvRate = false;
  double RecvRate = 0.0;
  int srclid = p_msg->getSrcLid();
  int BECNValue = 3;
  if((on_newcc || on_cc) /*&& i <= 2*/)
  {
    Recv.at(srclid)++;
    //FECNRecvTime: packet receive time. not only FECN
    FECNRecvTime.at(srclid) = FECNRecvTime.at(srclid)==0 ? omnetpp::simTime() : FECNRecvTime.at(srclid);
    if(p_msg->getIsFECN() == 1 && !p_msg->getIsBECN())
    {
      FECNRecv.at(srclid)++;  
    }
    omnetpp::simtime_t curtime = omnetpp::simTime();
    omnetpp::simtime_t interval = curtime - FECNRecvTime.at(srclid);
    //if(omnetpp::simTime() - FECNRecvTime[srclid] > 8.192*1e-6)
    if(interval * 1e6 >= 8.192)
    {   
    // sendRecvRate = true;
      if(Recv.at(srclid))
      {
        sendRecvRate = true;
        double fraction1 = (1.0 * FECNRecv.at(srclid)) / (1.0 * Recv.at(srclid));
        //double fraction2 = (1.0 * VictimRecv[srclid]) / (1.0 * Recv[srclid]);     
        if(fraction1 > 0.9)
        {
          //sendRecvRate = true;
          //RecvRate = Recv[srclid] * 2.048 * 8 /8.192;
          //RecvRate = Recv[srclid] * 2.048 * 8 / ((omnetpp::simTime() - FECNRecvTime[srclid])*1e6);
          RecvRate = Recv.at(srclid) * 2.048 * 8 / (interval*1e6);
          BECNValue = 1;
        }
        else
        {
          BECNValue = 3;
        }
        if(RecvRate >= 32)
        {
          sendRecvRate = false;
        }
        FECNRecv.at(srclid) = 0;
        //VictimRecv.at(srclid)= 0;
        Recv.at(srclid) = 0;
        FECNRecvTime.at(srclid) = curtime;  
      }
    }
    if(sendRecvRate && on_newcc)
    {
      IBPushFECNMsg* p_pushFECNmsg = new IBPushFECNMsg("pushFECNmsg", IB_PUSHFECN_MSG);
      FECNMsgSetup (p_pushFECNmsg,p_msg,RecvRate);
      //p_pushFECNmsg->setSL(p_msg->getSL());
      p_pushFECNmsg->setBECNValue(BECNValue);

      if(FirstSendCNPTime == 0)
      {
        FirstSendCNPTime = omnetpp::simTime();
      }
      SendCNPTime = omnetpp::simTime();
      cnpsent++;
    }  
  }
    /*if(on_newcc)
    {
      int i = p_msg->getSrcLid();
      if(i > 2)
      {
        return;
      }
      Recv[i]++;
      if(p_msg->getIsFECN() == 1 && !p_msg->getIsBECN())
      {
        FECNRecv[i]++;        
      }
      if(p_msg->getIsFECN() == 3 && !p_msg->getIsBECN())
      {
        //VictimRecv[i]++;        
      }
      if(FirstRecvTime[i] == 0)
      {
        FirstRecvTime[i] = omnetpp::simTime()*1e3;
        sinktimerMsg[i]->setSrcLid(i);
        omnetpp::simtime_t delay = periodT*1e-6;
        scheduleAt(omnetpp::simTime()+delay, sinktimerMsg[i]);
      }
    }*/
    /*if(i<=2)
    {
      int i = p_msg->getSrcLid();
      Recv_throughput++;
      if(FirstRecvTime[i] == 0)
      {
        FirstRecvTime[i] = omnetpp::simTime()*1e3;
        sinktimerMsg[i]->setSrcLid(i);
        omnetpp::simtime_t delay = 35*1e-6;
        scheduleAt(omnetpp::simTime()+delay, sinktimerMsg[i]);
      }
  }*/
  if(on_cc && p_msg->getIsFECN() == 1 && !p_msg->getIsBECN() /*&& i <= 2*/)
  {     
    //if(sendRecvRate == true)
    //{
    //FECNRecv++;
    //std::cout<<" received data with FECN mark:"<<omnetpp::endl;
    EV << "-I- " << getFullPath() << " received data with FECN mark:" << p_msg->getName() << omnetpp::endl;
    IBPushFECNMsg* p_pushFECNmsg = new IBPushFECNMsg("pushFECNmsg", IB_PUSHFECN_MSG);
    FECNMsgSetup (p_pushFECNmsg, p_msg,RecvRate);
    //p_pushFECNmsg->setSL(p_msg->getSL());
    p_pushFECNmsg->setBECNValue(1);
    //}    
    FirstSendCNPTime = FirstSendCNPTime == 0? omnetpp::simTime():FirstSendCNPTime;
    SendCNPTime = omnetpp::simTime();
    cnpsent++;
  }
}

// simply consume one message from the Q or stop the drain if Q is empty
// also under hiccup do nothing
void IBSink::handlePop(omnetpp::cMessage *p_msg) //parameter passed not used??
{
  // if we are under hiccup - do nothing or
  // got to pop from the queue if anything there
  if (!queue.isEmpty() && !duringHiccup) 
  {
    IBDataMsg *p_dataMsg = (IBDataMsg *)queue.pop();
    EV << "-I- " << getFullPath() << " De-queued data:" << p_dataMsg->getName() << omnetpp::endl;
    // when is our next pop event?
    double delay_ns = p_dataMsg->getByteLength() * popDlyPerByte_ns;
  
    // consume actually discards the message !!!
    consumeDataMsg(p_dataMsg);
    scheduleAt(omnetpp::simTime()+delay_ns*1e-9, p_drainMsg);
  } 
  else 
  {
    // The queue is empty. Next message needs to immediatly pop
    // so we clean the drain event
    EV << "-I- " << getFullPath() << " Nothing to POP" << omnetpp::endl;
    cancelEvent(p_drainMsg);
  }
}

// hickup really means we  drain and set another one.
void IBSink::handleHiccup(omnetpp::cMessage *p_msg) //parameter passed not used??
{
  omnetpp::simtime_t delay_us;

  if ( duringHiccup ) 
  {
    // we are inside a hiccup - turn it off and schedule next ON
    duringHiccup = 0;
    delay_us = par("hiccupDelay");
    EV << "-I- " << getFullPath() << " Hiccup OFF for:" 
       << delay_us << "usec" << omnetpp::endl;
    // as we are out of hiccup make sure we have at least one outstanding drain
    if (!p_drainMsg->isScheduled())
      newDrainMessage(1e-3); // 1ns
  } 
  else 
  {
    // we need to start a new hiccup
    duringHiccup = 1;
    delay_us = par("hiccupDuration");
    
    EV << "-I- " << getFullPath() << " Hiccup ON for:" << delay_us << "usec" << omnetpp::endl ;
  }

  hiccupStats.collect( omnetpp::simTime() );
  scheduleAt(omnetpp::simTime()+delay_us*1e-6, p_hiccupMsg);
}

void IBSink::handleMessage(omnetpp::cMessage *p_msg)
{
  switch (p_msg->getKind())
  {
    case 1  : handleData((IBDataMsg *)p_msg); break; //in the case of IB_DATA_MSG
    case 2  : EV << "-I- " << getFullPath() << " Dropping flow control message";
              delete p_msg; break; //in the case of IB_FLOWCTRL_MSG
    case 7  : handlePop(p_msg); break; //in the case of IB_POP_MSG
    case 8  : handleHiccup(p_msg); break; //in the case of IB_POP_MSG
    case 10 : delete p_msg; break; //in the case of IB_DONE_MSG
    case 19 : handleSinkTimer((IBSinkTimerMsg*)p_msg); break; //in the case of IB_SINKTIMER_MSG
    default : error("-E- %s does not know what to with msg: %d is local: %d"
              " senderModule: %s", getFullPath().c_str(), p_msg->getKind(), 
              p_msg->isSelfMessage(), p_msg->getSenderModule());
              delete p_msg;
  }
}

void IBSink::FECNMsgSetup (IBPushFECNMsg* FECNMsg, IBDataMsg *p_msg, double RecvRate)
{
  FECNMsg->setSrcLid(p_msg->getSrcLid());
  FECNMsg->setDstLid(p_msg->getDstLid());
  FECNMsg->setSL(0);
  FECNMsg->setMsgIdx(p_msg->getMsgIdx());
  FECNMsg->setAppIdx(p_msg->getAppIdx());
  FECNMsg->setRecvRate(RecvRate);
  send(FECNMsg, "pushFECN");
}

void IBSink::handleSinkTimer(IBSinkTimerMsg *p_msg)
{
  /*int srcLid = p_msg->getSrcLid();
  if(srcLid == 8)
  {
    RecvRateRecord.record(Recv[srcLid] * 2.048 * 8 / (periodT));
  }
    
  if(!Recv[srcLid])
  {
    RecvRateRecord.record(Recv[srcLid] * 2.048 * 8 / (periodT));
    omnetpp::simtime_t delay = periodT*1e-6;
    scheduleAt(omnetpp::simTime()+delay, sinktimerMsg[srcLid]);
    temp[srcLid]++;
    return;
  }
  double fraction1 = (1.0 * FECNRecv[srcLid]) / (1.0 * Recv[srcLid]);
    
  double RecvRate = 0;
  int BECNValue = 3;
  if(fraction1 > 0.95)
  {
    RecvRate = Recv[srcLid] * 2.048 * 8 / (periodT*temp[srcLid]);
    BECNValue = 1;
  }
  else
  {
    BECNValue = 3;
  }
  IBPushFECNMsg* p_pushFECNmsg = new IBPushFECNMsg("pushFECNmsg", IB_PUSHFECN_MSG);
  p_pushFECNmsg->setSrcLid(srcLid);
  p_pushFECNmsg->setDstLid(lid);
  p_pushFECNmsg->setSL(0);
  p_pushFECNmsg->setMsgIdx(0);
  p_pushFECNmsg->setAppIdx(0);
  p_pushFECNmsg->setRecvRate(RecvRate);
  p_pushFECNmsg->setBECNValue(BECNValue);
  send(p_pushFECNmsg, "pushFECN");

  FECNRecv[srcLid] = 0;
  //VictimRecv[srcLid]= 0;
  Recv[srcLid] = 0;
  temp[srcLid] = 1;

  omnetpp::simtime_t delay = periodT*1e-6;
  scheduleAt(omnetpp::simTime()+delay, sinktimerMsg[srcLid]);*/

  throughput.record((double)Recv_throughput*2048.0*8.0 / (timeStep_us*1e-6) / 1e9);
  omnetpp::simtime_t delay = timeStep_us*1e-6;
  Recv_throughput = 0;
  scheduleAt(omnetpp::simTime()+delay, sinktimerMsg.at(p_msg->getSrcLid()));
  delete p_msg;
}


void IBSink::finish()
{
  //char buf[128]; variable declared not used?? 
  //recordScalar("Time last packet consumed:", lastConsumedPakcet);
  //waitStats.record();
  PakcetFabricTime.record();
  msgLatency.record();
  msgF2FLatency.record();
  //enoughPktsLatency.record();
  //enoughToLastPktLatencyStat.record();

  //double iBW = AccBytesRcv / (omnetpp::simTime() - startStatCol_sec); variable declared not used?? 
  //recordScalar("Sink-BW-MBps", iBW/1e6);
  //oooWindow.record();
  //recordScalar("OO-IO-Packets-Ratio", 1.0*totOOPackets/totIOPackets);
  //recordScalar("Num-SRCs", lastPktSnPerSrc.size());
  lastPktSnPerSrc.clear();

  //recordScalar("Received-BECN", BECNRecv);
  //recordScalar("Received-FECN", FECNRecv);
  if(on_utilization)
  {
    double receive = AccBytesRcv * 8.0 /(LastRecvTime - PktRecvTime)/1e9;
    double fraction = receive/32.0;
    //double cnpfraction = cnpsent * 1024 * 8.0 / 32.0 / (SendCNPTime - FirstSendCNPTime)/1e9;
    recordScalar("link utilization",(double) AccBytesRcv * 8.0 /(LastRecvTime - PktRecvTime)/1e9 /32.0);
    std::cout<<"LastRecvTime: "<<LastRecvTime <<omnetpp::endl;
    //std::cout<<"cnpfraction: "<<cnpfraction <<" cnpsent: "<< cnpsent <<omnetpp::endl;
    if(cnpsent > 0)
    {
      recordScalar("CNP fraction", cnpsent * 1024 * 8.0 / 32.0 / (LastRecvTime - PktRecvTime)/1e9);
    }
  }
}

IBSink::~IBSink() 
{
	if (p_drainMsg)
	{
	  cancelAndDelete(p_drainMsg);
	}
	if(p_hiccupMsg)
	{
	  cancelAndDelete(p_hiccupMsg);
	}
  //if(on_newcc)
  //{
  for(int i= 0; i < 65; i++)
  {
    if(sinktimerMsg.at(i) != NULL)
    {
      delete sinktimerMsg.at(i);
    }
  }
  
  //}
  while (!queue.isEmpty()) 
  {
    IBDataMsg *p_dataMsg = (IBDataMsg *)queue.pop();
    if (p_dataMsg!=NULL)
      delete p_dataMsg;
  }
}