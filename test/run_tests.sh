#!/bin/sh
# SPDX-License-Identifier: MIT
#
# sdr-slc-rtld — example / reference code, published so you can build your
# own SDR-SLC device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).

# Build the stub daemon and run every suite against it.  No dongle needed.
# Equivalent to `make check`; kept so the old invocation still works.
set -u
cd "$(dirname "$0")/.."
exec make check
