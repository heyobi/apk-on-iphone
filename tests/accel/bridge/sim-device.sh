#!/usr/bin/env bash
# Prints the UDID of an available simulator with this exact name, creating it on the newest iOS runtime when the
# runner image does not have one. If that runtime does not support the model, takes the newest of the same family
# ("iPhone NN Pro" → highest NN). Usage: sim-device.sh "<device name>"
set -euo pipefail
NAME_DEV=$1
DEV=$(xcrun simctl list devices available -j | python3 -c "
import json, sys
name = sys.argv[1]
d = json.load(sys.stdin)['devices']
for r in sorted([r for r in d if 'iOS' in r], reverse=True):
    for x in d[r]:
        if x['name'] == name:
            print(x['udid']); sys.exit()
" "$NAME_DEV")
if [ -z "$DEV" ]; then
  read -r TYPE RT < <(xcrun simctl list runtimes -j | python3 -c "
import json, re, sys
name = sys.argv[1]
rs = [r for r in json.load(sys.stdin)['runtimes'] if r['platform'] == 'iOS' and r.get('isAvailable')]
rs.sort(key=lambda r: [-int(x) for x in r['version'].split('.')])
for r in rs:
    types = r.get('supportedDeviceTypes', [])
    exact = [t for t in types if t['name'] == name]
    fam = re.sub('[0-9]+', lambda m: '[0-9]+', re.escape(name))
    similar = sorted([t for t in types if re.fullmatch(fam, t['name'])], key=lambda t: t['name'])
    pick = exact or similar[-1:]
    if pick:
        print(pick[0]['identifier'], r['identifier']); break
" "$NAME_DEV")
  echo "creating $TYPE on $RT" >&2
  DEV=$(xcrun simctl create "$NAME_DEV" "$TYPE" "$RT")
fi
echo "$DEV"
