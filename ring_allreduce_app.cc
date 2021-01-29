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
    finishCount_ = 24;
    data_.resize(num_workers_, 0);
    // use self message to start 
    if (num_workers_ != 0)
    {
        data_[(2 * num_workers_ + rank_ - 2) % num_workers_] = 1;
        scheduleAt(simTime() + SimTime(10, SIMTIME_NS), new cMessage);
    }
}

void IBRingAllreduceApp::handleMessage(cMessage* msg)
{
    if (!msg->isSelfMessage())
    {
        const char* g = msg->getArrivalGate()->getFullName();
        EV << "-I- " << getFullPath() << " received from:" 
            << g << omnetpp::endl;
    }
    // init or sent a whole message
    if (msg->isSelfMessage() || msg->getKind() == IB_SENT_MSG)
    {
        if (!msg->isSelfMessage())
        {
            const char* g = msg->getArrivalGate()->getFullName();
            EV << "-I- " << getFullPath() << " received sent msg from:" 
                << g << omnetpp::endl;
        }
        is_sending_ = false;
        trySendNext();
    }
    // received a whole message
    else if (msg->getKind() == IB_DONE_MSG)
    {
        const char* g = msg->getArrivalGate()->getFullName();
        EV << "-I- " << getFullPath() << " received done msg " << recv_counter_ << " from:" 
            << g << omnetpp::endl;
        // {
        //     static int _cnt = 0;
        //     if (rank_ == 307)
        //     {
        //         ++_cnt;
        //         std::cout << "-I- " << getFullPath() << " received data: " << _cnt << std::endl;
        //     }
        // }
                
    

        ++recv_counter_;
        IBDoneMsg* d_msg = reinterpret_cast<IBDoneMsg*>(msg);
        ++data_[d_msg->getAppIdx()];
        trySendNext();

        if (recv_counter_ >= 2 * num_workers_ - 1)
        {
            std::cout << rank_ << " finished at " << getSimulation()->getSimTime().str() << "\n";
            IBRingAllreduceApp::finishCountMutex_.lock();
            --IBRingAllreduceApp::finishCount_;
            if (IBRingAllreduceApp::finishCount_ == 0)
            {
                std::cerr << "Finished at " << getSimulation()->getSimTime().str() << "\n";
                std::exit(0);
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
    p_msg->setAppIdx((4 * num_workers_ + rank_ - 2 - msgIdx) % num_workers_);
    p_msg->setMsgIdx(msgIdx);
    p_msg->setDstLid(nodeAllocVec_[rank_ + 1 > num_workers_ ? 0 : rank_]);
    // assert(p_msg->getDstLid() != rank_);
    p_msg->setSQ(0);
    p_msg->setLenBytes(msgLen_B_ / num_workers_);
    p_msg->setLenPkts(msgLen_B_ / num_workers_ / msgMtuLen_B_);
    p_msg->setMtuBytes(msgMtuLen_B_);

    // if (p_msg->getMsgIdx() == 2 && p_msg->getDstLid() == 1)
    //     error("Break\n");

    ++msgIdx;
    // EV << "msgIdx: " << msgIdx << omnetpp::endl;
    return p_msg;
}

void IBRingAllreduceApp::trySendNext()
{
    // if (rank_ == 1)
    //     error("Break\n");
    int idx = (4 * num_workers_ + rank_ - 2 - counter_) % num_workers_;
    if (is_sending_)
        return;
    if (counter_ <= num_workers_ - 1)
    {
        if (data_[idx] >= 1)
            goto send;
    }
    else 
    {
        if (data_[idx] == 2)
            goto send;
    }
    return;

    send:
    is_sending_ = true;
    cMessage* msg_new = getMsg(counter_);
    send(msg_new, "out$o");
    // if (rank_ == 307)
    //     std::cout << "-I- " << getFullPath() << " sent data: " << counter_ 
    //         << " at " << getSimulation()->getEventNumber() << std::endl;
    return;
}