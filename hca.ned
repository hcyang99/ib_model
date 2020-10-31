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
package ib_model;

//
// This NED provides an HCA model

module HCA
{
    parameters:
        int srcLid;
        @display("bgb=340,225;i=device/server2");
    gates:
        inout in;
        inout out;

    submodules:
        app: IBRingAllreduceApp {
            parameters:
                @display("p=313,170");
        }
        gen_in: IBGenerator {
            parameters:
                srcLid = srcLid;
                @display("i=block/source;p=246,170");
            gates:
                in[1];
        }
        gen_out: IBGenerator {
            parameters:
                srcLid = srcLid;
                @display("i=block/source;p=246,170");
            gates:
                in[1];
        }
        sink_in: IBSink {
            parameters:
                @display("i=block/sink;p=248,74");
        }
        sink_out: IBSink {
            parameters:
                @display("i=block/sink;p=248,74");
        }
        obuf_in: IBOutBuf {
            parameters:
                isHcaOBuf = 1;
                @display("i=block/queue;p=69,169");
        }
        obuf_out: IBOutBuf {
            parameters:
                isHcaOBuf = 1;
                @display("i=block/queue;p=69,169");
        }
        ibuf_in: IBInBuf {
            parameters:
                isHcaIBuf = 1;
                numPorts = 1;
                @display("i=block/subqueue;p=69,74");
            gates:
                out[1];
                sent[1];
        }
        ibuf_out: IBInBuf {
            parameters:
                isHcaIBuf = 1;
                numPorts = 1;
                @display("i=block/subqueue;p=69,74");
            gates:
                out[1];
                sent[1];
        }
        vlarb_in: IBVLArb {
            parameters:
                isHcaArbiter = 1;
                numPorts = 1;
                @display("i=block/dispatch;p=158,170");
            gates:
                in[1];
                sent[1];

        }
        vlarb_out: IBVLArb {
            parameters:
                isHcaArbiter = 1;
                numPorts = 1;
                @display("i=block/dispatch;p=158,170");
            gates:
                in[1];
                sent[1];

        }
        dummy: Dummy {}
    connections:
        app.out <--> gen_out.in[0];
        sink_in.out --> app.in;
        dummy.out <--> gen_in.in[0];
        sink_out.out --> dummy.in;

        gen_out.out --> vlarb_out.in[0];
        gen_in.out --> vlarb_in.in[0];

        vlarb_out.sent[0] --> { @display("m=m,68,20,68,20"); } --> gen_out.sent;
        vlarb_out.out --> { @display("m=m,16,56,24,60"); } --> obuf_out.in;
        obuf_out.free --> { @display("m=m,20,28,4,24"); } --> vlarb_out.free;
        vlarb_in.sent[0] --> { @display("m=m,68,20,68,20"); } --> gen_in.sent;
        vlarb_in.out --> { @display("m=m,16,56,24,60"); } --> obuf_in.in;
        obuf_in.free --> { @display("m=m,20,28,4,24"); } --> vlarb_in.free;

        ibuf_out.out[0] --> sink_out.in;
        sink_out.sent --> ibuf_out.sent[0];
        ibuf_in.out[0] --> sink_in.in;
        sink_in.sent --> ibuf_in.sent[0];

        ibuf_out.rxCred --> { @display("m=m,28,12,36,12"); } --> obuf_out.rxCred;
        ibuf_out.txCred --> vlarb_out.txCred;
        ibuf_in.rxCred --> { @display("m=m,28,12,36,12"); } --> obuf_in.rxCred;
        ibuf_in.txCred --> vlarb_in.txCred;

        out$i --> ibuf_out.in;
        obuf_out.out --> out$o;
        in$i --> ibuf_in.in;
        obuf_in.out --> in$o;

        sink_out.pushFECN --> gen_out.recvFECN;
        sink_out.pushBECN --> gen_out.recvBECN;
        sink_in.pushFECN --> gen_in.recvFECN;
        sink_in.pushBECN --> gen_in.recvBECN;
}
