@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo [TEST START]

echo [test-root]
rfbrowse.efi http://10.0.2.2:%1 --raw

echo [test-system]
rfbrowse.efi http://10.0.2.2:%1 -u admin -p password --raw system

echo [test-thermal]
rfbrowse.efi http://10.0.2.2:%1 -u admin -p password --raw thermal

echo [test-members]
rfbrowse.efi http://10.0.2.2:%1 -u admin -p password --members systems

echo [test-noauth]
rfbrowse.efi http://10.0.2.2:%1 --raw systems

echo [test-badpass]
rfbrowse.efi http://10.0.2.2:%1 -u admin -p wrong system

echo [test-json5-rejected]
rfbrowse.efi http://10.0.2.2:%1 --members /redfish/v1/JSON5Trap

echo [TEST DONE]
