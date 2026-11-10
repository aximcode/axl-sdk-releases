@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Running echo-client test...
echo-client.efi 10.0.2.2 %1 "hello from UEFI"
