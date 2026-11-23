#!/bin/sh
# Downloads Debian's official arm64 root file system - the `debian` image on Docker Hub
# (built by Debian's debuerreotype) - straight from the registry, without Docker. The
# image manifest and the layer are content-addressed: each is checked against its
# SHA-256 digest before it is used. Unpacks into DIR.
# usage: tests/linux/fetch-debian.sh DIR [TAG]   (TAG default: trixie-slim)
set -eu
DIR=$1
TAG=${2:-trixie-slim}
REG=https://registry-1.docker.io/v2/library/debian
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TOKEN=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/debian:pull" |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
H="Authorization: Bearer $TOKEN"
curl -fsSL -H "$H" -H "Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json" \
    -o "$TMP/index.json" "$REG/manifests/$TAG"
MD=$(python3 - "$TMP/index.json" <<'EOF'
import json, sys
idx = json.load(open(sys.argv[1]))
for m in idx["manifests"]:
    p = m.get("platform", {})
    if p.get("architecture") == "arm64" and p.get("os") == "linux":
        print(m["digest"]); break
EOF
)
[ -n "$MD" ] || { echo "no linux/arm64 manifest for debian:$TAG" >&2; exit 1; }
curl -fsSL -H "$H" -H "Accept: application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json" \
    -o "$TMP/manifest.json" "$REG/manifests/$MD"
GOT=sha256:$(sha256sum "$TMP/manifest.json" | cut -d' ' -f1)
[ "$GOT" = "$MD" ] || { echo "manifest digest mismatch: $GOT != $MD" >&2; exit 1; }
LAYERS=$(python3 -c 'import json,sys; print(" ".join(l["digest"] for l in json.load(open(sys.argv[1]))["layers"]))' "$TMP/manifest.json")
mkdir -p "$DIR"
for L in $LAYERS; do
    curl -fsSL -H "$H" -o "$TMP/layer" "$REG/blobs/$L"
    GOT=sha256:$(sha256sum "$TMP/layer" | cut -d' ' -f1)
    [ "$GOT" = "$L" ] || { echo "layer digest mismatch: $GOT != $L" >&2; exit 1; }
    echo "verified debian:$TAG arm64 manifest $MD layer $L"
    tar -xzf "$TMP/layer" -C "$DIR" --no-same-owner --exclude='dev/*'
done
echo "debian:$TAG $MD" > "$DIR/.lxrun-image"
