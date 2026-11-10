@echo -off
echo Loading the service that will not stop...
load reload-svc-stuck-dxe.efi

echo Reloading it from this Shell command...
reload-svc.efi
echo RELOAD-DONE
