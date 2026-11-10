@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Running concurrent-Transmit spike...
AxlTestNet.efi tcp-multi-tx 10.0.2.2 %1 %2 %3 %4
