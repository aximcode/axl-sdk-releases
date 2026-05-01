AxlWatchdog — boot-services watchdog
=====================================

Wrapper for ``gBS->SetWatchdogTimer`` (UEFI 2.11 §7.5). UEFI starts
every loaded image with a 5-minute boot-services watchdog armed;
long-running diagnostics need to disarm or extend the timer or
they're killed without warning.

API Reference
-------------

.. doxygenfile:: axl-watchdog.h
