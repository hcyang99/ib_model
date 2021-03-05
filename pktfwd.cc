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
#include "pktfwd.h"
#include "vec_file.h"
#include "obuf.h"

Define_Module(Pktfwd);

void Pktfwd::initialize() 
{
	Switch = getParentModule();
	if (!Switch) 
	{
		error("-E- Failed to obtain an parent Switch module");
	}
	numPorts = Switch->par("numSwitchPorts");
}

void Pktfwd::FDB_Autoconfigure(const omnetpp::cPacket *pkt) 
{
	Enter_Method("Configuring FDB");
	if (pkt == NULL|| !pkt->getVector.size())
	{
		error("-E- Failed to configure FDB as no routing information can be found on packet received %s",pkt->getName());
	}
	else
	{
		FDB_Auto.resize(pkt->getVector.size(),NULL);
		for (auto &elm:pkt->getVector)
		{
			if (FDB_Auto.at(elm.first)==NULL)
				FDB_Auto.at(elm.first) = elm.second;
			else
				error("Fail in configuring FDB as encounter multiple output ports for LID:%d !!",elm.first);
		}
		FDB = &FDB_Auto;
	}
}

// get the output port for the given LID - the actual AR or deterministic routing
int Pktfwd::getPortByLID(unsigned int sLid, unsigned int dLid) 
{
	Enter_Method("getPortByLID LID: %d", dLid);
	unsigned int outPort; // the resulting output port
	if (dLid >= FDB->size()) 
	{
	    error("-E- getPortByLID: LID %d is out of available FDB range %d",
		dLid, FDB->size() - 1);
	}
	outPort = (*FDB).at(dLid);
	return(outPort);
}

// report queuing of flits on TQ for DLID (can be negative for arb)
int Pktfwd::repQueuedFlits(unsigned int rq, unsigned int tq, unsigned int dlid, int numFlits) 
{
	Enter_Method("repQueuedFlits tq:%d flits:%d", tq, numFlits);
	return(0);
}

// IBuf received a TQLoadUpdate - Handle Received Port Usage Notification
void Pktfwd::handleTQLoadMsg(unsigned int tq, unsigned int srcRank, unsigned int firstLid, unsigned int lastLid, int load) 
{
	Enter_Method("handleTQLoadMsg tq:%d srcRank:%d lid-range: [%d,%d] load:%d", tq, srcRank, firstLid,
			lastLid, load);
	EV << "-I- " << getFullPath() << " handleTQLoadMsg tq: " << tq << " srcRank: " << srcRank << " lids: "
			<< firstLid << "," << lastLid << " load: " << load << omnetpp::endl;
}

void Pktfwd::finish()
{
}

Pktfwd::~Pktfwd() 
{
	delete FDB;
}