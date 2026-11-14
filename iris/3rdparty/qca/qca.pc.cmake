prefix=@QCA_PREFIX_INSTALL_DIR@
exec_prefix=@QCA_PREFIX_INSTALL_DIR@
libdir=@QCA_PKGCONFIG_LIBRARY_DIR@
includedir=@QCA_PKGCONFIG_INCLUDE_DIR@

Name: QCA
Description: Qt Cryptographic Architecture library
Version: @QCA_LIB_VERSION_STRING@
Requires: @QCA_QT_PC_VERSION@
Libs: @PKGCONFIG_LIBS@
Cflags: @PKGCONFIG_CFLAGS@
