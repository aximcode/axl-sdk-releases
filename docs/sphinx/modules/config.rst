AxlConfig — Configuration
==========================

See :doc:`sys` for an overview of all utility modules including the
configuration framework.

Header: ``<axl/axl-config.h>``

API Reference
-------------

.. doxygenfile:: axl-config.h

AxlConfigFile — free-form ``key=value`` map
-------------------------------------------

The open-vocabulary counterpart to descriptor-bound ``AxlConfig``: parse a
``key=value`` text file (``#`` comments, blank lines, trimmed values) into a
flat string map with typed getters that fall back to a caller default for any
missing key. Use this when keys are not known at compile time (a
``softbmc.cfg``-style file where modules invent their own ``prefix.key``
names); use ``AxlConfig`` when the option set is fixed and typed.

Header: ``<axl/axl-config-file.h>``

.. doxygenfile:: axl-config-file.h
