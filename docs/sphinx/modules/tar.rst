AxlTar — ustar Archive Reader/Writer
====================================

A small POSIX ustar codec layered on :doc:`AxlStream <stream>`: write a
sequence of named byte blobs into a tar archive, or read them back,
streaming through any stream — an in-memory buffer (``axl_bufopen``), a
file (``axl_fopen``), or anything else backing the stream API.

The writer (``axl_tar_writer_new`` → ``axl_tar_writer_add`` /
``axl_tar_writer_add_dir`` → ``axl_tar_writer_finish``) emits standard
ustar headers with a correct checksum and block padding; names longer
than 100 bytes use the ustar name/prefix split (up to ~255 bytes), and a
name that can't be split is rejected rather than truncated. The reader
(``axl_tar_reader_new`` → ``axl_tar_reader_next`` → ``axl_tar_reader_read``)
validates each header checksum and streams entry data, stopping at the
end-of-archive marker.

It backs the ``tar`` host tool and mkfixture's HTTP write target (which
builds a fixture tarball in memory and POSTs it). GNU/PAX long-name and
sparse extensions are out of scope.

Header: ``<axl/axl-tar.h>``

API Reference
-------------

.. doxygenfile:: axl-tar.h
