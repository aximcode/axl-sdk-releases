AxlRand — deterministic pseudo-random numbers
=============================================

Mirrors GLib's ``GRand``: a seedable, reproducible PRNG
(xoshiro256** seeded through SplitMix64) independent of any firmware
entropy source. It is the deterministic complement to
:doc:`rng` — reach for AxlRand when you want repeatable streams (test
fixtures, sampling, retry-backoff jitter, procedural graphics,
shuffling) and for ``axl_rng_bytes`` when you need cryptographic
material.

A given seed produces a byte-identical stream on x86-64 and AArch64.
The output is defined as a sequence of 64-bit words and every call is
pinned to it: ``axl_rand_uint32`` returns the high 32 bits of the next
word, ``axl_rand_double`` maps the top 53 bits into ``[0, 1)``, and
``axl_rand_bytes`` emits successive words little-endian. ``int_range``
is unbiased (rejection-sampled) and ``double_range`` rounds its scaled
product before adding the offset so no fused multiply-add can perturb
the low bit.

A process-global stream (``axl_random_*``, mirroring ``g_random_*``)
is lazily seeded from a non-reproducible source; call
``axl_random_set_seed`` first for a deterministic global sequence.

.. code-block:: c

   AXL_AUTOPTR(AxlRand) r = axl_rand_new_seeded(0x1234);
   uint32_t a = axl_rand_uint32(r);
   int      d = axl_rand_int_range(r, 1, 7);   // dice roll, [1,7)
   double   t = axl_rand_double(r);            // [0.0, 1.0)

API Reference
-------------

.. doxygenfile:: axl-rand.h
