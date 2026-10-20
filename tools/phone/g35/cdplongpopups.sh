#!/bin/bash
# cdplongpopups.sh TAG [SECONDS=1000] [EVERY=180] - cdplong.sh plus the DWM swap test (pacingab.sh = cmd + d3dswap
# windows over the video) every EVERY s, as in the failing videosoak run - does covering the video break the decoder?
TAG=${1:?tag}; W=${2:-1000}; E=${3:-180}
bash tools/phone/g35/cdplong.sh "$TAG" "$W" &
P=$!; sleep 60
end=$(( $(date +%s) + W - 60 ))
while [ "$(date +%s)" -lt "$end" ] && kill -0 $P 2>/dev/null; do
  echo "$(date +%T) popup: $(bash tools/phone/g18/pacingab.sh "$TAG-pop" 1 2>&1 | grep -o '= [0-9.]*/s' | tr -d '=/s ' | paste -sd' ')"
  sleep "$E"
done
wait $P
