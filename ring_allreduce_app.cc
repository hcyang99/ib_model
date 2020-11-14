#include "ring_allreduce_app.hh"
#include "ib_m.h"

using namespace omnetpp;

Define_Module(IBRingAllreduceApp);

void IBRingAllreduceApp::initialize()
{
    rank_ = par("rank");
    counter_ = 0;
    // use self message to start 
    scheduleAt(simTime() + SimTime(10, SIMTIME_NS), new cMessage);
}

void IBRingAllreduceApp::handleMessage(cMessage* msg)
{
    if (!msg->isSelfMessage())
    {
        const char* g = msg->getArrivalGate()->getFullName();
        EV << "-I- " << getFullPath() << " received from:" 
            << g << omnetpp::endl;
    }
    if (msg->isSelfMessage() || msg->getKind() == IB_SENT_MSG && counter_ < num_workers_)
    {
        cMessage* msg_new = getMsg(counter_);
        send(msg_new, "out$o");
        EV << "-I- " << getFullPath() << " sent data: " << counter_ 
            << msg_new->getName() << omnetpp::endl;
    }
    delete msg;
    // if (counter_ >= num_workers_)
    //     error("Finished.\n");
}

cMessage* IBRingAllreduceApp::getMsg(unsigned& msgIdx)
{
    IBAppMsg* p_msg = new IBAppMsg(nullptr, IB_APP_MSG);
    p_msg->setAppIdx(rank_);
    p_msg->setMsgIdx(msgIdx);
    p_msg->setDstLid(rank_ + 1 > num_workers_ ? 2 : 2 * rank_ + 2);
    // assert(p_msg->getDstLid() != rank_);
    p_msg->setSQ(0);
    p_msg->setLenBytes(msgLen_B_ / num_workers_);
    p_msg->setLenPkts(msgLen_B_ / num_workers_ / msgMtuLen_B_);
    p_msg->setMtuBytes(msgMtuLen_B_);
    ++msgIdx;
    return p_msg;
}