#!/bin/sh
#
# ipsec.cgi -- IPsec/IKEv2 status page for the OpenIPC WebUI.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Feature detection first: without the daemon this page renders nothing but a
# short note. A control that has no runtime behind it is worse than an absent
# one -- the user cannot tell "off" from "broken".
#
# Deliberate limits, because this runs as root behind a web server:
#
#   * No configuration editing. The config holds a pre-shared key; putting it
#     through a CGI would mean the secret crosses the browser, lands in POST
#     bodies and possibly in logs. Editing stays at /etc/weirdike/weirdike.conf
#     with mode 0600.
#   * The only actions are three fixed words, compared with `=` against a
#     whitelist and never interpolated into a command line. There is no path
#     here by which request data reaches a shell.
#   * Nothing secret is printed: weirdikectl's status carries no key material.

DAEMON=/usr/sbin/weirdiked
CTL=/usr/sbin/weirdikectl
INIT=/etc/init.d/S99weirdike

printf 'Content-Type: text/html; charset=utf-8\r\n\r\n'

if [ ! -x "$DAEMON" ] || [ ! -x "$CTL" ]; then
    echo '<p>IPsec is not installed on this camera.</p>'
    exit 0
fi

# ---- action, whitelisted -----------------------------------------------------
action=""
if [ "$REQUEST_METHOD" = "POST" ] && [ -n "$CONTENT_LENGTH" ]; then
    # Bounded read: a status page never needs more than this, and an unbounded
    # read from the network is how a CGI becomes a denial of service.
    body=$(dd bs=1 count=64 2>/dev/null)
    case "$body" in
        action=connect)    action=connect ;;
        action=disconnect) action=disconnect ;;
        action=rekey)      action=rekey ;;
        *)                 action="" ;;
    esac
fi

msg=""
case "$action" in
    connect)    "$INIT" start  >/dev/null 2>&1; msg="Connect requested." ;;
    disconnect) "$INIT" stop   >/dev/null 2>&1; msg="Disconnect requested." ;;
    rekey)      "$CTL"  rekey  >/dev/null 2>&1; msg="Child rekey requested." ;;
esac

# ---- status ------------------------------------------------------------------
status=$("$CTL" status 2>/dev/null)
rc=$?

esc() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }

echo '<h2>IPsec (IKEv2)</h2>'
[ -n "$msg" ] && echo "<p><em>$msg</em></p>"

if [ $rc -ne 0 ]; then
    echo '<p><strong>Not running.</strong></p>'
    if [ ! -c /dev/net/tun ]; then
        echo '<p>The tun device is missing. The kernel module ships with this'
        echo 'image but is not loaded; add a line <code>tun</code> to'
        echo '<code>/etc/modules</code> and reboot.</p>'
    fi
    if [ ! -f /etc/weirdike/weirdike.conf ]; then
        echo '<p>No <code>/etc/weirdike/weirdike.conf</code> yet. Copy the'
        echo 'example, fill in gateway and PSK, and <code>chmod 600</code> it.</p>'
    fi
    echo '<form method="post"><button name="action" value="connect">Connect</button></form>'
    exit 0
fi

echo '<table>'
echo "$status" | while IFS='=' read -r k v; do
    [ -n "$k" ] || continue
    ek=$(printf '%s' "$k" | esc)
    ev=$(printf '%s' "$v" | esc)
    echo "<tr><td>$ek</td><td>$ev</td></tr>"
done
echo '</table>'

echo '<form method="post">'
echo '<button name="action" value="disconnect">Disconnect</button>'
echo '<button name="action" value="rekey">Rekey now</button>'
echo '</form>'
