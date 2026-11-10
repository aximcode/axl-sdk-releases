# drivers/crashhandler -- the crash-handler driver.
axl_object(axl_crashhandler_objs NONE entrypoint.c exception.c report.c)
axl_includes_after(axl_crashhandler_objs drivers/crashhandler)

axl_driver_image(drivers/crashhandler axl_crashhandler_objs)
