#!/bin/sh

find $(pwd) -name configure.ac | xargs touch

mkdir -vp m4

# Regenerate configuration files
aclocal -I m4 --install || exit 1
autoheader || exit 1
automake --foreign --include-deps --add-missing --copy || exit 1
autoreconf -i -f || exit 1

# Run configure for this platform
#./configure $*
