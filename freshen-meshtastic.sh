#!/bin/bash
set -euo pipefail

git fetch meshtastic develop:develop

current_branch=$(git rev-parse --abbrev-ref HEAD)
trap 'git checkout "$current_branch"' EXIT

branches=$(git for-each-ref --format='%(refname:short)' refs/heads/meshenvy/)

if [[ -z $branches ]]; then
	echo "No local branches matching meshenvy/* found."
	exit 0
fi

for branch in $branches; do
	echo "Updating $branch"
	git checkout "$branch"
	if ! git merge --ff-only develop; then
		git merge develop
	fi
done

git checkout "$current_branch"
