#!/bin/bash

set -e

RED="\e[1;36;41m"
YEL="\e[1;33;44m"
NOR="\e[0m"
USAGE_WARNING="
${YEL}WARNING: This will not work without local sudo access to run podman,${NOR}
         ${YEL}and prior authorization to use the conmon GCP project. Also,${NOR}
         ${YEL}possession of the proper ssh private key is required.${NOR}
"
# TODO: Many/most of these values should come from .cirrus.yml
ZONE="us-central1-f"
CPUS="4"
MEMORY="16Gb"
DISK="200"
PROJECT="conmon-222014"
SRC="/tmp/conmon"
SSHUSER="${SSHUSER:-root}"
# Command shortcuts save some typing
PGCLOUD="sudo podman run -it --rm -e AS_ID=$UID -e AS_USER=$USER --security-opt label=disable -v /home/$USER:$HOME -v /tmp:/tmp:ro quay.io/cevich/gcloud_centos:latest --configuration=conmon --project=$PROJECT"
SCP_CMD="$PGCLOUD compute scp"

CONMONROOT=$(realpath "$(dirname $0)/../")
# else: Assume $PWD is the root of the conmon repository
[[ "$CONMONROOT" != "/" ]] || CONMONROOT=$PWD

showrun() {
    if [[ "$1" == "--background" ]]
    then
        shift
        # Properly escape any nested spaces, so command can be copy-pasted
        echo '+ '$(printf " %q" "$@")' &' > /dev/stderr
        "$@" &
        echo -e "${RED}<backgrounded>${NOR}"
    else
        echo '+ '$(printf " %q" "$@") > /dev/stderr
        "$@"
    fi
}

TEMPFILE=$(mktemp -p '' $(basename $0)_XXXXX.tar.bz2)
cleanup() {
    set +e
    wait
    rm -f "$TEMPFILE"
}
trap cleanup EXIT

delvm() {
    cleanup
    echo -e "\n"
    echo -e "\n${YEL}Offering to Delete $VMNAME ${RED}(Might take a minute or two)${NOR}"
    showrun $CLEANUP_CMD  # prompts for Yes/No
}

image_hints() {
    egrep '[[:space:]]+[[:alnum:]].+_CACHE_IMAGE_NAME:[[:space:]+"[[:print:]]+"' \
        "$CONMONROOT/.cirrus.yml" | cut -d: -f 2 | tr -d '"[:blank:]' | \
        grep -v 'notready' | grep -v 'image-builder' | sort -u
}

show_usage(){
    echo -e "\n${RED}ERROR: $1${NOR}"
    echo -e "${YEL}Usage: $(basename $0) [-s | -p] <image_name>${NOR}\n"
    if [[ -r ".cirrus.yml" ]]
    then
        echo -e "${YEL}Some possible image_name values (from .cirrus.yml):${NOR}"
        image_hints
        echo ""
    fi
    exit 1
}

get_env_vars() {
    python -c '
import yaml
env=yaml.load(open(".cirrus.yml"))["env"]
keys=[k for k in env if "ENCRYPTED" not in str(env[k])]
for k,v in env.items():
    v=str(v)
    if "ENCRYPTED" not in v:
        print "{0}=\"{1}\"".format(k, v),
    '
}

parse_args(){
    if [[ -z "$1" ]]
    then
        show_usage "Must specify image name for VM."
    else  # no -s or -p
        DEPS="$(get_env_vars)"
        IMAGE_NAME="$1"
    fi

    if [[ "$USER" =~ "root" ]]
    then
        show_usage "This script must be run as a regular user."
    fi

    echo -e "$USAGE_WARNING"

    SETUP_CMD="env $DEPS $SRC/contrib/cirrus/setup_environment.sh"
    VMNAME="${VMNAME:-${USER}-${IMAGE_NAME}}"
    CREATE_CMD="$PGCLOUD compute instances create --zone=$ZONE --image=${IMAGE_NAME} --custom-cpu=$CPUS --custom-memory=$MEMORY --boot-disk-size=$DISK --labels=in-use-by=$USER $VMNAME"
    SSH_CMD="$PGCLOUD compute ssh $SSHUSER@$VMNAME"
    CLEANUP_CMD="$PGCLOUD compute instances delete --zone $ZONE --delete-disks=all $VMNAME"
}

##### main

parse_args $@

cd $CONMONROOT

# Attempt to determine if named 'conmon' gcloud configuration exists
showrun $PGCLOUD info > $TEMPFILE
if egrep -q "Account:.*None" "$TEMPFILE"
then
    echo -e "\n${YEL}WARNING: Can't find gcloud configuration for conmon, running init.${NOR}"
    echo -e "         ${RED}Please choose "#1: Re-initialize" and "login" if asked.${NOR}"
    showrun $PGCLOUD init --project=$PROJECT --console-only --skip-diagnostics

    # Verify it worked (account name == someone@example.com)
    $PGCLOUD info > $TEMPFILE
    if egrep -q "Account:.*None" "$TEMPFILE"
    then
        echo -e "${RED}ERROR: Could not initialize conmon configuration in gcloud.${NOR}"
        exit 5
    fi

    # If this is the only config, make it the default to avoid persistent warnings from gcloud
    [[ -r "$HOME/.config/gcloud/configurations/config_default" ]] || \
        ln "$HOME/.config/gcloud/configurations/config_conmon" \
           "$HOME/.config/gcloud/configurations/config_default"
fi

# Couldn't make rsync work with gcloud's ssh wrapper :(
echo -e "\n${YEL}Packing up repository into a tarball $VMNAME.${NOR}"
showrun --background tar cjf $TEMPFILE --warning=no-file-changed -C $CONMONROOT .

trap delvm INT  # Allow deleting VM if CTRL-C during create
# This fails if VM already exists: permit this usage to re-init
echo -e "\n${YEL}Trying to creating a VM named $VMNAME ${RED}(might take a minute/two.  Errors ignored).${NOR}"
showrun $CREATE_CMD || true # allow re-running commands below when "delete: N"

# Any subsequent failure should prompt for VM deletion
trap delvm EXIT

echo -e "\n${YEL}Waiting up to 30s for ssh port to open${NOR}"
ATTEMPTS=10
for (( COUNT=1 ; COUNT <= $ATTEMPTS ; COUNT++ ))
do
    if $SSH_CMD --command "true"; then break; else sleep 3s; fi
done
if (( COUNT > $ATTEMPTS ))
then
    echo -e "\n${RED}Failed${NOR}"
    exit 7
fi
echo -e "${YEL}Got it${NOR}"

if $SSH_CMD --command "test -r .bash_profile_original"
then
    echo -e "\n${YEL}Resetting environment configuration${NOR}"
    showrun $SSH_CMD --command "cp .bash_profile_original .bash_profile"
fi

echo -e "\n${YEL}Removing and re-creating $SRC on $VMNAME.${NOR}"
showrun $SSH_CMD --command "rm -rf $SRC"
showrun $SSH_CMD --command "mkdir -p $SRC"

echo -e "\n${YEL}Transfering tarball to $VMNAME.${NOR}"
wait
showrun $SCP_CMD $TEMPFILE $SSHUSER@$VMNAME:$TEMPFILE

echo -e "\n${YEL}Unpacking tarball into $SRC on $VMNAME.${NOR}"
showrun $SSH_CMD --command "tar xjf $TEMPFILE -C $SRC"

echo -e "\n${YEL}Removing tarball on $VMNAME.${NOR}"
showrun $SSH_CMD --command "rm -f $TEMPFILE"

echo -e "\n${YEL}Executing environment setup${NOR}"
showrun $SSH_CMD --command "exec env $DEPS $SETUP_CMD"

echo -e "\n${YEL}Connecting to $VMNAME ${RED}(option to delete VM upon logout).${NOR}\n"
showrun $SSH_CMD -- -t "cd $SRC && exec env $DEPS bash -il"
