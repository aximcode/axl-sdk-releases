@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Loading gen-0 service...
load reload-svc-dxe.efi
stall 3000000

echo Reloading it to the poisoned image from this Shell command...
reload-svc.efi
stall 5000000
