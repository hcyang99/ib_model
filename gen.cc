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
// IB FLITs Generator.
// Send IB FLITs one at a time. Received Messages from a set of Applications
// 
// Internal Messages: 
// push - inject a new data FLIT into the Queues in PCIe speed and jitter
//
// External Messages:
// sent - IN: a vHoQ was sent by the VLA (carry the VL)
// msgDone - OUT: tell the App that the message was sent
// msg - IN: receive a message from the App
//
// For a full description read the gen.h
//

#include "ib_m.h"
#include "gen.h"
#include "vlarb.h"
#include "vec_file.h"


Define_Module(IBGenerator);

// main init of the module
void IBGenerator::initialize() 
{
  cct_index.setName("CCT_Index");
  throughput.setName("Throughput");
  BECN_msgIdx.setName("BECN_msgIdx");
  gap.setName("gap_gen");
  // General non-volatile parameters
  srcLid = par("srcLid");
  flitSize_B = par("flitSize"); 
  genDlyPerByte_ns = par("genDlyPerByte");

  // statistics
  on_throughput_gen = par("on_throughput_gen");
  on_average_throughput = par("on_average_throughput");
  timeStep_us = par("timeStep_us");
  timeLastSent = 0;
  totalBytesSent = 0;
  firstPktSendTime = 0;
  lastPktSendTime = 0;// not use
  timeLastPeriod = 0;
  BytesSentLastPeriod = 0;
  startTime_s = par("startTime");
  endTime_s = par("endTime");

  // we start with pkt 0
  pktId = 0;
  msgIdx = 0;

  // init the vector of incoming messages
  numApps = gateSize("in");
  appMsgs.resize(numApps, NULL);
  VLQ.resize(8,NULL); // initialize the size of VLQ vector
  curApp = 0;
  numContPkts = 0;
  maxContPkts = par("maxContPkts");
  maxQueuedPerVL = par("maxQueuedPerVL");

  pushMsg = new omnetpp::cMessage("push1", IB_PUSH_MSG);
  timerMsg = new omnetpp::cMessage("timeout", IB_TIMER_MSG);
  cctimerMsg = new omnetpp::cMessage("cctimeout", IB_CCTIMER_MSG);
  sendtimerMsg = new omnetpp::cMessage("sendtimeout", IB_SENDTIMER_MSG);

  on_cc = par("on_cc");
  on_newcc = par("on_newcc");
  gen_BECN = 0;
  sent_BECN = 0;
  CCT_Limit = 127;
  CCT_MIN = 0;
  //CCT_Limit = 1023;
  last_RecvRate = 0;
  CCT_Index.resize(numApps, 0);
  last_BECNValue = 0;
  last_BECNValue_count = 0;
  
  increaseStep_us = par("CCT_Timer");
  Last_BECN.resize(100, 0);
  send_interval_ns = 1638.4 * 1.25 / 4;
  send_interval_ns_last = send_interval_ns;
  //send_interval_ns = genDlyPerByte_ns*flitSize_B;
  //if(srcLid == 1 || srcLid == 2)
  //{
  //send_interval_ns = 1638 / 2;
  //}
  //else
  //{
  //send_interval_ns = 1638;
  //}

  if (!timerMsg->isScheduled() && on_throughput_gen > 0) 
  {
    //omnetpp::simtime_t delay = genDlyPerByte_ns*1e-9*flitSize_B;
    omnetpp::simtime_t delay = timeStep_us*1e-6;
    scheduleAt(omnetpp::simTime()+delay+startTime_s, timerMsg);
  }
  target = 40.0 * 0.8;
  fd = NULL;
  //char index[20]; variable declared not used
  output = "throughput" + std::to_string(srcLid) + ".txt";
  LastPktSendTime = 0;
  // no need for self start
}

// initialize the packet with index pktIdx parameters on the message
void IBGenerator::initPacketParams(IBAppMsg *p_msg, unsigned int pktIdx)
{
  // double check
  if (pktIdx >= p_msg->getLenPkts()) 
  {
    error("try initPacketParams with index %d > lenPkts %d",
    pktIdx, p_msg->getLenPkts());
  }
  // zero the FLIT index
  p_msg->setFlitIdx(0);
  p_msg->setPktIdx(pktIdx);
  p_msg->setVL(vlBySQ(p_msg->getSQ()));

  unsigned int pktLen_B;
  unsigned int pktLen_F;
  // length of last msg packet may be smaller
  pktLen_B = p_msg->getPktIdx() >= p_msg->getLenPkts()? p_msg->getLenBytes() % p_msg->getMtuBytes() : p_msg->getMtuBytes();
  pktLen_B = !pktLen_B? p_msg->getMtuBytes() : pktLen_B;
  pktLen_F = (pktLen_B + flitSize_B - 1) / flitSize_B;
  p_msg->setPktLenBytes(pktLen_F*flitSize_B);
  p_msg->setPktLenFlits(pktLen_F);
}

// find the VLA and check it HoQ is free...
// NOTE THIS WILL LOCK THE HoQ - MUST IMMEDIATLY PLACE THE FLIT THERE
int IBGenerator::isRemoteHoQFree(int vl)
{
  // find the VLA connected to the given port and
  // call its method for checking and setting HoQ
  omnetpp::cGate *p_gate = gate("out")->getPathEndGate();
  IBVLArb *p_vla = dynamic_cast<IBVLArb *>(p_gate->getOwnerModule());
  if (p_vla || strcmp(p_vla->getName(), "vlarb")) 
  {
    error("cannot get VLA for generator out port");
  }
  return(p_vla->isHoQFree((int)p_gate->getIndex(), vl));
}

unsigned int IBGenerator::vlBySQ(unsigned sq) 
{
  return(sq);
}

// scan through the available applications and schedule next one
// take current VLQ threshold and maxContPkts into account
// updates curApp
// return true if found new appMsg to work on
bool IBGenerator::arbitrateApps() 
{
  // try to stay with current app if possible
  if (appMsgs.at(curApp)) 
  {
    unsigned vl = vlBySQ(appMsgs.at(curApp)->getSQ());
    if ((numContPkts < maxContPkts) && ((unsigned)VLQ.at(vl).getLength() < maxQueuedPerVL)) 
    {
      EV << "-I-" << getFullPath() << " arbitrate apps continue" << omnetpp::endl;
      return true;
    }
  }
  unsigned int oldApp = curApp;
  bool found = false;
  // search through all apps return to current 
  for (unsigned i = 1; !found && (i <= numApps); i++) 
  {
    unsigned int app_no = (curApp + i) % numApps;
    EV << "-I-" << getFullPath() << " trying app: " << app_no << omnetpp::endl;
    if (appMsgs.at(app_no)) 
    {
      unsigned vl = vlBySQ(appMsgs.at(app_no)->getSQ());
      if ((unsigned)VLQ.at(vl).getLength() < maxQueuedPerVL) 
      {
        curApp = app_no;
        EV << "-I-" << getFullPath() << " arbitrate apps selected:" 
           << app_no << omnetpp::endl;
        found = true;
      } 
      else 
      {
        EV << "-I-" << getFullPath() << " skipping app:" << app_no
           << " since VLQ[" << vl << "] is full" << omnetpp::endl;
      }
    }
  }
  numContPkts = oldApp != curApp ? 0 : ++numContPkts;
  if (!found) 
  {
    EV << "-I-" << getFullPath() << " arbitrate apps found no app" << omnetpp::endl;
  }
  return found;
}

// Called when there is some active appMsg that can be
// handled. Create the FLIT and place on VLQ, Maybe send (if VLA empty)
// also may retire the appMsg and clean the appMsgs and send it back to 
// its app
void IBGenerator::getNextAppMsg()
{
  // IN THE MSG CONECT WE ALWAYS STORE NEXT (TO BE SENT) FLIT AND PKT INDEX
  // incremeant flit idx:
  // now make the new FLIT:
  IBAppMsg *p_msg = appMsgs.at(curApp);
  IBDataMsg *p_cred;
  char name[128];
  sprintf(name, "data-%d-%d-%d-%d", srcLid, msgIdx, p_msg->getPktIdx(), p_msg->getFlitIdx());
  p_cred = new IBDataMsg(name, IB_DATA_MSG);
  p_cred->setSrcLid(srcLid);
  p_cred->setBitLength(flitSize_B*8);
  p_cred->setByteLength(flitSize_B);

  p_cred->setDstLid(p_msg->getDstLid());
  p_cred->setSL(p_msg->getSQ());
  p_cred->setVL(p_msg->getVL());

  p_cred->setFlitSn(p_msg->getFlitIdx());
  p_cred->setPacketId(p_msg->getPktIdx());
  p_cred->setMsgIdx(p_msg->getMsgIdx());
  p_cred->setAppIdx(p_msg->getAppIdx());
  p_cred->setPktIdx(p_msg->getPktIdx());
  p_cred->setMsgLen(p_msg->getLenPkts());
  p_cred->setPacketLength(p_msg->getPktLenFlits());
  p_cred->setPacketLengthBytes(p_msg->getPktLenBytes());

  p_cred->setBeforeAnySwitch(true);

  p_cred->setIsBECN(0);
  p_cred->setIsFECN(0);

  // provide serial number to packet head flits
  if (!p_msg->getFlitIdx()) 
  {
	  unsigned int dstPktSn = 0;
	  if (lastPktSnPerDst.find(p_msg->getDstLid()) == lastPktSnPerDst.end()) 
    {
		  dstPktSn = 1;
		  lastPktSnPerDst.at(p_msg->getDstLid()) = dstPktSn;
	  } 
    else 
    {
		  dstPktSn = ++lastPktSnPerDst.at(p_msg->getDstLid());
	  }
	  p_cred->setPacketSn(dstPktSn);
  } 
  else 
  {
	  p_cred->setPacketSn(0);
  }

  // now we have a new FLIT at hand we can either Q it or send it over 
  // if there is a place for it in the VLA 
  unsigned int vl = p_msg->getVL();
  BytesSentLastPeriod += p_cred->getByteLength(); 
  
  if (VLQ.at(vl).isEmpty() && isRemoteHoQFree(vl)) 
  {
    sendDataOut(p_cred);
  } 
  else 
  {
    VLQ.at(vl).insert(p_cred);
    EV << "-I- " << getFullPath() << " Queue new FLIT " << p_cred->getName() << " as HoQ not free for vl:"
       << vl << omnetpp::endl;
  }

  // now anvance to next FLIT or declare the app msg done

  // decide if we are at end of packet or not
  if (p_msg->getFlitIdx() + 1 == p_msg->getPktLenFlits()) 
  {
    // we completed a packet was it the last?
    if (p_msg->getPktIdx() + 1 == p_msg->getLenPkts())
    {
      // we are done with the app msg
      EV << "-I- " << getFullPath() << " completed appMsg:" 
         << p_msg->getName() << omnetpp::endl;
      send(p_msg, "in$o", curApp);
      appMsgs.at(curApp) = NULL;
    } 
    else 
    {
      p_msg->setPktIdx(p_msg->getPktIdx());
      initPacketParams(p_msg, p_msg->getPktIdx());
    }
  } 
  else 
  {
    p_msg->setFlitIdx(p_msg->getFlitIdx());
  }
}

// arbitrate for next app, generate its FLIT and schedule next push
void IBGenerator::genNextAppFLIT() 
{
  // get the next application to work on
  if (!arbitrateApps()) 
  {
    // may be we do not have anything to do
    if (pushMsg->isScheduled()) 
    {
      cancelEvent(pushMsg);
    }
    return;
  }
  IBAppMsg *p_msg = appMsgs.at(curApp);
  omnetpp::simtime_t delay_s = 0;
  double gapInCCT_s = 0;


  // place the next app msg FLIT into the VLQ and maybe send it
  getNextAppMsg();
  BytesSentLastPeriod += flitSize_B; 
  
  // schedule next push
  
  omnetpp::simtime_t delay = genDlyPerByte_ns*1e-9*flitSize_B ;//+ (5*CCT_Index.at(p_msg->getAppIdx()))*1e-9;
  
  if(!on_cc && !on_newcc)
  {
    scheduleAt(omnetpp::simTime()+delay, pushMsg);
  }
  else if (on_cc == 1 && !on_newcc)
  {
    //change send_interval_ns
    //send_interval_ns = 48 + 1000*(CCT_Index.at(p_msg->getAppIdx()) * CCT_Index.at(p_msg->getAppIdx())) * 600 / (69 * 69);
    
    //send_interval_ns = 1000*(CCT_Index.at(p_msg->getAppIdx()) * CCT_Index[p_msg->getAppIdx()]) * 50 / (50 * 50);
    //send_interval_ns = 1000*(CCT_Index[p_msg->getAppIdx()] * 2) * 20 / (40 * 40);
    
    //send_interval_ns = ((CCT_Index[p_msg->getAppIdx()]) * 116 + 1638);
    //
    //here!!!!!!!!!!!!!!!!!!!!!!!!! 1500
    //send_interval_ns = ((CCT_Index[p_msg->getAppIdx()]) * (CCT_Index[p_msg->getAppIdx()]) * 1500.0 / 10201.0 + 1638.4 * 1.25 / 4);
    //send_interval_ns = ((CCT_Index[p_msg->getAppIdx()]) * (CCT_Index[p_msg->getAppIdx()]) * 2800.0 / 10201.0 + 1638.4 * 1.25 / 4);
    //send_interval_ns = ((CCT_Index[p_msg->getAppIdx()]) * (CCT_Index[p_msg->getAppIdx()]) * 2900.0 / 10201.0 + 1638.4 * 1.25 / 4);
    send_interval_ns = ((CCT_Index.at(p_msg->getAppIdx())) * (CCT_Index.at(p_msg->getAppIdx())) * 3300.0 / 6889.0 + 1638.4 * 1.25 / 4);
  
    /*if(p_msg->getFlitIdx() < 31)
    {
      send_interval_ns = genDlyPerByte_ns*flitSize_B;
    }
    else
    {
      //send_interval_ns = 25.6 * 16;
      send_interval_ns = ((CCT_Index[p_msg->getAppIdx()]) * (CCT_Index[p_msg->getAppIdx()]) *  31.0 * 2800.0 / 10201.0 + genDlyPerByte_ns*flitSize_B);
      throughput.record(2048 * 8 / (send_interval_ns + genDlyPerByte_ns*flitSize_B * 31));
      //throughput.record(128 * 8 / send_interval_ns);
      for(int i = 0; i < numApps; i++)
      {
        cct_index.record(CCT_Index[i]);
      }
    }*/
  }
}

// push is an internal event for generating a new FLIT
void IBGenerator::handlePush(omnetpp::cMessage *p_msg) //parameter passed not used??
{
  // arbitrate next app, 
  if(!on_cc && !on_newcc)
  {
    genNextAppFLIT();
  }
}


void IBGenerator::handleTimer(omnetpp::cMessage *p_msg)
{
  //record throughput
  if(on_throughput_gen && p_msg->getKind() == IB_TIMER_MSG)
  {
    double oBW = BytesSentLastPeriod / (timeStep_us*1e-6);
    //throughput.record(oBW * 8.0 / 1e9);
    BytesSentLastPeriod = 0;  
    //double temp = (send_interval_ns + send_interval_ns_last) / 2.0;
    double temp = send_interval_ns;  
    throughput.record(2048 * 8.0 / temp);
    omnetpp::simtime_t delay = timeStep_us*1e-6;
    scheduleAt(omnetpp::simTime()+delay, p_msg);  
    send_interval_ns_last = send_interval_ns;
    return;
  }

  //timer for rate increase
  if(on_cc && p_msg->getKind() == IB_CCTIMER_MSG )
  {
    //decrease cct_index
    for(int i = 0; i < numApps; i++)
    {
      if( CCT_Index.at(i) > CCT_MIN)
      {
        CCT_Index.at(i) = (CCT_Index.at(i)-1 <= CCT_MIN) ? CCT_MIN : --CCT_Index.at(i);
      }
      //cct_index.record(CCT_Index[i]);
    }
    omnetpp::simtime_t delay = increaseStep_us*1e-6;
    scheduleAt(omnetpp::simTime()+delay, p_msg);    
    return;
  }

  if(!on_newcc && p_msg->getKind() == IB_CCTIMER_MSG )
  {
    //send_interval_ns = send_interval_ns * 0.8;
    omnetpp::simtime_t delay = increaseStep_us*1e-6;
    if(!cctimerMsg->isScheduled())
    {
      scheduleAt(omnetpp::simTime()+delay, p_msg);
    }  
    return;
  }
}

void IBGenerator::handleSendTimer(omnetpp::cMessage *p_msg)
{
  //timer for send interval
  if(on_cc || on_newcc)
  {
    genNextAppFLIT();
    omnetpp::simtime_t delay1 = send_interval_ns*1e-9;
    //omnetpp::simtime_t delay2 = genDlyPerByte_ns*1e-9*flitSize_B;
    //if(p_msg->getFlitIdx()> 0)
    //{
    //scheduleAt(omnetpp::simTime()+delay2, sendtimerMsg);
    //}
    //else
    //{
    //throughput.record(2048 * 8.0 / send_interval_ns);
    scheduleAt(omnetpp::simTime()+delay1, p_msg);
    //}
  }
}
/* receive FECN from sink module
1. generate a BECN message, initialize the srclid, dstlid,sl
*/

void IBGenerator::handlePushFECN(IBPushFECNMsg *msg)
{
  //cct_index.record(msg->getRecvRate());
  char name[128];
  sprintf(name, "BECN-%d-%d-%d-%d", msg->getDstLid(), msg->getSrcLid(), msg->getMsgIdx(), msg->getAppIdx());
  IBDataMsg *p_BECN;
  p_BECN = new IBDataMsg(name, IB_DATA_MSG);
  p_BECN->setSrcLid(msg->getDstLid());
  p_BECN->setBitLength(flitSize_B*8);
  p_BECN->setByteLength(flitSize_B);

  p_BECN->setDstLid(msg->getSrcLid());
  p_BECN->setSL(msg->getSL());
  p_BECN->setVL(msg->getSL());

  p_BECN->setFlitSn(0);
  p_BECN->setPacketId(0);
  p_BECN->setMsgIdx(msg->getMsgIdx());
  p_BECN->setAppIdx(msg->getAppIdx());
  p_BECN->setPktIdx(0);
  p_BECN->setMsgLen(1);
  p_BECN->setPacketLength(1);
  p_BECN->setPacketLengthBytes(flitSize_B);

  p_BECN->setBeforeAnySwitch(true);

  p_BECN->setIsBECN(msg->getBECNValue());
  p_BECN->setIsFECN(0);
  p_BECN->setRecvRate(msg->getRecvRate());
  unsigned int vl = msg->getSL();
  
  //BECN_msgIdx.record(msg->getAppIdx());

  // now we have a new FLIT at hand we can either Q it or send it over 
  // if there is a place for it in the VLA 
  if(Last_BECN.at(msg->getSrcLid())!=0)
  {
    //if (omnetpp::simTime()*1e6 - Last_BECN[msg->getSrcLid()] > 15){
    if (omnetpp::simTime()*1e6 - Last_BECN.at(msg->getSrcLid()) > 0)
    {
      Last_BECN.at(msg->getSrcLid()) = omnetpp::simTime()*1e6;
      if (VLQ.at(vl).isEmpty() && isRemoteHoQFree(vl)) 
      {
        sendDataOut(p_BECN);
      } 
      else 
      {
        VLQ.at(vl).insert(p_BECN);
        EV << "-I- " << getFullPath() << " Queue new BECN FLIT " << p_BECN->getName() << " as HoQ not free for vl:"
        << vl << omnetpp::endl;
      }
      gen_BECN++;
    }
    else
    {      
      delete p_BECN;
    }
    delete msg;
    return;
  }
  Last_BECN.at(msg->getSrcLid()) = omnetpp::simTime()*1e6;
  if (VLQ.at(vl).isEmpty() && isRemoteHoQFree(vl)) 
  {
    sendDataOut(p_BECN);
  } 
  else 
  {
    VLQ.at(vl).insert(p_BECN);
    EV << "-I- " << getFullPath() << " Queue new BECN FLIT " << p_BECN->getName() << " as HoQ not free for vl:"
       << vl << omnetpp::endl;
  }
  gen_BECN++;
  delete msg;
}


/* receive BECN from sink module
1. rate decrease: increase the CCT_index
*/
void IBGenerator::handlePushBECN(IBPushBECNMsg *msg)
{
  if(on_newcc /*&& srcLid <= 2*/)
  {
    double currentRate = flitSize_B * 8.0 / send_interval_ns;
    double nextRate = currentRate;
    //cct_index.record(target);
    
    if(last_BECNValue == 3 && msg->getBECNValue() == 3)
    {
      last_BECNValue_count ++;
    }
    else if(last_BECNValue != 3 && msg->getBECNValue() == 3)
    {
      last_BECNValue_count = 1;
    }
    else
    {
      last_BECNValue_count = 0;
    }
    if(msg->getBECNValue() == 1)//congested
    {
      if(msg->getRecvRate() > 0)
      { 
        //if(last_BECNValue != 1)
        //{
        target = flitSize_B * 8.0 / send_interval_ns;
        //}             
        send_interval_ns = 1.07 * 2048 * 8 / msg->getRecvRate();
        last_RecvRate = msg->getRecvRate();
      }
    }    
    else if (msg->getBECNValue() == 3) //non-congestion
    {
      /*if(last_BECNValue_count >= 5){
      nextRate = currentRate * 0.5 + 0.5*currentRate*(1+ 0.2 * (40.0-currentRate)/40.0);     
      send_interval_ns = 2048 * 8 / nextRate;
      }
      else
      {
        nextRate = (currentRate + currentRate+1) * 0.5;     
        send_interval_ns = 2048 * 8 / nextRate;
      }*/
      if(last_BECNValue_count >= 3)
      {
        //target = target + 0.28;
        target = target + 0.39;  
        if(target > 32.0)
        {
          target = 32.0;
        }     
        nextRate = (currentRate + target) * 0.5;
        send_interval_ns = 2048.0 * 8 / nextRate;
        //last_BECNValue_count = 0;
      }
      else
      {
        nextRate = (currentRate + target) * 0.5;     
        send_interval_ns = 2048.0 * 8 / nextRate;
      }
    }
    last_BECNValue = msg->getBECNValue();
  }
  else if(on_cc /*&& srcLid <= 2*/)
  {
    if(CCT_Index.at(msg->getAppIdx()) < CCT_Limit)
    {
      CCT_Index.at(msg->getAppIdx()) = (CCT_Index.at(msg->getAppIdx())+1 >= CCT_Limit)? CCT_Limit: ++CCT_Index.at(msg->getAppIdx());
    }
  }
  delete msg;
}

// when a new application message in provided
void IBGenerator::handleApp(IBAppMsg *p_msg)
{
  // decide what port it was provided on
  unsigned int appidx = p_msg->getArrivalGate()->getIndex();
  // check that the app is empty or error
  if (appMsgs.at(appidx)) 
  {
    error("provided app %d message but app not empty!", appidx);
  }
  // count total of messages injected
  msgIdx++;
  // init the first packet parameters
  initPacketParams(p_msg, 0);
  
  // store it
  appMsgs.at(appidx) = p_msg;

  if(!on_cc && !on_newcc)
  {
    // if there is curApp msg or waiting on push pushMsg = do nothing
    if (((curApp != appidx) && appMsgs.at(curApp)) || (pushMsg->isScheduled())) 
    {
      EV << "-I-" << getFullPath() << " new app message:" << p_msg->getName()
         << " queued since previous message:" << appMsgs.at(curApp)->getName() 
         << " being served" << omnetpp::endl;
      return;
    }
    // force the new app to be arbitrated
    curApp = appidx;
    genNextAppFLIT();
  }
  else
  {
    if(msgIdx == 1)
    {
      curApp = appidx;
      genNextAppFLIT();
      if(!sendtimerMsg->isScheduled())
      {
        omnetpp::simtime_t delay1 = send_interval_ns*1e-9;
        scheduleAt(omnetpp::simTime() + delay1, sendtimerMsg);
      }
    }
    curApp = appidx;
    //
  }
}

// send out data and wait for it to clear
void IBGenerator::sendDataOut(IBDataMsg *p_msg)
{
  unsigned int bytes = p_msg->getByteLength();
  double delay_ns = ((double)par("popDlyPerByte"))*bytes;  

  // time stamp to enable tracking time in Fabric  
  p_msg->setInjectionTime(omnetpp::simTime()+delay_ns*1e-9);
  p_msg->setTimestamp(omnetpp::simTime()+delay_ns*1e-9);
  //p_msg->setTimestamp(omnetpp::simTime());
  if(omnetpp::simTime()>=0.01)
  {
    totalBytesSent += bytes;
  }
  LastPktSendTime = omnetpp::simTime();
  
  sendDelayed(p_msg, delay_ns*1e-9, "out");

  EV << "-I- " << getFullPath() 
     << " sending " << p_msg->getName() 
     << " packetLength(B):" << bytes
     << " flitSn:" << p_msg->getFlitSn() 
     << " dstLid:" << p_msg->getDstLid() 
     << omnetpp::endl;
  
  // For oBW calculations
  if (firstPktSendTime == 0) 
  {   
    firstPktSendTime = LastPktSendTime;
    timeLastPeriod = firstPktSendTime;
    if (!timerMsg->isScheduled() && on_throughput_gen > 0) 
    {
      //omnetpp::simtime_t delay = genDlyPerByte_ns*1e-9*flitSize_B;
      // omnetpp::simtime_t delay = timeStep_us*1e-6;
      //scheduleAt(omnetpp::simTime()+delay, timerMsg);
    }
    //actual start timer
    if(on_cc && !on_newcc)
    {
      //CCT_Index[p_msg->getAppIdx()] = 14;
      if (!cctimerMsg->isScheduled()) 
      {
        omnetpp::simtime_t delay = increaseStep_us*1e-6;
        scheduleAt(omnetpp::simTime()+delay, cctimerMsg);
      }
    }
  }
}

// when the VLA has sent a message
void IBGenerator::handleSent(IBSentMsg *p_sent)
{
  int vl = p_sent->getVL();
  // We can not just send - need to see if the HoQ is free...
  // NOTE : since we LOCK the HoQ when asking if HoQ is free we 
  // must make sure we have something to send before we ask about it
  if (!VLQ.at(vl).isEmpty()) 
  {
    if (isRemoteHoQFree(vl)) 
    {   
      IBDataMsg *p_msg = (IBDataMsg *)VLQ.at(vl).pop();
      EV << "-I- " << getFullPath() << " de-queue packet:"
         << p_msg->getName()<< " at time " << omnetpp::simTime() << omnetpp::endl;
      sendDataOut(p_msg);

      // since we popped a message we may have now free'd some space
      // if there is no shceduled push ...
      if(!on_cc)
      {
        if (!pushMsg->isScheduled()) 
        {
          omnetpp::simtime_t delay = genDlyPerByte_ns*1e-9*flitSize_B;
          scheduleAt(omnetpp::simTime()+delay, pushMsg);
        }
      }
    } 
    else 
    {
      EV << "-I- " << getFullPath() << " HoQ not free for vl:" << vl << omnetpp::endl;
    }
  } 
  else 
  {
    EV << "-I- " << getFullPath() << " nothing to send on vl:" << vl << omnetpp::endl;
  }
  delete p_sent;
}

void IBGenerator::handleMessage(omnetpp::cMessage *p_msg) 
{
  if (p_msg->isSelfMessage())
    cancelAndDelete(p_msg);
  else
  {
    switch(p_msg->getKind())
    {
      case 3  : handleSent((IBSentMsg *)p_msg); break; //in the case of IB_SENT_MSG
      case 11 : handleApp((IBAppMsg*)p_msg); break; //in the case of IB_APP_MSG
      case 12 : handlePush(p_msg); break; //in the case of IB_PUSH_MSG
      case 14 : handleTimer(p_msg); break; //in the case of IB_TIMER_MSG
      case 15 : handlePushFECN((IBPushFECNMsg*)p_msg); break; //in the case of IB_PUSHFECN_MSG
      case 16 : handlePushBECN((IBPushBECNMsg*)p_msg); break; //in the case of IB_PUSHBECN_MSG
      case 17 : handleTimer(p_msg); break; //in the case of IB_CCTIMER_MSG
      case 18 : handleSendTimer(p_msg); break; //in the case of IB_SENDTIMER_MSG
      default: delete p_msg;
    }
  }
}

void IBGenerator::finish()
{
  double oBW = totalBytesSent * 8.0 / (LastPktSendTime - /*firstPktSendTime*/0.01) / 1e9;
  //EV << "STAT: " << getFullPath() << " Gen Output BW (Bit/s):" << oBW * 8  << omnetpp::endl;
  if(on_average_throughput == 1)
  {
    //std::cout<<srcLid<<" "<<LastPktSendTime <<" "<<firstPktSendTime<<omnetpp::endl;
    recordScalar("average throughput", oBW);
  }  
  //recordScalar("generate BECN", gen_BECN);
  //PktSendTime = firstPktSendTime;
  //recordScalar("first packet for RTT", PktSendTime);
  if(fd)
  {
    std::fclose(fd);
  }
}

IBGenerator::~IBGenerator() 
{
  if (pushMsg) cancelAndDelete(pushMsg);
  if (timerMsg) cancelAndDelete(timerMsg);
  if (cctimerMsg) cancelAndDelete(cctimerMsg);
  if (sendtimerMsg) cancelAndDelete(sendtimerMsg);

  for(int no = 0;no < appMsgs.size();no++)
  {
    if(appMsgs.at(no))
    {
      delete appMsgs.at(no);
    }
  }
  appMsgs.clear();
}