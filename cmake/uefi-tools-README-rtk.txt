
sshd-rtk.efi
------------
sshd-rtk.efi is sshd.efi with Realtek's USB NIC driver, RtkUsbUndiDxe.efi,
embedded: on a box whose cabled NIC is a Realtek USB dongle the firmware has
no driver for, it brings that NIC up with nothing in drivers/<arch>. It is
run, configured (sshd.conf), stopped and queried exactly as sshd.efi is --
the two are one service. The embedded driver is Realtek's own binary,
included as Realtek distributes it; it is not covered by this SDK's licence.

sshd-full-rtk.efi is the same with SftpServer.efi embedded as well (as in
sshd-full.efi): one file for the Shell, sftp and scp on such a box.
