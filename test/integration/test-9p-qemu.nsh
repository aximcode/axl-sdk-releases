@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Running 9P client mode...
AxlTestNet.efi 9p-client 10.0.2.2 %1
stall 1000000

echo Running 9P mount selftest...
9p-mount-selftest.efi 10.0.2.2 %1
stall 1000000
