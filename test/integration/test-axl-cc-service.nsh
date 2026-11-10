@echo -off
# test-axl-cc-service.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
# Out-of-range --port (range [1,65535] declared on the config
# descriptor) must be rejected by the SYNTHESIZED CLI at parse time,
# before the service starts -- proving axl_service_main propagated the
# descriptor's min/max into the AxlArgDesc it builds.
service_demo.efi start --port 99999
service_demo.efi start --detach
