#!/bin/sh
# Host-side network prerequisites for the ubuntu26 QETH device (192.168.100.0/24).
# Run this once per host boot, before ./start.sh ubuntu26 -- iptables rules and
# ip_forward don't survive a host reboot, so this needs re-running after one.
set -e

SUBNET=192.168.100.0/24
UPLINK=$(ip route show default 0.0.0.0/0 | awk '{print $5; exit}')

if [ -z "$UPLINK" ]; then
    echo "net-up.sh: could not detect default-route interface" >&2
    exit 1
fi

sudo sysctl -w net.ipv4.ip_forward=1

sudo iptables -t nat -C POSTROUTING -s "$SUBNET" -o "$UPLINK" -j MASQUERADE 2>/dev/null \
    || sudo iptables -t nat -A POSTROUTING -s "$SUBNET" -o "$UPLINK" -j MASQUERADE

sudo iptables -C FORWARD -s "$SUBNET" -j ACCEPT 2>/dev/null \
    || sudo iptables -A FORWARD -s "$SUBNET" -j ACCEPT

sudo iptables -C FORWARD -d "$SUBNET" -m state --state ESTABLISHED,RELATED -j ACCEPT 2>/dev/null \
    || sudo iptables -A FORWARD -d "$SUBNET" -m state --state ESTABLISHED,RELATED -j ACCEPT

echo "net-up.sh: ip_forward=1, NAT via $UPLINK for $SUBNET ready."
