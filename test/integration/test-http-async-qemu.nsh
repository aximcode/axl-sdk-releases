@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo === TEST-GET-ASYNC-HTTP ===
AxlTestNet.efi get-async http://10.0.2.2:%1/hello

echo === TEST-POST-ASYNC-HTTP ===
AxlTestNet.efi post-async http://10.0.2.2:%1/echo async-post-body

echo === TEST-GET-ASYNC-HTTPS ===
AxlTestNet.efi get-async https://10.0.2.2:%2/hello

echo === TEST-POST-ASYNC-HTTPS ===
AxlTestNet.efi post-async https://10.0.2.2:%2/echo async-post-tls

echo === TEST-GET-SYNC-RTPL ===
AxlTestNet.efi get-sync-rtpl http://10.0.2.2:%1/hello

echo === TEST-GET-LARGE-SINGLE ===
AxlTestNet.efi get-size http://10.0.2.2:%1/large?size=1572864

echo === TEST-GET-LARGE-SINGLE-HTTPS ===
AxlTestNet.efi get-size https://10.0.2.2:%2/large?size=1572864

echo === TEST-END ===
