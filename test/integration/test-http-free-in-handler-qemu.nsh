@echo -off
connect -r
stall 1000000
ifconfig -s eth0 dhcp
stall 3000000
AxlTestNet.efi serve-free-in-handler
stall 1000000
