#!/bin/bash
# Clawdmeter no longer needs a host-side daemon: the board connects to WiFi
# itself and polls the Anthropic API directly with its own Claude login.
# This script now just points you at the right next step.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Clawdmeter setup ==="
echo ""
echo "There is no host daemon to install anymore — the board is standalone."
echo ""
echo "1) Flash the firmware:"
echo "     ./flash.sh <board-env> [/dev/ttyACM0]"
echo "   Run './flash.sh' with no args to list available board envs."
echo ""
echo "2) On first boot, join the WiFi network \"Clawdmeter-Setup\" from your"
echo "   phone or laptop, and use the captive portal to give the board your"
echo "   home WiFi credentials. It will reboot onto your network."
echo ""
echo "3) Open http://clawdmeter.local in a browser, click \"Log in to Claude\","
echo "   approve access, then paste the code back into the board's page."
echo ""
echo "See README.md for the full walkthrough."
echo ""
echo "(The old daemon/ directory is kept for history but is no longer used"
echo "or installed by this script.)"
