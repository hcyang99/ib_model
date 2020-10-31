#include <omnetpp.h>

class Dummy : public omnetpp::cSimpleModule
{
    private:
    virtual ~Dummy() {}

    protected:
    virtual void initialize() override {}
    virtual void handleMessage(omnetpp::cMessage* msg) override {}
    virtual void finish() override {}
};