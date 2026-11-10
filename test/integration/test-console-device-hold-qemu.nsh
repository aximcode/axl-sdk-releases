@echo -off
set -v T cdh
probe.efi
probe.efi decoy
load console-device-hold.efi
probe.efi
echo %T%-arm-break
loop.nsh
echo %T%-after-loop
echo %T%-ping
stall 300000
echo %T%-arm-busy
busy.nsh
echo %T%-after-busy
echo %T%-ping
stall 300000
echo %T%-release
stall 500000
probe.efi
echo %T%-end
