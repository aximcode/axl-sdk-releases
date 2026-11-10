@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo === TEST-NETINFO-LIST ===
netinfo.efi list

echo === TEST-NETINFO-NO-LOAD ===
netinfo.efi -v --no-load list

echo === TEST-NETINFO-VERBOSE ===
netinfo.efi -v list

echo === TEST-NETINFO-LIST-BUNDLE ===
netinfo.efi list-bundle

echo === TEST-NETINFO-DIAG ===
netinfo.efi diag

echo === TEST-FETCH-GET ===
fetch.efi http://10.0.2.2:%1/hello

echo === TEST-FETCH-404 ===
fetch.efi http://10.0.2.2:%1/no-such-thing

echo === TEST-FETCH-REDIRECT ===
fetch.efi http://10.0.2.2:%1/redirect

echo === TEST-FETCH-SAVE ===
fetch.efi http://10.0.2.2:%1/hello -o fetched.txt
type fetched.txt

echo === TEST-END ===
