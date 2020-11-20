#include "ring_allreduce_app.hh"
#include "ib_m.h"

using namespace omnetpp;

Define_Module(IBRingAllreduceApp);

int IBRingAllreduceApp::finishCount_;
std::mutex IBRingAllreduceApp::finishCountMutex_;

void IBRingAllreduceApp::initialize()
{
    nodeAllocVec_.init(par("nodeAllocFile"));
    rank_ = par("rank");
    counter_ = 0;
    recv_counter_ = 0;
    num_workers_ = nodeAllocVec_.size();
    finishCount_ = num_workers_;
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
        
        ++recv_counter_;
        if (recv_counter_ >= num_workers_)
        {
            IBRingAllreduceApp::finishCountMutex_.lock();
            --IBRingAllreduceApp::finishCount_;
            if (IBRingAllreduceApp::finishCount_ == 0)
            {
                error("Finished.\n");
            }
            IBRingAllreduceApp::finishCountMutex_.unlock();
        }
    }
    delete msg;
}

cMessage* IBRingAllreduceApp::getMsg(unsigned& msgIdx)
{
    IBAppMsg* p_msg = new IBAppMsg(nullptr, IB_APP_MSG);
    p_msg->setAppIdx(rank_);
    p_msg->setMsgIdx(msgIdx);
    p_msg->setDstLid(nodeAllocVec_[(rank_ + 1 > num_workers_ ? 1 : rank_ + 1) - 1]);
    // assert(p_msg->getDstLid() != rank_);
    p_msg->setSQ(0);
    p_msg->setLenBytes(msgLen_B_ / num_workers_);
    p_msg->setLenPkts(msgLen_B_ / num_workers_ / msgMtuLen_B_);
    p_msg->setMtuBytes(msgMtuLen_B_);
    ++msgIdx;
    EV << "msgIdx: " << msgIdx << omnetpp::endl;
    return p_msg;
}