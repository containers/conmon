#!/bin/bash
# this script is based off of the similarly named in github.com/containers/libpod/hack/tree_status.sh

set -e

SUGGESTION="${SUGGESTION:-call 'make config' and commit all changes.}"

STATUS=$(git status --porcelain)
if [[ -z $STATUS ]]
then
	echo "tree is clean"
else
	echo "tree is dirty, please $SUGGESTION"
	echo ""
	echo "$STATUS"
	exit 1
fi
