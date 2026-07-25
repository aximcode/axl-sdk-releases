AxlAttempt — crash-safe attempt engine
=======================================

Breadcrumb + quarantine + bounded result log, for operations that can
hang or fault the box outright — loading an arbitrary firmware image,
initializing an option ROM, staging a SPI update.

The hazard is that the failure takes the whole machine with it: no
exception fires, no handler runs, nothing is written down, and the next
boot repeats the identical sequence and hangs identically. The engine
breaks that loop by writing the name of the thing about to be tried to
non-volatile storage *before* trying it. A breadcrumb that outlives its
attempt names the culprit on the next boot, which is then quarantined
and skipped so the run makes progress.

This is complementary to :doc:`crashrecord`, not an alternative to it.
``AxlCrashRecord`` dumps registers and a stack trace when an *exception*
fires; a hang raises no exception and a reset runs no handler, so it
captures nothing. The breadcrumb covers exactly the case a crash handler
cannot see.

The caller supplies the namespace, vendor GUID, key names and bounds, so
independent consumers cannot collide and a consumer with existing
on-disk state can name it exactly and keep it.

API Reference
-------------

.. doxygenfile:: axl-attempt.h
