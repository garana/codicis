#!/bin/sh
# Recording shim used as codicis's storage.helper_cmd for the black-box test.
# It proxies the real reference helper unchanged while capturing both directions
# of the (text-codec) wire traffic to files, so the test can snapshot exactly
# what codicis emits to, and reads from, its storage helper.
#
#   argv: <real-helper> <capture-dir>
#
# stdin  (codicis -> helper) is tee'd to  <capture-dir>/to-storage.txt
# stdout (helper -> codicis) is tee'd to  <capture-dir>/from-storage.txt
set -eu
REAL="$1"
CAP="$2"
# GNU tee uses unbuffered read/write, so requests/responses forward promptly.
tee -a "$CAP/to-storage.txt" | "$REAL" | tee -a "$CAP/from-storage.txt"
