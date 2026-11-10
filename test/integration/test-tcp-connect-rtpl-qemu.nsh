@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Running raised-TPL TCP connect test...
AxlTestNet.efi tcp-connect-rtpl 10.0.2.2 %1
