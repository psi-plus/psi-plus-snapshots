This directories includes copies of source files
taken from https://github.com/BLAKE2/BLAKE2 at rev 320c325

The copy was required because we neither current Qt no QCA support BLAKE2
algorithm at the moment of this writing. It's supported by OpenSSL though
which is now under Apache2 but we don't link it directly.
So it's proposed to keep the copies here until either Qt or QCA get support
for the algo. Note it has to be done eventually if we need optimized
versions.

Copied files: blake2b-ref.c blake2s-ref.c blake2.h blake2-impl.h
The copied files is matter of CC0 1.0 Universal license
https://raw.githubusercontent.com/BLAKE2/BLAKE2/master/COPYING

Any other files in this directory just wrap the copies to have Qt interface.
