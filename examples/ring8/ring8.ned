import ib_model.*;
package ib_model.examples.ring8;

network ring8
{
    submodules:
        H[8]: HCA {
            parameters:
                @display("i=device/server2;p=80,109");
        }

    connections:
        for i = 0..6
        {
            H[i].out <--> IB4XQDRWire <--> H[i+1].in;
        }
        H[7].out <--> IB4XQDRWire <--> H[0].in;
}