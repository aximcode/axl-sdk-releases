@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting serve-ws-held-101...
AxlTestNet.efi serve-ws-held-101
stall 1000000
