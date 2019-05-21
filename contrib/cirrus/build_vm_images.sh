#!/bin/bash

# This is assumed to be executed through ssh, on an VM running from
# the 'image-builder-image', by Cirrus CI.  Running manually requires
# setting all the 'req_env_var' items below, as well as
# $GOOGLE_APPLICATION_CREDENTIALS = JSON credentials file cooresponding
# to $SERVICE_ACCOUNT.

set -e
source $(dirname $0)/lib.sh

req_env_var "
SRC $SRC
SCRIPT_BASE $SCRIPT_BASE
PACKER_BASE $PACKER_BASE
PACKER_VER $PACKER_VER
PACKER_BUILDS $PACKER_BUILDS
BUILT_IMAGE_SUFFIX $BUILT_IMAGE_SUFFIX

CRIO_REPO $CRIO_REPO
CRIO_SLUG $CRIO_SLUG

FEDORA_BASE_IMAGE $FEDORA_BASE_IMAGE

SERVICE_ACCOUNT $SERVICE_ACCOUNT
GCE_SSH_USERNAME $GCE_SSH_USERNAME
GCP_PROJECT_ID $GCP_PROJECT_ID
"

show_env_vars

# Everything here is running on the 'image-builder-image' GCE image
# Assume basic dependencies are all met, but there could be a newer version
# of the packer binary
PACKER_FILENAME="packer_${PACKER_VER}_linux_amd64.zip"
# image_builder_image has packer pre-installed, check if same version requested
if [[ -r "$HOME/packer/$PACKER_FILENAME" ]]
then
    cp "$HOME/packer/$PACKER_FILENAME" "$SRC/$PACKER_BASE/"
fi

cd "$SRC/$PACKER_BASE"

# Separate PR-produced images from those produced on master.
if [[ "${CIRRUS_BRANCH:-}" == "master" ]]
then
    POST_MERGE_BUCKET_SUFFIX="-master"
else
    POST_MERGE_BUCKET_SUFFIX=""
fi

make conmon_images \
    SRC=$SRC \
    SCRIPT_BASE=$SCRIPT_BASE \
    PACKER_BASE=$PACKER_BASE \
    PACKER_VER=$PACKER_VER \
    PACKER_BUILDS=$PACKER_BUILDS \
    BUILT_IMAGE_SUFFIX=$BUILT_IMAGE_SUFFIX \
    CRIO_REPO=$CRIO_REPO \
    CRIO_SLUG=$CRIO_SLUG \
    FEDORA_BASE_IMAGE=$FEDORA_BASE_IMAGE \
    POST_MERGE_BUCKET_SUFFIX=$POST_MERGE_BUCKET_SUFFIX

# When successful, upload manifest of produced images using a filename unique
# to this build.
URI="gs://packer-import${POST_MERGE_BUCKET_SUFFIX}-temp/manifest${BUILT_IMAGE_SUFFIX}.json"
gsutil cp packer-manifest.json "$URI"

echo "Finished."
echo "Any tarball URI's referenced above at at $URI"
echo "may be used to create VM images suitable for use in"
echo ".cirrus.yml as values for the 'image_name' keys."
