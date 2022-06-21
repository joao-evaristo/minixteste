#!/bin/sh
#
# Copyright (C) 2014  Internet Systems Consortium, Inc. ("ISC")
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
# OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

# Id: tests.sh,v 1.3 2010/06/08 23:50:24 tbox Exp 

SYSTEMTESTTOP=..
. $SYSTEMTESTTOP/conf.sh

DIGOPTS="+tcp +noadd +nosea +nostat +nocmd +dnssec -p 5300"

status=0
ret=0

alg=rsa
zonefile=ns1/rsa.example.db 
echo "I:testing PKCS#11 key generation (rsa)"
count=`$PK11LIST | grep robie-rsa-ksk | wc -l`
if [ $count != 2 ]; then echo "I:failed"; status=1; fi

echo "I:testing offline signing with PKCS#11 keys (rsa)"

count=`grep RRSIG $zonefile.signed | wc -l`
if [ $count != 12 ]; then echo "I:failed"; status=1; fi

echo "I:testing inline signing with PKCS#11 keys (rsa)"

$NSUPDATE > /dev/null <<END || status=1
server 10.53.0.1 5300
ttl 300
zone rsa.example.
update add `grep -v ';' ns1/${alg}.key`
send
END

echo "I:waiting 20 seconds for key changes to take effect"
sleep 20

$DIG $DIGOPTS ns.rsa.example. @10.53.0.1 a > dig.out || ret=1
if [ $ret != 0 ]; then echo "I:failed"; fi
status=`expr $status + $ret`
count=`grep RRSIG dig.out | wc -l`
if [ $count != 4 ]; then echo "I:failed"; status=1; fi

echo "I:testing PKCS#11 key destroy (rsa)"
ret=0
$PK11DEL -l robie-rsa-ksk -w0 > /dev/null 2>&1 || ret=1
$PK11DEL -l robie-rsa-zsk1 -w0 > /dev/null 2>&1 || ret=1
$PK11DEL -i $id -w0 > /dev/null 2>&1 || ret=1
if [ $ret != 0 ]; then echo "I:failed"; fi
status=`expr $status + $ret`
count=`$PK11LIST | grep robie-rsa | wc -l`
if [ $count != 0 ]; then echo "I:failed"; fi
status=`expr $status + $count`

echo "I:exit status: $status"
exit $status
