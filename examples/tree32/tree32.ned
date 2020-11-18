import ib_model.*;
package ib_model.examples.tree32;

network tree32
{
    submodules:
        H[32]: HCA {
            parameters:
                @display("i=device/server2;p=80,109");
        }

        L1_sw[4]: Switch {
            parameters:
                numSwitchPorts = 17;
                @display("p=182,58");
            gates:
                port[17];
        }

        L2_sw: Switch {
            parameters:
                numSwitchPorts = 4;
                @display("p=182,58");
            gates:
                port[4];
        }
    
    connections:
        for i = 0..3
        {
            L1_sw[i].port[16] <--> L2_sw.port[i];
        }

        for k = 0..31
        {
            H[k].out$o --> IB4XQDRWire --> L1_sw[k / 8].port$i[2 * (k % 8)+ 1];
            H[k].in$i <-- IB4XQDRWire <-- L1_sw[k / 8].port$o[2 * (k % 8)+ 1];

            H[k].in$o --> IB4XQDRWire --> L1_sw[k / 8].port$i[2 * (k % 8)];
            H[k].out$i <-- IB4XQDRWire <-- L1_sw[k / 8].port$o[2 * (k % 8)];
        }
}