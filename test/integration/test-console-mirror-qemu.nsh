@echo -off
echo Running console-mirror self-test...
AxlTestNet.efi mirror-selftest
stall 1000000
