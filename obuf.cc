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
// The IBOutBuf implements an IB port FIFO
// See functional description in the header file.
//
#include "ib_m.h"
#include "obuf.h"

Define_Module( IBOutBuf );

void IBOutBuf::initialize()
{
  // read parameters
  qSize = par("size");
  maxVL = par("maxVL");

  Enabled = par("enabled");

  credMinTime_us = par("credMinTime");

  // Initiazlize the statistical collection elements
  qDepthHist.setName("Queue Usage");
  qDepthHist.setRangeAutoUpper(0, 10, 1);
  packetStoreHist.setName("Packet Storage Time");
  packetStoreHist.setRangeAutoUpper(0, 10, 1.5);
  flowControlDelay.setName("Time between VL0 FC");
  flowControlDelay.setRangeAutoUpper(0,10,1.2);
  qDepth.setName("Queue Depth");


  totalBytesSent = 0;
  firstPktSendTime = 0;
  flitsSources.setName("Flits Sources");

  // 4x 2.5Gbps = 1Byte/nsec ; but we need the 10/2.5/4.0 ...
  WATCH(credMinTime_us);

  p_popMsg = new omnetpp::cMessage("pop", IB_POP_MSG);

  // queue is empty
  prevPopWasDataCredit = 0;
  insidePacket = 0;
  prevFCTime = 0;
  isMinTimeUpdate = 0;

  for ( int i = 0; i < maxVL+1; i++ ) {
    prevSentFCTBS.push_back(-9999);
    prevSentFCCL.push_back(-9999);
    FCCL.push_back(0);
    FCTBS.push_back(0);
  }

  WATCH_VECTOR(prevSentFCTBS);
  WATCH_VECTOR(prevSentFCCL);
  WATCH_VECTOR(FCTBS);
  WATCH_VECTOR(FCCL);

  if (Enabled) {
    // we do want to have a continous flow of MinTime
    p_minTimeMsg = new omnetpp::cMessage("minTime", IB_MINTIME_MSG);
    
    // Send the first mintime immediately so that all is initialised 
    // when we get first packets
    scheduleAt(omnetpp::simTime() , p_minTimeMsg);
  } else {
	 EV << "-I- " << getFullPath() << " port DISABLED " << omnetpp::endl;
  }

  sent_BECN = 0;
  hcaOBuf = par("isHcaOBuf");
  on_throughput_obuf = par("on_throughput_obuf");
  timeStep_us = par("timeStep_us");
  BytesSentLastPeriod = 0;
  timerMsg = new omnetpp::cMessage("timeout", IB_TIMER_MSG);
  throughput.setName("Throughput-obuf");
} // initialize

// places a new allocated IBTQLoadUpdateMsg on the buffer
void IBOutBuf::sendOrQueuePortLoadUpdateMsg(unsigned int rank, unsigned int firstLid, unsigned int lastLid, int load) {
	Enter_Method("sendOrQueuePortLoadUpdateMsg lid-range:[%d,%d] load:%d",
			firstLid, lastLid, load);

	IBTQLoadUpdateMsg *p_msg = new IBTQLoadUpdateMsg("load-update", IB_TQ_LOAD_MSG);
	p_msg->setSrcRank(rank);
	p_msg->setLoad(load);
	p_msg->setFirstLid(firstLid);
	p_msg->setLastLid(lastLid);
	p_msg->setVL(0);
	p_msg->setByteLength(8);

	// if there is no other message on the wire sneak out
	if ( ! p_popMsg->isScheduled() ) {
		sendOutMessage(p_msg);
	} else {
	    EV << "-I- " << getFullPath() << " queued port-load msg. mgtQ depth " << mgtQ.length() << omnetpp::endl;
	    mgtQ.insert(p_msg);
	}
}

// send the message out
// Init a new pop message and schedule it after delay
// Note that at this stage the Q might be empty but a
// data packet will be streamed out
void IBOutBuf::sendOutMessage(IBWireMsg *p_msg) {
  
  EV << "-I- " << getFullPath() << " sending msg:" << p_msg->getName() 
     << " at time " << omnetpp::simTime() <<omnetpp::endl;

  // track out going packets
  if ( p_msg->getKind() == IB_DATA_MSG ) {
    IBDataMsg *p_dataMsg = (IBDataMsg *)p_msg;
    
    // track if we are in the middle of packet
    if (!p_dataMsg->getFlitSn() && (p_dataMsg->getPacketLength() > 1))
      insidePacket = 1;
    else if (p_dataMsg->getFlitSn() + 1 == p_dataMsg->getPacketLength())
      insidePacket = 0;
    
    FCTBS[p_msg->getVL()]++;

    flitsSources.collect(p_dataMsg->getSrcLid());


    if(p_dataMsg->getIsBECN() && !p_dataMsg->getIsFECN())
    {
      sent_BECN++;
    }
  }
  send(p_msg, "out");

  if (p_popMsg &&  ! p_popMsg->isScheduled() ) {
    scheduleAt(gate("out")->getTransmissionChannel()->getTransmissionFinishTime(), p_popMsg);
  }

  if (firstPktSendTime == 0)
  {
    firstPktSendTime = omnetpp::simTime();
    //if(hcaOBuf)
    //{
      if (!timerMsg->isScheduled() && on_throughput_obuf > 0) {
              //omnetpp::simtime_t delay = genDlyPerByte_ns*1e-9*flitSize_B;
          omnetpp::simtime_t delay = timeStep_us*1e-6;
          scheduleAt(omnetpp::simTime()+delay, timerMsg);
      }
    //}
  }

  totalBytesSent += p_msg->getByteLength();
  BytesSentLastPeriod += p_msg->getByteLength();

  //double oBW = totalBytesSent / (omnetpp::simTime() - firstPktSendTime);
  //throughput.record(oBW);

} // sendOutMessage

// Q a message to be sent out.
// If there is no pop message pending can directly send...
void
IBOutBuf::qMessage(IBDataMsg *p_msg) {
  // we stamp it to know how much time it stayed with us
  //p_msg->setTimestamp(simTime());
  
  if ( p_popMsg && p_popMsg->isScheduled() ) {
    if ( qSize <= queue.length() ) {
        error("-E- %s  need to insert into a full Q. qSize:%d qLength:%d",
                getFullPath().c_str(), qSize, queue.length());
    }
    
    EV << "-I- " << getFullPath() << " queued data msg:" << p_msg->getName()
       << " Qdepth " << queue.length() << omnetpp::endl;

    queue.insert(p_msg);
    //qDepth.record(queue.length());
  } else {
    // track the time this PACKET (all credits) spent in the Q
    // the last credit of a packet always
    if ( p_msg->getFlitSn() + 1 == p_msg->getPacketLength() ) {
      packetStoreHist.collect( omnetpp::simTime() - packetHeadTimeStamp );
    } else if ( p_msg->getFlitSn() == 0 ) {
      packetHeadTimeStamp = p_msg->getTimestamp();
    }
    sendOutMessage(p_msg);
  }
} // qMessage

// check if need to send a flow control and send it if required.
// return 1 if sent or 0 if not
// this function should be called by the pop event to check if flow control
// is required to be sent
// The minTime event only zeros out the curFlowCtrVL such that the operation 
// restarts.
// New Hermon mode provides extra cases where a flow control might be sent:
// 1. If there are no other messages in the Q 
//
// Also if there are messages in the Q we might not send FC unless the 
// difference is large enough
int IBOutBuf::sendFlowControl()
{
  static long flowCtrlId = 0;
  int sentUpdate = 0;

  // we should not continue if the Q is not empty if we aren't in mintime mode
  if (! isMinTimeUpdate && ! queue.empty())
    return(0);

  if (curFlowCtrVL >= maxVL+1) {
    return(0);
  }

  for (; (sentUpdate == 0) && (curFlowCtrVL < maxVL+1); curFlowCtrVL++ ) {
    int i = curFlowCtrVL;
    
    if (i == 0) {
      // avoid the first send...
      if (prevFCTime != 0)
        // flowControlDelay.collect(simTime() - prevFCTime);
        prevFCTime = omnetpp::simTime();
    }
    
    // We may have ignored prevSentFCTBS[i] == FCTBS[i] since the other side
    // tracks ABR but the spec asks us to send anyways
    if ( (prevSentFCTBS[i] != FCTBS[i]) || (prevSentFCCL[i] != FCCL[i]) ) {
      // create a new message and place in the Q
      char name[128];
      sprintf(name, "fc-%d-%ld", i, flowCtrlId++);
      IBFlowControl *p_msg = new IBFlowControl(name, IB_FLOWCTRL_MSG);
      
      p_msg->setBitLength(8*8);
      p_msg->setVL(i);
      p_msg->setFCCL(FCCL[i]);
      p_msg->setFCTBS(FCTBS[i]);
      prevSentFCCL[i] = FCCL[i];
      prevSentFCTBS[i] = FCTBS[i];
      EV << "-I- " << getFullPath() << " generated:" << p_msg->getName() 
         << " vl: " << p_msg->getVL() << " FCTBS: " 
         << p_msg->getFCTBS() << " FCCL: " << p_msg->getFCCL() << omnetpp::endl;
      
      // we do not need to Q as we are only called in pop
      sendOutMessage(p_msg);
      sentUpdate = 1;
    }

    // send management message if no FC sent
    if (!sentUpdate && !mgtQ.empty()) {
    	IBWireMsg *p_msg = (IBWireMsg*)mgtQ.pop();
    	EV << "-I- " << getFullPath() << " popped mgt message:"  << p_msg->getName() << omnetpp::endl;
    	sendOutMessage(p_msg);
    	sentUpdate = 1;
    }

    // last VL zeros the min time update flag only if there are no mgt messages
    if ( (curFlowCtrVL == maxVL+1) && mgtQ.empty())
      isMinTimeUpdate = 0;
  }

  return(sentUpdate);
}

// Handle Pop Message
// Should not be called if the Q is empty.
// Simply pop and schedule next pop.
// Also schedule a "free" message to be sent out later
void IBOutBuf::handlePop()
{
  cancelEvent(p_popMsg);

  // if we got a pop - it means the previous message just left the
  // OBUF. In that case if it was a data credit packet we have now a
  // new space for it. tell the VLA.
  if (prevPopWasDataCredit) {
    omnetpp::cMessage *p_msg = new omnetpp::cMessage("free", IB_FREE_MSG);
    EV << "-I- " << getFullPath() << " sending 'free' to VLA as last "
       << " packet just completed." << omnetpp::endl;
    send(p_msg, "free");
  }

  // first send mgt msg then try sending a flow control if required:
  if (!mgtQ.empty()) {
	  IBWireMsg *p_msg = (IBWireMsg*)mgtQ.pop();
	  EV << "-I- " << getFullPath() << " first pop mgt message:"  << p_msg->getName() << omnetpp::endl;
	  sendOutMessage(p_msg);
	  prevPopWasDataCredit = 0;
	  return;
  }

  if (!insidePacket) {
	  if (sendFlowControl()) {
		  prevPopWasDataCredit = 0;
		  return;
	  }
  }

  // got to pop from the queue if anything there
  if ( !queue.empty() ) {
    IBWireMsg *p_msg = (IBWireMsg *)queue.pop();
    if ( p_msg->getKind() == IB_DATA_MSG ) {
      IBDataMsg *p_cred = (IBDataMsg *)p_msg;
      EV << "-I- " << getFullPath() << " popped data message:" 
         << p_cred->getName() << omnetpp::endl;
      sendOutMessage(p_msg);
      
      // track the time this PACKET (all credits) spent in the Q
      // the last credit of a packet always
      if ( p_cred->getFlitSn() + 1 == p_cred->getPacketLength() ) {
        packetStoreHist.collect( omnetpp::simTime() - packetHeadTimeStamp );
      } else if ( p_cred->getFlitSn() == 0 ) {
        packetHeadTimeStamp = p_msg->getTimestamp();
      }

      // we just popped a real credit 
      prevPopWasDataCredit = 1;
    } else {
      EV << "-E- " << getFullPath() << " unknown message type to pop:"
         << p_msg->getKind() << omnetpp::endl;
    }
  } else {
    // The queue is empty. Next message needs to immediately pop
    // so we clean this event
    EV << "-I- " << getFullPath() << " nothing to POP" << omnetpp::endl;
    prevPopWasDataCredit = 0;
  }

  //qDepth.record(queue.length());
} // handlePop

// Handle MinTime:
// If the prev sent VL Credits are no longer valid send push an update
void IBOutBuf::handleMinTime()
{
  EV << "-I- " << getFullPath() << " handling MinTime event" << omnetpp::endl;
  curFlowCtrVL = 0;
  isMinTimeUpdate = 1;
  // if we do not have any pop message - we need to create one immediatly
  if (p_popMsg && ! p_popMsg->isScheduled() ) {
    scheduleAt(omnetpp::simTime() + 1e-9, p_popMsg);
  }
  
  // we use the min time to collect Queue depth stats:
  //qDepthHist.collect( queue.length() );

  scheduleAt(omnetpp::simTime() + credMinTime_us*1e-6, p_minTimeMsg);
} // handleMinTime
  
// Handle rxCred
void IBOutBuf::handleRxCred(IBRxCredMsg *p_msg)
{
  // update FCCL...
  FCCL[p_msg->getVL()] = p_msg->getFCCL();
  //delete p_msg;
  cancelAndDelete(p_msg);
}


void IBOutBuf::handleTimer(omnetpp::cMessage *p_msg)
{
  //record throughput
  if(on_throughput_obuf && p_msg->getKind() == IB_TIMER_MSG)
  {
    double oBW = BytesSentLastPeriod / (timeStep_us*1e-6);
    throughput.record(oBW * 8.0 * 1e-9);
    BytesSentLastPeriod = 0;

    omnetpp::simtime_t delay = timeStep_us*1e-6;
    scheduleAt(omnetpp::simTime()+delay, p_msg);
       
  }
}

void IBOutBuf::handleMessage(omnetpp::cMessage *p_msg)
{
  int msgType = p_msg->getKind();
  if ( msgType == IB_POP_MSG ) {
    handlePop();
  } else if ( msgType == IB_MINTIME_MSG ) {
    handleMinTime();
  } else if ( msgType == IB_DATA_MSG ) {
    qMessage((IBDataMsg*)p_msg);
  } else if ( msgType == IB_RXCRED_MSG ) {
    handleRxCred((IBRxCredMsg*)p_msg);
  } else if (msgType == IB_TIMER_MSG){
    handleTimer(p_msg);
  } 
  else {
    EV << "-E- " << getFullPath() << " do no know how to handle message:"
       << msgType << omnetpp::endl;
    delete p_msg;
  }
}

void IBOutBuf::finish()
{
  /* EV << "STAT: " << getFullPath() << " Data Packet Q time num/avg/max/std:"
     << packetStoreHist.getCount() << " / "
     << packetStoreHist.getMean() << " / "
     << packetStoreHist.getMax() << " / "
     << packetStoreHist.getStddev() << endl; 
     EV << "STAT: " << getFullPath() << " Q depth num/avg/max/std:"        
     << qDepthHist.getCount() << " / "
     << qDepthHist.getMean() << " / "
     << qDepthHist.getMax() << " / "
     << qDepthHist.getStddev() << endl;
     EV << "STAT: " << getFullPath() << " FlowControl Delay num/avg/max/std:"
     << flowControlDelay.getCount() << " / "
     << flowControlDelay.getMean() << " / "
     << flowControlDelay.getMax() << " / "
     << flowControlDelay.getStddev() << endl;
  */
	//double oBW = totalBytesSent / (omnetpp::simTime() - firstPktSendTime);
	//recordScalar("Output BW (Byte/Sec)", oBW);
	//flitsSources.record();
	// EV << "STAT: " << getFullPath() << " Flit Sources:" << endl << flitsSources.detailedInfo() << endl;
  //recordScalar("sent BECN",sent_BECN);
}

IBOutBuf::~IBOutBuf() {
  if (p_popMsg) cancelAndDelete(p_popMsg);
  if (timerMsg) cancelAndDelete(timerMsg);
  if (p_minTimeMsg) cancelAndDelete(p_minTimeMsg);
}
