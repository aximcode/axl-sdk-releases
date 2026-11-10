@echo -off
echo Connecting drivers...
connect -r
stall 1000000
echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000
ifconfig -l
stall 1000000
echo Starting multi-server (TLS 8443 + plain 8081, shared loop)...
AxlTestNet.efi serve-multi-tls
