#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
# Deletes every GitHub Actions cache entry of this ref whose key starts with $1 except the newest one. The ccache steps save a new entry for every
# commit (the key ends in the sha), the next run restores the newest one by prefix, and the older ones only fill the repository's 10 GB, so useful
# entries of other branches were evicted. Run after the save step; needs GH_TOKEN with actions: write.
set -u
PREFIX=${1:?usage: prune_caches.sh <key prefix>}
REPO=${GITHUB_REPOSITORY:?}
REF=${GITHUB_REF:?}
ids=$(gh cache list --repo "$REPO" --ref "$REF" --key "$PREFIX" --sort created_at --order desc --limit 100 --json id,key \
      --jq ".[] | select(.key | startswith(\"$PREFIX\")) | .id" | tail -n +2)
for id in $ids; do gh cache delete "$id" --repo "$REPO" || true; done
echo "pruned $(echo "$ids" | grep -c .) older entries of $PREFIX on $REF"
