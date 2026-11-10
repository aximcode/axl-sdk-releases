@echo -off
# test-service-driver.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
service_demo.efi start --detach
service_demo.efi stop
# Short-flag + CHOICE round-trip (exercises AxlConfigDesc.short_name
# and AxlConfigDesc.choices passthrough in the axl_service_main
# synthesizer): -p 9090 short flag, -v short flag, --name alpha
# is a valid choice. Driver's setup log line should reflect all
# three values post-LoadOptions decode.
service_demo.efi start -p 9090 -v --name alpha --detach
service_demo.efi stop
service_demo.efi start --detach
