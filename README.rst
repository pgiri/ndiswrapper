ndiswrapper
##########

`ndiswrapper <http://ndiswrapper.sourceforge.net>`_ is an implementation
of Windows' Network Driver Interface (NDIS) for Linux kernel. With this,
Windows network drivers can be used in Linux kernel to support chipsets
that have no native implementation or not well supported in Linux.

Current implementation supports many chipsets / drivers, but it requires
Windows XP (NDIS 5) drivers. Newer Windows kernels use NDIS 6; there is
an (incomplete) implementation of
`NDIS 6 <https://github.com/pgiri/ndiswrapper/tree/ndisv6>`_, but it is
not useful to users as of now.

Links
-----
* `Project page <http://ndiswrapper.sourceforge.net>`_.
