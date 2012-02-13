TEMPLATE = subdirs

include(../libbase.pri)

sub_corelib.subdir = corelib
sub_appledns.subdir = appledns
sub_appledns.depends = sub_corelib
sub_noncore.subdir = noncore
!irisnetcore_bundle:sub_noncore.depends = sub_corelib

!irisnetcore_bundle:SUBDIRS += sub_corelib
appledns:!appledns_bundle:SUBDIRS += sub_appledns
!iris_bundle:SUBDIRS += sub_noncore
