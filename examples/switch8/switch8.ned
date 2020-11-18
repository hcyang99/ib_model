import ib_model.*;
package ib_model.examples.switch8;

network switch8
{
    submodules:
        H[8]: HCA {
            parameters:
                @display("i=device/server2;p=80,109");
        }

        sw: Switch {
            parameters:
                numSwitchPorts = 16;
                @display("p=182,58");
            gates:
                port[16];
        }
    
    connections:
        for k = 0..7 
        {
            H[k].out$o --> IB4XQDRWire --> sw.port$i[2 * k + 1];
            H[k].in$i <-- IB4XQDRWire <-- sw.port$o[2 * k + 1];

            H[k].in$o --> IB4XQDRWire --> sw.port$i[2 * k];
            H[k].out$i <-- IB4XQDRWire <-- sw.port$o[2 * k];
        }
}