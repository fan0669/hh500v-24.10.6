#!/bin/sh
# Long poll test: does SMS_READ ever succeed while data is up?
killall hh500v-mbimprox 2>/dev/null
sleep 2
killall -9 hh500v-mbimprox 2>/dev/null
umount /dev/wwan0mbim0 2>/dev/null
: > /tmp/hh500v-sms-pdu.txt
: > /tmp/hh500v-sms-raw.txt
: > /usr/bin/hh500v-mbimprox.log

echo "=== stop data ==="
ifdown wwan
sleep 4
echo "=== start proxy (5s poll) ==="
/usr/bin/hh500v-mbimprox -d /dev/wwan0mbim0 -i 5 >> /tmp/mbimprox-out.log 2>&1 &
sleep 2
grep wwan0mbim0 /proc/mounts || echo "!! not bind-mounted"
echo "=== dial ==="
ifup wwan
sleep 20
echo "ping1: $(ping -c 2 -W 4 223.5.5.5 >/dev/null 2>&1 && echo OK || echo FAIL)"
sleep 45
echo "ping2: $(ping -c 2 -W 4 223.5.5.5 >/dev/null 2>&1 && echo OK || echo FAIL)"
echo "=== status histogram ==="
grep -o 'status=0x[0-9a-f]*' /usr/bin/hh500v-mbimprox.log | sort | uniq -c
echo "=== successes ==="
grep -E 'sms count|SMS idx' /usr/bin/hh500v-mbimprox.log | head -10
echo "=== db ==="
wc -l < /tmp/hh500v-sms.txt 2>/dev/null || echo "(none)"
head -3 /tmp/hh500v-sms.txt 2>/dev/null
