#!/bin/sh
# Test: does netifd still dial through the pty proxy, and does SMS poll without killing data?
killall hh500v-mbimprox 2>/dev/null
sleep 2
killall -9 hh500v-mbimprox 2>/dev/null
umount /dev/wwan0mbim0 2>/dev/null
echo "binary: $(ls -l /tmp/hh500v-mbimprox | awk '{print $5}') bytes"
ps w | grep mbimprox | grep -v grep | wc -l
: > /tmp/hh500v-sms-pdu.txt
: > /tmp/hh500v-sms-raw.txt
: > /tmp/hh500v-mbimprox.log
: > /tmp/mbimprox-out.log

echo "=== stop data ==="
ifdown wwan
sleep 4
ip -4 addr show wwan0 2>/dev/null | grep inet || echo "(no inet, expected)"

echo "=== start proxy ==="
/tmp/hh500v-mbimprox -d /dev/wwan0mbim0 -i 20 >> /tmp/mbimprox-out.log 2>&1 &
sleep 2
ls -l /dev/wwan0mbim0
grep wwan0mbim0 /proc/mounts || echo "!! not bind-mounted"

echo "=== dial through proxy ==="
ifup wwan
sleep 18
ip -4 addr show wwan0 2>/dev/null | grep inet || echo "!! no inet"
echo "ping: $(ping -c 2 -W 4 223.5.5.5 >/dev/null 2>&1 && echo OK || echo FAIL)"

echo "=== wait for SMS poll ==="
sleep 25
echo "ping: $(ping -c 2 -W 4 223.5.5.5 >/dev/null 2>&1 && echo OK || echo FAIL)"
ip -4 addr show wwan0 2>/dev/null | grep inet

echo "=== proxy log ==="
tail -40 /tmp/hh500v-mbimprox.log
echo "=== sms raw ==="
tail -5 /tmp/hh500v-sms-raw.txt 2>/dev/null || echo "(none)"
echo "=== sms pdu ==="
tail -5 /tmp/hh500v-sms-pdu.txt 2>/dev/null || echo "(none)"
echo "=== stdout tail ==="
tail -10 /tmp/mbimprox-out.log
