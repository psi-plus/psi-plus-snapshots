TEMPLATE = subdirs

include(libbase.pri)

sub_irisnet.subdir = irisnet
sub_xmpp.subdir = xmpp
sub_xmpp.depends = sub_irisnet

SUBDIRS += sub_irisnet
!iris_bundle:SUBDIRS += sub_xmpp
