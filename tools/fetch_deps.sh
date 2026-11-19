#!/usr/bin/env bash
# Download the pinned third-party tarballs into .deps/ (or $1) and verify them,
# so that `cmake` can configure without network access.
set -euo pipefail
dest="${1:-$(cd "$(dirname "$0")/.." && pwd)/.deps}"
mkdir -p "$dest"
while read -r file sha url; do
  [[ -z "$file" || "$file" == \#* ]] && continue
  if [[ ! -s "$dest/$file" ]]; then
    echo "fetch $file"
    curl -fL --retry 3 --connect-timeout 30 -o "$dest/$file.part" "$url"
    mv "$dest/$file.part" "$dest/$file"
  fi
  echo "$sha  $dest/$file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
done <<'LIST'
abseil-cpp-20240116.0.tar.gz 338420448b140f0dfd1a1ea3c3ce71b3bc172071f24f4d9a57d59b45037da440 https://github.com/abseil/abseil-cpp/archive/refs/tags/20240116.0.tar.gz
protobuf-29.6.tar.gz 877bf9f880631aa31daf2c09896276985696728137fcd43cc534a28c5566d9ba https://github.com/protocolbuffers/protobuf/releases/download/v29.6/protobuf-29.6.tar.gz
yaml-cpp-0.8.0.tar.gz fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16 https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
glog-0.6.0.tar.gz 8a83bf982f37bb70825df71a9709fa90ea9f4447fb3c099e1d720a439d88bad6 https://github.com/google/glog/archive/refs/tags/v0.6.0.tar.gz
apache-arrow-18.0.0.tar.gz abcf1934cd0cdddd33664e9f2d9a251d6c55239d1122ad0ed223b13a583c82a9 https://archive.apache.org/dist/arrow/arrow-18.0.0/apache-arrow-18.0.0.tar.gz
LIST
echo "all dependencies present in $dest"
