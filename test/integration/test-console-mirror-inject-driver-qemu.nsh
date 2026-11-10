@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting HTTP+WS server + mirror, launching Shell...
AxlTestNet.efi serve-ws-shell-inject
