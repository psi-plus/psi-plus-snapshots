This is a stringprep library extracted from LibIDN and slightly rewritten to use Qt's unicode support instead glib's one.

The current version corresponds to 86e84739c5186faf3722a0f42e1e2db27870b3a5 commit of git://git.savannah.gnu.org/libidn.git

The necessity of usage of separate stringprep library is described here: https://gitlab.com/libidn/libidn2/-/issues/28

Note this directory contains generated rfc3454 files from rfc3454.txt. It's very unlikely these files will ever be regenerated but just in case the directory also contains both rfc3454.txt and a perl script to generate the files.
