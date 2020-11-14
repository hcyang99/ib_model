import ib_model.*;
package ib_model.examples.switch8;

network switch8
{
    submodules:
        H[8]: HCA {
            parameters:
                @display("i=device/server2;p=80,109");
        }

        sw[2]: Switch {
            parameters:
                numSwitchPorts = 8;
                @display("p=182,58");
            gates:
                port[8];
        }
    
    connections:
        for k = 0..7 
        {
            H[k].out$o --> IB4XQDRWire --> sw[0].port$i[k];
            H[k].in$i <-- IB4XQDRWire <-- sw[0].port$o[k];

            H[k].in$o --> IB4XQDRWire --> sw[1].port$i[k];
            H[k].out$i <-- IB4XQDRWire <-- sw[1].port$o[k];
        }
}