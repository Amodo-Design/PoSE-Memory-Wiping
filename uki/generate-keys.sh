#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# generate-keys.sh — generate the Secure Boot signing pair that uki/build-uki.sh
# signs UKIs with (-s KEYDIR).
#
# Writes a self-signed RSA-2048 certificate and its private key:
#   KEYDIR/mok.key   private key, PEM, mode 0600   (never leaves this machine)
#   KEYDIR/mok.crt   certificate, PEM              (what sbsign and sbverify take)
#   KEYDIR/mok.der   certificate, DER              (what firmware enrolment takes)
#
# Enrol the certificate into the UEFI db: HTTP boot has the firmware load
# the UKI directly, and MOK certificates are only honoured for images loaded
# through shim, which isn't being used here.
#
# Usage: uki/generate-keys.sh [-o KEYDIR] [-n NAME] [-d DAYS]
#   -o KEYDIR   where to write the pair          (default keys)
#   -n NAME     certificate common name          (default "PoSE Boot Image Signing Cert")
#   -d DAYS     validity                         (default 3650)
#   -h          help
#
# Paths are relative to the repo root.  Needs: openssl.

set -euo pipefail

KEYDIR=keys
NAME="PoSE Boot Image Signing Cert"
DAYS=3650

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "o:n:d:h" opt; do
    case "$opt" in
        o) KEYDIR=$OPTARG ;;
        n) NAME=$OPTARG ;;
        d) DAYS=$OPTARG ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 0 ]] || { echo "error: unexpected argument '$1' (see -h)" >&2; exit 1; }
command -v openssl >/dev/null || { echo "error: openssl is not installed" >&2; exit 1; }

cd "$(dirname "$(readlink -f "$0")")/.."

# Refuse to overwrite an existing key, a new pair means re-enrolling on every
# target that trusts the old one!
for f in mok.key mok.crt mok.der; do
    [[ -e $KEYDIR/$f ]] && { echo "error: $KEYDIR/$f exists; refusing to overwrite (move it aside first)" >&2; exit 1; }
done

mkdir -p "$KEYDIR"
chmod 700 "$KEYDIR"
openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
    -subj "/CN=$NAME/" -keyout "$KEYDIR/mok.key" -out "$KEYDIR/mok.crt" 2>/dev/null
chmod 600 "$KEYDIR/mok.key"
openssl x509 -in "$KEYDIR/mok.crt" -outform DER -out "$KEYDIR/mok.der"

echo "== generated: $KEYDIR/{mok.key,mok.crt,mok.der}"
openssl x509 -in "$KEYDIR/mok.crt" -noout -subject -enddate | sed 's/^/   /'
echo "   sign with:  uki/build-uki.sh ... -s $KEYDIR"
