@echo -off
echo MLOG_OFF_BEGIN
app\mlog-off.efi
echo MLOG_OFF_RC=%lasterror%
echo MLOG_ON_BEGIN
app\mlog-on.efi
echo MLOG_DONE
