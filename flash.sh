#!/bin/bash
# Build the sketch and upload it to the board.
#   ./flash.sh            compile + upload
#   ./flash.sh --compile  compile only
set -euo pipefail
cd "$(dirname "$0")"

SKETCH=windbird_display_1
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB"

CLI=$(command -v arduino-cli || true)
[ -n "$CLI" ] || CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
[ -x "$CLI" ] || { echo "arduino-cli not found (install it, or the Arduino IDE)"; exit 1; }

[ -f .env ] || { echo "No .env file - copy .env.example to .env and fill it in"; exit 1; }

# .env -> secrets.h (gitignored), escaping the values for C
python3 - "$SKETCH/secrets.h" <<'EOF'
import sys
env = {}
for line in open('.env'):
    line = line.rstrip('\n')
    if not line.strip() or line.lstrip().startswith('#') or '=' not in line:
        continue
    k, v = line.split('=', 1)
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
        v = v[1:-1]
    env[k.strip()] = v
missing = [k for k in ('WIFI_SSID', 'WIFI_PASS') if k not in env]
if missing:
    sys.exit('.env is missing: ' + ', '.join(missing))
esc = lambda v: v.replace('\\', '\\\\').replace('"', '\\"')
with open(sys.argv[1], 'w') as f:
    f.write('// Generated from .env by flash.sh - do not edit, do not commit\n')
    f.write('const char* WIFI_SSID = "%s";\n' % esc(env['WIFI_SSID']))
    f.write('const char* WIFI_PASS = "%s";\n' % esc(env['WIFI_PASS']))
EOF

if [ "${1:-}" = "--compile" ]; then
  "$CLI" compile --fqbn "$FQBN" "$SKETCH"
  exit
fi

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
[ -n "$PORT" ] || { echo "Board not found - plug it in with a data USB-C cable"; exit 1; }
"$CLI" compile --upload -p "$PORT" --fqbn "$FQBN" "$SKETCH"
