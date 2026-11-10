@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting serve-rebind (%1)...
AxlTestNet.efi serve-rebind %1
stall 1000000
