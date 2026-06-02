AxlShm — Boot-Persistent Shared Memory
======================================

``<axl/axl-shm.h>`` is the UEFI analog of POSIX ``shm_open`` + ``mmap``
(or System V ``shmget`` / ``shmat``): a named, fixed-size memory region
that outlives the app that created it and is reachable by any later app in
the same boot. UEFI is a single flat, identity-mapped address space, so
there is no per-process mapping — ``axl_shm_open`` returns a pointer that
*is* the region; map/attach collapse to nothing and detach is a no-op.

The region lives in boot-services RAM and is therefore volatile: it is
reclaimed at reboot / ``ExitBootServices``. Not NVRAM — no flash wear, and
its capacity is system RAM rather than the tiny firmware variable store.
The name is hashed to a GUID (``axl_guid_v5``) and the region published as
a UEFI protocol, so it resolves across images with no shared handle. It
survives an image unload because it is a *data-only* pool allocation (no
function pointers, which would dangle once the creating image is gone)
from ``EfiBootServicesData``, which the firmware does not reclaim on
unload — the same mechanism the backend uses to cache the calibrated TSC
frequency across processes.

Names are one global namespace (like POSIX ``/name``) — prefix yours.
Single-threaded (UEFI): no locking is provided; in the shell apps run one
at a time. ``AxlClipboard`` and the ``clip`` / ``paste`` tools are built
on it.

Header: ``<axl/axl-shm.h>``

API Reference
-------------

.. doxygenfile:: axl-shm.h
