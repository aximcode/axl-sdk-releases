@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo === TEST-HTTPS-GET ===
fetch.efi https://10.0.2.2:%1/hello

echo === TEST-HTTPS-REDIRECT ===
fetch.efi http://10.0.2.2:%2/redirect

echo === TEST-TLS-CLOSE-NOTIFY ===
fetch.efi https://10.0.2.2:%1/resp/te-close-notify

echo === TEST-TLS-CLOSE-CUT ===
fetch.efi https://10.0.2.2:%1/resp/te-close-nonotify

echo === TEST-END ===
