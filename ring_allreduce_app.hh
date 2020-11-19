#include <omnetpp.h>

class IBRingAllreduceApp : public omnetpp::cSimpleModule
{
    private:
    static const unsigned num_workers_ = 8;
    static const unsigned msgMtuLen_B_ = 2048;
    static const unsigned msgLen_B_ = 80 * 1024;
    
    unsigned rank_;
    unsigned counter_;
    
    virtual ~IBRingAllreduceApp() {}
    omnetpp::cMessage* getMsg(unsigned& msgIdx);

    protected:
    virtual void initialize() override;
    virtual void handleMessage(omnetpp::cMessage* msg) override;
    virtual void finish() override {}
};