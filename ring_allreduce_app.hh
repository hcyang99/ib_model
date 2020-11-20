#include <omnetpp.h>
#include <mutex>
#include "NodeAlloc.hh"

class IBRingAllreduceApp : public omnetpp::cSimpleModule
{
    private:
    static constexpr unsigned num_workers_ = 8;
    static constexpr unsigned msgMtuLen_B_ = 2048;
    static constexpr unsigned msgLen_B_ = 80 * 1024;
    static std::mutex finishCountMutex_;
    static int finishCount_;
    
    NodeAlloc nodeAllocVec_;
    unsigned rank_;
    unsigned counter_;
    unsigned recv_counter_;
    
    virtual ~IBRingAllreduceApp() {}
    omnetpp::cMessage* getMsg(unsigned& msgIdx);

    protected:
    virtual void initialize() override;
    virtual void handleMessage(omnetpp::cMessage* msg) override;
    virtual void finish() override {}
};