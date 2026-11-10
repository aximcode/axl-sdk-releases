@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting SSH server...
ssh-server-selftest.efi %1 %2 authorized_keys credentials %3 - %4 maxpwfail=%5 capafter=%6 banner=banner.txt bannerafter=banner2.txt
