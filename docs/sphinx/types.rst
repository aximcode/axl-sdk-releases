Shared Types
============

Callback and function pointer types used across AXL modules. These
follow GLib naming conventions where applicable.

Core Types
----------

.. list-table::
   :header-rows: 1
   :widths: 25 40 35

   * - Type
     - Signature
     - Header
   * - ``AxlDestroyNotify``
     - ``void (*)(void *data)``
     - ``<axl/axl-list.h>``
   * - ``AxlFunc``
     - ``void (*)(void *data, void *user_data)``
     - ``<axl/axl-list.h>``
   * - ``AxlCompareFunc``
     - ``int (*)(const void *a, const void *b)``
     - ``<axl/axl-array.h>``
   * - ``AxlCompareDataFunc``
     - ``int (*)(const void *a, const void *b, void *user_data)``
     - ``<axl/axl-array.h>``
   * - ``AxlCopyFunc``
     - ``void *(*)(const void *src, void *user_data)``
     - ``<axl/axl-list.h>``

Hash Table Types
----------------

.. list-table::
   :header-rows: 1
   :widths: 30 40 30

   * - Type
     - Signature
     - Header
   * - ``AxlHashFunc``
     - ``size_t (*)(const void *key)``
     - ``<axl/axl-hash-table.h>``
   * - ``AxlEqualFunc``
     - ``bool (*)(const void *a, const void *b)``
     - ``<axl/axl-hash-table.h>``
   * - ``AxlHashTableForeachFunc``
     - ``void (*)(const void *key, void *value, void *data)``
     - ``<axl/axl-hash-table.h>``
   * - ``AxlHashTableForeachRemoveFunc``
     - ``bool (*)(const void *key, void *value, void *data)``
     - ``<axl/axl-hash-table.h>``

Event and Task Types
--------------------

.. list-table::
   :header-rows: 1
   :widths: 25 40 35

   * - Type
     - Signature
     - Header
   * - ``AxlLoopCallback``
     - ``bool (*)(void *data)``
     - ``<axl/axl-loop.h>``
   * - ``AxlKeyCallback``
     - ``bool (*)(uint16_t key, void *data)``
     - ``<axl/axl-loop.h>``
   * - ``AxlDeferCallback``
     - ``void (*)(void *data)``
     - ``<axl/axl-defer.h>``
   * - ``AxlPubsubCallback``
     - ``void (*)(void *event_data, void *user_data)``
     - ``<axl/axl-pubsub.h>``
   * - ``AxlSignalHandler``
     - ``void (*)(void)``
     - ``<axl/axl-signal.h>``
   * - ``AxlAtexitFn``
     - ``void (*)(void *data)``
     - ``<axl/axl-atexit.h>``
   * - ``AxlTaskProc``
     - ``void (*)(void *arg)``
     - ``<axl/axl-task.h>``
   * - ``AxlTaskComplete``
     - ``void (*)(void *arg, void *result)``
     - ``<axl/axl-task.h>``

I/O and Network Types
---------------------

.. list-table::
   :header-rows: 1
   :widths: 25 40 35

   * - Type
     - Signature
     - Header
   * - ``AxlProgressFunc``
     - ``void (*)(uint64_t done, uint64_t total, void *ctx)``
     - ``<axl/axl-stream.h>``, ``<axl/axl-fs.h>``
   * - ``AxlLogHandler``
     - ``void (*)(int level, const char *domain, const char *msg, void *data)``
     - ``<axl/axl-log.h>``
   * - ``AxlHttpHandler``
     - ``int (*)(AxlHttpRequest *req, AxlHttpResponse *resp, void *ctx)``
     - ``<axl/axl-http-server.h>``
   * - ``AxlTcpCallback``
     - ``void (*)(AxlTcp *sock, int status, void *data)``
     - ``<axl/axl-tcp.h>``
   * - ``AxlUdpRecvCallback``
     - ``void (*)(void *buf, size_t len, void *data)``
     - ``<axl/axl-udp.h>``
   * - ``AxlConfigApplyFunc``
     - ``int (*)(void *target, const char *key, const char *value)``
     - ``<axl/axl-config.h>``

Built-in Hash/Equal Functions
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 45 30

   * - Function
     - Purpose
     - Header
   * - ``axl_str_hash``
     - FNV-1a hash for NUL-terminated strings
     - ``<axl/axl-hash-table.h>``
   * - ``axl_str_equal``
     - String equality (strcmp == 0)
     - ``<axl/axl-hash-table.h>``
   * - ``axl_direct_hash``
     - Hash a pointer/integer value directly
     - ``<axl/axl-hash-table.h>``
   * - ``axl_direct_equal``
     - Pointer equality (a == b)
     - ``<axl/axl-hash-table.h>``
