@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo TOOL-LS
9p.efi ls 10.0.2.2:%1 /
echo TOOL-GET-STDOUT
9p.efi get 10.0.2.2:%1 /hello.txt
echo TOOL-GET-FILE
9p.efi get 10.0.2.2:%1 /readonly.txt got.txt
type got.txt
echo TOOL-PUT
9p.efi put putsrc.txt 10.0.2.2:%1 /puttest.txt
echo TOOL-PUT-RB
9p.efi get 10.0.2.2:%1 /puttest.txt
echo TOOL-MOUNT
9p.efi mount 10.0.2.2 --port %1
echo TOOL-MOUNT-STATUS
9p.efi status
echo TOOL-MOUNT-READ
type fs20:\dir\a.txt
echo TOOL-UMOUNT
9p.efi umount
9p.efi status
echo TOOL-UMOUNT-DONE
type fs20:\dir\a.txt
echo TOOL-DONE
