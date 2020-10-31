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
//
// The IBInBuf implements an IB Input Buffer
// See functional description in the header file.
//
#include "ib_m.h"
#include "ibuf.h"
#include "vlarb.h"
#include "vec_file.h"

Define_Module( IBInBuf );

long IBInBuf::getDoneMsgId()
{
  static long id = 0;
  return(id++);
}

void IBInBuf::parseIntListParam(char *parName, int numEntries, 
                                std::vector<int> &out) 
{
  int cnt = 0;
  const char *str = par(parName);
  char *tmpBuf = new char[strlen(str)+1];
  strcpy(tmpBuf, str);
  char *entStr = strtok(tmpBuf, " ,");
  while (entStr) {
    cnt++;
    out.push_back(atoi(entStr));
    entStr = strtok(NULL, " ,");
  }
  for (; cnt < numEntries; cnt++)
    out.push_back(0);
}

void IBInBuf::initialize()
{
  CreditLimit.setName("CreditLimit");
  lossyMode = par("lossyMode");
  numDroppedCredits = 0;
  WATCH(numDroppedCredits);
  maxVL = par("maxVL");
  
  Q = new omnetpp::cQueue*[gateSize("out")];
  for (int pn = 0; pn < gateSize("out"); pn++) {
    Q[pn] = new omnetpp::cQueue[maxVL+1];
  }
  maxBeingSent = par("maxBeingSent");
  numPorts = par("numPorts");
  totalBufferSize = par("totalBufferSize");      
  
  width = par("width");
  totalBufferSize = totalBufferSize*width/4;
  
  hcaIBuf = par("isHcaIBuf");
  if (hcaIBuf) {
    EV << "-I- " << getFullPath() << " is HCA IBuf" << omnetpp::endl;
    pktfwd = NULL;
  } else {
    EV << "-I- " << getFullPath() << " is Switch IBuf " << getId() <<  omnetpp::endl;
    Switch = getParentModule()->getParentModule();
    if (Switch == NULL) {
      error("Could not find parent Switch module");
    }
    pktfwd = dynamic_cast<Pktfwd*>(getSimulation()->getModule(Switch->findSubmodule("pktfwd")));
    if (pktfwd == NULL) {
      error("Could not find Packet FWDer");
    }
    ISWDelay = Switch->par("ISWDelay");
    portsnum_parent = Switch->par("numSwitchPorts");
  }

  // track how many parallel sends the IBUF do:
  numBeingSent = 0;
  WATCH(numBeingSent);
  
  // read Max Static parameters
  unsigned int totStatic = 0;
  int val;
  for (unsigned int vl = 0; vl < maxVL+1; vl++ ) {
    char parName[12];
    sprintf(parName,"maxStatic%d", vl);
    val = par(parName);
    val = val*width/4;
    maxStatic.push_back(val);
    totStatic += val;
  }
  
  if (totStatic > totalBufferSize) {
    error("-E- can not define total static (%d) > (%d) total buffer size", totStatic, totalBufferSize);
  }
  
  // Initiazlize the statistical collection elements
  for (unsigned int vl = 0; vl < maxVL+1; vl++ ) {
    char histName[40];
    sprintf(histName,"Used Static Credits for VL:%d", vl);
    staticUsageHist[vl].setName(histName);
    staticUsageHist[vl].setRangeAutoUpper(0, 10, 1);
  }
  
  // Initialize the data structures
  for (unsigned int vl = 0; vl < maxVL+1; vl++ ) {
    ABR.push_back(0);
    staticFree.push_back(maxStatic[vl]);
    sendRxCred(vl, 1e-9);
  }
  
  WATCH_VECTOR(ABR);
  WATCH_VECTOR(staticFree);
  usedStaticCredits.setName("static credits used");
  
  lastSendTime = 0;
  curPacketId  = 0;
  curPacketSrcLid = 0;
  curPacketCredits = 0;
  curPacketVL = -1;
  curPacketOutPort = -1;

  gate("in")->setDeliverOnReceptionStart(true);

  thisPortNum = getParentModule()->getIndex();

  inputQueueLength.setName("inputQueueLength");

  marknum = 0;
  BECNRecv = 0;
  FECNRecv = 0;
  markrate = 5;


  p_popMsg = NULL;
  p_minTimeMsg = NULL;



} // init

int IBInBuf::incrBusyUsedPorts() {
  if (numBeingSent < maxBeingSent) {
    numBeingSent++;
    EV << "-I- " << getFullPath() << " increase numBeingSent to:"
       << numBeingSent<< omnetpp::endl;
    return 1;
  }
  EV << "-I- " << getFullPath() << " already sending:"<< numBeingSent<< omnetpp::endl;
  return 0;
};

// calculate FCCL and send to the OBUF
void IBInBuf::sendRxCred(int vl, double delay = 0)
{
  IBRxCredMsg *p_msg = new IBRxCredMsg("rxCred", IB_RXCRED_MSG);
  p_msg->setVL(vl);
  if (!lossyMode) {
	 p_msg->setFCCL(ABR[vl] + staticFree[vl]);
   //CreditLimit.record(ABR[vl] + staticFree[vl]);
  } else {
	 p_msg->setFCCL(ABR[vl] + maxStatic[vl]);
  }
  
  if (delay)
    sendDelayed(p_msg, delay, "rxCred");
  else
    send(p_msg, "rxCred");
}

// Forward the FCCL received in flow control packet to the VLA
void IBInBuf::sendTxCred(int vl, long FCCL)
{
  IBTxCredMsg *p_msg = new IBTxCredMsg("txCred", IB_TXCRED_MSG);
  p_msg->setVL(vl);
  p_msg->setFCCL(FCCL);
  send(p_msg, "txCred"); 
}

// Try to send the HoQ to the VLA
void IBInBuf::updateVLAHoQ(short int portNum, short vl)
{
  if (Q[portNum][vl].empty()) return;
  
  // find the VLA connected to the given port and
  // call its method for checking and setting HoQ
  omnetpp::cGate *p_gate = gate("out", portNum)->getPathEndGate();
  if (! hcaIBuf) {
    int remotePortNum = p_gate->getIndex();
    IBVLArb *p_vla = dynamic_cast<IBVLArb *>(p_gate->getOwnerModule());
    if ((p_vla == NULL) || strcmp(p_vla->getName(), "vlarb")) {
      error("-E- fail to get VLA from out port: %d", portNum);
    }
    if (! p_vla->isHoQFree(remotePortNum, vl))
      return;
    
    EV << "-I- " << getFullPath() << " free HoQ on VLA:"
       << p_vla->getFullPath() << " port:"
       << remotePortNum << " vl:" << vl << omnetpp::endl;
  }
 
  IBDataMsg *p_msg = (IBDataMsg *)Q[portNum][vl].pop();
  
  if (!hcaIBuf) {
    // Add the latency only if not in cut through mode
    // also may be required if the last delivery time is too close
    // and we must insert delay to avoid reordering
    omnetpp::simtime_t storeTime = omnetpp::simTime() - p_msg->getArrivalTime();
    omnetpp::simtime_t extraStoreTime = ISWDelay*1e-9 - storeTime;
    if (omnetpp::simTime() + extraStoreTime <= lastSendTime) {
      extraStoreTime = lastSendTime + 1e-9 - omnetpp::simTime();
    }
    if (extraStoreTime > 0) {
      lastSendTime = omnetpp::simTime()+extraStoreTime;
      sendDelayed(p_msg, extraStoreTime, "out", portNum);
    } else {
      lastSendTime = omnetpp::simTime();
      send(p_msg, "out", portNum);
    }
  } else {
    send(p_msg, "out", portNum);
  }
}

// Handle Push message
void IBInBuf::handlePush(IBWireMsg *p_msg)
{
  int msgType = p_msg->getKind();
  if (msgType == IB_FLOWCTRL_MSG) {
    // FlowControl:
    // * The FCCL is delivered through the TxCred to the VLA
    // * ABR is overwritten with FCTBS (can cause RxCred send to OBUF
    //   FCCL = ABR + FREE is provided to the OBUF through the RxCred)
    IBFlowControl *p_flowMsg = (IBFlowControl *)p_msg;
    int vl = p_flowMsg->getVL();
    
    EV << "-I- " << getFullPath() << " received flow control message:"
       << p_flowMsg->getName() << " vl:" << vl
       << " FCTBS:" << p_flowMsg->getFCTBS()
       << " FCCL:" << p_flowMsg->getFCCL() << omnetpp::endl;
    
    sendTxCred(vl, p_flowMsg->getFCCL());
    
    // update ABR and send RxCred
    if (ABR[vl] > p_flowMsg->getFCTBS()) {
      EV << "-E- " << getFullPath() << " how come we have ABR:" << ABR[vl]
         << " > wire FCTBS" << p_flowMsg->getFCTBS() << "?" << omnetpp::endl;
    } else if (ABR[vl] < p_flowMsg->getFCTBS()) {
      EV << "-W- " << getFullPath() << " how come we have ABR:" << ABR[vl]
         << " < wire FCTBS" << p_flowMsg->getFCTBS()
         << " in lossles wires?" << omnetpp::endl;
      ABR[vl] = p_flowMsg->getFCTBS();
    }
    
    sendRxCred(vl);
    cancelAndDelete(p_msg);
  } else if (msgType == IB_DATA_MSG) {
    // Data Packet:
    IBDataMsg *p_dataMsg = (IBDataMsg *)p_msg;

    if(p_dataMsg->getIsFECN() && !p_dataMsg->getIsBECN())
    {
      FECNRecv++;
    }
    if(!p_dataMsg->getIsFECN() && p_dataMsg->getIsBECN())
    {
      BECNRecv++;
    }
    
    // track the time of the packet in the switch
    p_dataMsg->setSwTimeStamp(omnetpp::simTime());

    if (p_dataMsg->getFlitSn() == 0) {
      curPacketId = p_dataMsg->getPacketId();
      curPacketSrcLid = p_dataMsg->getSrcLid();
      curPacketName = p_dataMsg->getName();
      curPacketCredits = p_dataMsg->getPacketLength();
      curPacketVL = p_dataMsg->getVL();
      unsigned short dLid = p_dataMsg->getDstLid();
      unsigned short sLid = p_dataMsg->getSrcLid();
      
      if (dLid == 0) {
        error("Error: dLid should not be 0 for %s", p_dataMsg->getName());
      }
      
      if ((curPacketVL < 0) || (curPacketVL > (int)maxVL+1)) {
        error("VL out of range: %d", curPacketVL);
      }
      
      // do we have enough credits?
		if (!lossyMode) {
		  if (curPacketCredits > staticFree[curPacketVL]) {
			  error(" Credits overflow. Required: %d available: %d",curPacketCredits, staticFree[curPacketVL]);
		  }

		} else {
		  // we need to mark out port as -1 to make next flits drop
		  if (curPacketCredits > staticFree[curPacketVL]) {
			 curPacketOutPort = -1;
			 numDroppedCredits += curPacketCredits;
			 delete p_msg;
			 return;
		  }
		} 
      
      // lookup out port  on the first credit of a packet
      if (numPorts > 1) {
    	  if (!hcaIBuf) {
    		  curPacketOutPort = pktfwd->getPortByLID(sLid,dLid);
			  if (!p_dataMsg->getBeforeAnySwitch() &&
					(curPacketOutPort == (int)thisPortNum)) {
				 error("loopback ! packet %s from lid:%d to dlid %d is sent back throgh port: %d ", p_dataMsg->getName(), p_dataMsg->getSrcLid(),p_dataMsg->getDstLid(),curPacketOutPort);
			  }
    	  } else {
    		  curPacketOutPort = 0;
    	  }

		  // this is an error flow we need to pass the current message
		  // to /dev/null
    	  if (curPacketOutPort < 0) {
    		  curPacketOutPort = -1;
    	  } else {
    		  // get the current inbuf index in the switch
    		  pktfwd->repQueuedFlits(thisPortNum, curPacketOutPort, p_dataMsg->getDstLid(), curPacketCredits);
    	  }
      } else {
        curPacketOutPort = 0;
      }
    } else {
      // Continuation Credit 
      
      // check the packet is the expected one:
      if ((curPacketId != p_dataMsg->getPacketId()) ||
          (curPacketSrcLid != p_dataMsg->getSrcLid())) {
        error("got unexpected packet: %s from:%d id: %d "
                  "during packet: %s from: %d %d",
                  p_dataMsg->getName(), 
                  p_dataMsg->getSrcLid(), p_dataMsg->getPacketId(), 
                  curPacketName.c_str(), curPacketSrcLid, curPacketId);
      }
    }

	 // mark the packet as after first switch
	 if (!hcaIBuf)
		p_dataMsg->setBeforeAnySwitch(false);
    
    // check out port is valid
    if ((curPacketOutPort < 0) ||  (curPacketOutPort >= (int)numPorts) ) {
      EV << "-E- " << getFullPath() << " dropping packet:" << p_dataMsg->getName() << " by FDB mapping to port:" << curPacketOutPort << omnetpp::endl;
      cancelAndDelete(p_dataMsg);
      return;
    }
    
    // Now consume a credit
	 staticFree[curPacketVL]--;
    staticUsageHist[curPacketVL].collect(staticFree[curPacketVL]);
    ABR[curPacketVL]++;
    EV << "-I- " << getFullPath() << " New Static ABR[" 
       << curPacketVL << "]:" << ABR[curPacketVL] << omnetpp::endl;
    EV << "-I- " << getFullPath() << " static queued msg:" 
       << p_dataMsg->getName() << " vl:" << curPacketVL
       << ". still free:" << staticFree[curPacketVL] << omnetpp::endl;

    

    /*
    congestion control marking FECN
    */   
    double totallength = 0;
    int congnum = 0;
    double fraction = 0;
   
    if(!hcaIBuf)
    {
      congnum = 0;
      for(int i = 0; i < portsnum_parent; i++)
      {
        char path[40];
        sprintf(path,"^.^.subport[%d].ibuf",i);
        IBInBuf* otheribuf = dynamic_cast<IBInBuf*>(getModuleByPath(path));
        totallength += otheribuf->Q[curPacketOutPort][curPacketVL].length(); 
        if(otheribuf->Q[curPacketOutPort][curPacketVL].length())
        {
          congnum++;
          fraction = fraction + otheribuf->Q[curPacketOutPort][curPacketVL].length()/32.0;
          //std::cout<<curPacketOutPort<<" "<< i<<" "<<otheribuf->Q[curPacketOutPort][curPacketVL].length()<<omnetpp::endl;
        }
      }
       //totallength = Q[curPacketOutPort][curPacketVL].length();
       //inputQueueLength.record(totallength);
       //inputQueueLength.record(Q[curPacketOutPort][curPacketVL].length());
      
       
      if(p_dataMsg->getSrcLid() == 2)
       {
        //totallength = Q[curPacketOutPort][curPacketVL].length();
         //inputQueueLength.record(totallength);
         //std::cout<<fraction<<" "<<congnum<<" "<<omnetpp::endl;
       }  
     
    }
    //StaticFreeNum.record(staticFree[curPacketVL]);
    
    //if(! hcaIBuf && congnum && totallength/(congnum*32) > 0.1 && Q[curPacketOutPort][curPacketVL].length())
    if(!hcaIBuf && totallength)
    //fraction = totallength/32.0*congnum;
    //if(!hcaIBuf && fraction)
    {

      if(!p_dataMsg->getIsFECN()&& !p_dataMsg->getIsBECN() && p_dataMsg->getFlitSn()== 0)
      {
        //inputQueueLength.record(totallength / 32);
        //std::cout<<"mark! "<< totallength << omnetpp::endl;
        p_dataMsg->setIsFECN(2);
        
      }
      //inputQueueLength.record(p_dataMsg->getSrcLid());
    }
    
    

    




    // For every DATA "credit" (not only first one)
    // - Queue the Data in the Q[V]
    Q[curPacketOutPort][curPacketVL].insert(p_dataMsg);
    
    // - Send RxCred with updated ABR[VL] and FREE[VL] - only if the sum has
    //   changed which becomes the FCCL of the sent flow control
    sendRxCred(curPacketVL);
    
    // - If HoQ in the target VLA is empty - send the push event out.
    //   when the last packet is sent the "done" event has to be sent to 
    //   all output ports, Note this also dequeue and send
    updateVLAHoQ(curPacketOutPort, curPacketVL);
  } else {
    EV << "-E- " << getFullPath() << " push does not know how to handle message:"
       << msgType << omnetpp::endl;
    cancelAndDelete(p_msg);
  }
}

// simple free static credits as reqired
void IBInBuf::simpleCredFree(int vl)
{
  // simply return the static credit first
  if (staticFree[vl] < maxStatic[vl]) {
    staticFree[vl]++;
    // need to update the OBUF we have one free... 
    sendRxCred(vl);
  } else {
    error("Error: got a credit leak? trying to add credits to full buffer on vl  %d", vl);
  }
}

// Handle Sent Message
// A HoQ was sent by the VLA
void IBInBuf::handleSent(IBSentMsg *p_msg)
{
  // first calculate the total used static
  int totalUsedStatics = 0;
  for (unsigned int vli = 0; vli < maxVL+1; vli++) {
    totalUsedStatics += maxStatic[vli] - staticFree[vli]; 
  }
  
  //usedStaticCredits.record( totalUsedStatics );
  
  // update the free credits accordingly:
  int vl = p_msg->getVL();

  simpleCredFree(vl);
  
  // Only on switch ibuf we need to do the following...
  if (! hcaIBuf) {
	// update the outstanding flits for this out-port
	// HACK: assume the port index is the port num that is switch connectivity is N x N following port idx
	pktfwd->repQueuedFlits(thisPortNum, p_msg->getArrivalGate()->getIndex(), 0, -1);

    // if this was the last message we need to schedule a "done"
    // on each of the output ports
    if (p_msg->getWasLast()) {
      // first we decrement the number of outstanding sends
      if (numBeingSent <= 0) {
        EV << "-E- " << getFullPath() << " got last message when numBeingSent:"
           << numBeingSent << omnetpp::endl;
        EV.flush();
        exit(1);
      }
      
      numBeingSent--;
      EV << "-I- " << getFullPath() << " completed send. down to:" 
         << numBeingSent << " sends" << omnetpp::endl;

      // inform all arbiters we drive
      int numOutPorts = gateSize("out");
      for (int pn = 0; pn < numOutPorts; pn++) {
        char name[32];
        sprintf(name,"done-%ld",getDoneMsgId());
        IBDoneMsg *p_doneMsg = new IBDoneMsg(name, IB_DONE_MSG);
        send(p_doneMsg, "out", pn);
      }
    }
    
    // if the data was sent we can expect the HoQ to be empty...
    updateVLAHoQ(p_msg->getArrivalGate()->getIndex(), p_msg->getVL());
  }
  
  cancelAndDelete(p_msg);
}

void IBInBuf::handleTQLoadMsg(IBTQLoadUpdateMsg *p_msg)
{
	if (!hcaIBuf) {
		unsigned int firstLid = p_msg->getFirstLid();
		unsigned int lastLid = p_msg->getLastLid();
		unsigned int srcRank = p_msg->getSrcRank();
		int load= p_msg->getLoad();
		delete p_msg;
		pktfwd->handleTQLoadMsg(getParentModule()->getIndex(), srcRank, firstLid, lastLid, load);
	} else {
		//delete p_msg;
    if (p_msg->isSelfMessage())
      cancelAndDelete(p_msg);
    else
      delete p_msg;
	}
}

void IBInBuf::handleMessage(omnetpp::cMessage *p_msg)
{
    int msgType = p_msg->getKind();
    if ( msgType == IB_SENT_MSG ) {
        handleSent((IBSentMsg *)p_msg);
    } else if ( (msgType == IB_DATA_MSG) || (msgType == IB_FLOWCTRL_MSG) ) {
        handlePush((IBWireMsg*)p_msg);
    } else if (msgType == IB_TQ_LOAD_MSG) {
        handleTQLoadMsg((IBTQLoadUpdateMsg*)p_msg);
    } else {
        EV << "-E- " << getFullPath() << " does not know how to handle message:" << msgType << omnetpp::endl;
        if (p_msg->isSelfMessage())
            cancelAndDelete(p_msg);
        else
           delete p_msg;
    }
}

void IBInBuf::finish()
{
  /*for (unsigned int vl = 0; vl < maxVL+1; vl++ ) {
       EV << "STAT: " << getFullPath() << " VL:" << vl;
       EV << " Used Static Credits num/avg/max/std:"
             << staticUsageHist[vl].getCount()
             << " / " << staticUsageHist[vl].getMean()
             << " / " << staticUsageHist[vl].getMax()
             << " / " << staticUsageHist[vl].getStddev()
             << omnetpp::endl;
  }*/
  if (lossyMode){
      //recordScalar("numDroppedCredits", numDroppedCredits);
  }
  //recordScalar("marknum", marknum);
  //if(hcaIBuf)
  //{
    //recordScalar("Received-BECN-ibuf", BECNRecv);
    //recordScalar("Received-FECN-ibuf", FECNRecv);
  //}


}

IBInBuf::~IBInBuf()
{
    if (p_popMsg)
    {
      cancelAndDelete(p_popMsg);
    }
    if(p_minTimeMsg)
    {
      cancelAndDelete(p_minTimeMsg);
    }
    for (int pn = 0; pn < gateSize("out"); pn++) {
      if(Q[pn]!= NULL)
      {
          //std::cout<<Q[pn]->length();
          //Q[pn]->clear();
          //delete Q[pn];
      }
    }
    if (Q)
    {
        //Q->clear();
    }


}
