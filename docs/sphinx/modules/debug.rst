AXL_DEBUG_ASSERT — debug-build invariant guards
================================================

``AXL_DEBUG_ASSERT(expr)`` enforces an *internal* invariant — a
condition the code's own logic guarantees, which a future edit could
quietly break. It is distinct from a runtime error check, which
validates *input* and returns an ``AxlStatus``; an assert documents
and enforces an internal contract and is compiled out under
``NDEBUG``, so release builds pay nothing.

Its purpose is to catch a concurrency or lifecycle fault **at its
cause**, in a debug or test build, instead of as a downstream symptom
(a wedge, a stream desync, a corrupted dispatch) that only a
consumer's integration surfaces later. See
``docs/AXL-Concurrency.md`` § "Testing the model" for how this fits
the broader strategy of modeling the consumer topology in-repo.

On failure (debug/test builds only) it emits a loud, grep-able log
line — ``AXL_DEBUG_ASSERT FAILED: <file>:<line> <func>(): <expr>`` —
and **continues**: it does not abort or wedge, so an integration test
can observe the marker and still run to its results footer.
``_axl_debug_assert_count()`` is a test hook to confirm a guard fired
without parsing logs.

API Reference
-------------

.. doxygenfile:: axl-debug.h
