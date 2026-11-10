@echo -off
# test-clip-paste.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).

# 1. Copy text in ONE app, paste it in ANOTHER. clip prints nothing,
#    so 'axl-cross-app-clip' in the log can only have come from paste
#    reading what the separate clip invocation stored.
echo PROBE-1-START
echo axl-cross-app-clip | clip.efi
paste.efi
echo PROBE-1-END

# 2. A MIME tag set by clip is reported by a later paste --mime.
echo PROBE-2-START
echo tagged | clip.efi -m application/x-axl-test
paste.efi --mime
echo PROBE-2-END

# 3. clip --clear empties it across apps: the following paste prints
#    nothing between the markers.
echo PROBE-3-START
clip.efi --clear
paste.efi
echo PROBE-3-END

echo === DONE ===
