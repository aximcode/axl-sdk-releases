@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting HTTPS+WS pending-TX close server (resident driver-tick loop)...
AxlTestNet.efi serve-tls-ws-close-pendtx-driver
