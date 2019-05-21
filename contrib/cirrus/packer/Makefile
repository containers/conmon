
PACKER_VER ?= 1.3.5
PACKER_DIST_FILENAME := packer_${PACKER_VER}_linux_amd64.zip

# Only needed for conmon_base_images target
TIMESTAMP := $(shell date +%s)
SRC ?= $(shell realpath "./../../../")
PACKER_BASE ?= ./contrib/cirrus/packer
SCRIPT_BASE ?= ./contrib/cirrus

# For debugging nested-virt, use
#TTYDEV := $(shell tty)
TTYDEV := /dev/null

.PHONY: all
all: conmon_images

%.json: %.yml
	@python3 -c 'import json,yaml; json.dump( yaml.load(open("$<").read()), open("$@","w"), indent=2);'

${PACKER_DIST_FILENAME}:
	@curl -L --silent --show-error \
		-O https://releases.hashicorp.com/packer/${PACKER_VER}/${PACKER_DIST_FILENAME}

packer: ${PACKER_DIST_FILENAME}
	@curl -L --silent --show-error \
		https://releases.hashicorp.com/packer/${PACKER_VER}/packer_${PACKER_VER}_SHA256SUMS \
		| grep 'linux_amd64' > /tmp/packer_sha256sums
	@sha256sum --check /tmp/packer_sha256sums
	@unzip -o ${PACKER_DIST_FILENAME}
	@touch --reference=Makefile ${PACKER_DIST_FILENAME}

.PHONY: test
test: conmon_base_images.json conmon_images.json packer
	./packer inspect conmon_base_images.json > /dev/null
	./packer inspect conmon_images.json > /dev/null
	@echo "All good"

.PHONY: check_common_env
check_common_env:
ifndef SRC
	$(error SRC is undefined, expected complete path to repository source directory.)
endif
ifndef SCRIPT_BASE
	$(error SCRIPT_BASE is undefined, expected relative path to cirrus scripts directory.)
endif
ifndef PACKER_BASE
	$(error PACKER_BASE is undefined, expected relative path to packer directory.)
endif
ifndef PACKER_VER
	$(error PACKER_VER is undefined, expected string version number of packer to use.)
endif
ifndef PACKER_BUILDS
	$(error PACKER_BUILDS is undefined, expected CSV of builders to utilize.)
endif
ifndef GCP_PROJECT_ID
	$(error GCP_PROJECT_ID is undefined, expected complete GCP project ID string e.g. foobar-12345.)
endif

.PHONY: cache_image_checkenv
cache_image_checkenv: check_common_env
ifndef GCE_SSH_USERNAME
	$(error GCE_SSH_USERNAME is undefined, expected user with ssh access to base-image.)
endif
ifndef RHSM_COMMAND
	$(error RHSM_COMMAND is undefined, expected complete command-line to register/subscribe RHEL VM.)
endif
ifndef SERVICE_ACCOUNT
	$(error SERVICE_ACCOUNT is undefined, expected GCE service account name for managing VMs.)
endif
ifndef BUILT_IMAGE_SUFFIX
	$(error BUILT_IMAGE_SUFFIX is undefined, expected suffix string to make produced image names unique.)
endif
ifndef RHEL_CRIO_BASEURL
	$(error RHEL_CRIO_BASEURL is undefined, expected yum repository baseurl for cri-o and cri-tools packages.)
endif
ifndef CRIO_REPO
	$(error CRIO_REPO is undefined, expected URL to CRI-O repository to use)
endif
ifndef CRIO_SLUG
	$(error CRIO_SLUG is undefined, expected GOPATH/src/ subdirectory to use for CRI-O.)
endif
ifndef FEDORA_BASE_IMAGE
	$(error FEDORA_BASE_IMAGE is undefined, expected name of existing base-image.)
endif
ifndef RHEL_BASE_IMAGE
	$(error RHEL_BASE_IMAGE is undefined, expected name of existing base-image.)
endif

.PHONY: base_image_checkenv
base_image_checkenv: check_common_env
ifndef TIMESTAMP
	$(error TIMESTAMP is undefined, expected unix epoch.)
endif
ifndef GOOGLE_APPLICATION_CREDENTIALS
	$(error GOOGLE_APPLICATION_CREDENTIALS is undefined, expected path to JSON file.  See https://cloud.google.com/docs/authentication/end-user#creating_your_client_credentials)
endif
ifndef RHEL_IMAGE_FILE
	$(error RHEL_IMAGE_FILE is undefined, expected full path to a rhel-server-ec2-*.raw.xz file)
endif
ifndef RHEL_CSUM_FILE
	$(error RHEL_CSUM_FILE is undefined, expected full path to a rhel-server-ec2-*.raw.xz.SHA256SUM file)
endif
ifndef RHSM_COMMAND
	$(error RHSM_COMMAND is undefined, expected string required for temporarily registering VM)
endif


.PHONY: conmon_images
conmon_images: cache_image_checkenv conmon_images.json packer
	@GCE_SSH_USERNAME=${GCE_SSH_USERNAME} \
	GCP_PROJECT_ID=${GCP_PROJECT_ID} \
	RHSM_COMMAND="${RHSM_COMMAND}" \
	SERVICE_ACCOUNT="${SERVICE_ACCOUNT}" \
	./packer build -only=${PACKER_BUILDS} \
		-var SRC=${SRC} \
		-var SCRIPT_BASE=${SCRIPT_BASE} \
		-var PACKER_BASE=${PACKER_BASE} \
		-var RHEL_CRIO_BASEURL=${RHEL_CRIO_BASEURL} \
		-var BUILT_IMAGE_SUFFIX=${BUILT_IMAGE_SUFFIX} \
		-var CRIO_REPO=${CRIO_REPO} \
		-var CRIO_SLUG=${CRIO_SLUG} \
		-var RHEL_BASE_IMAGE=${RHEL_BASE_IMAGE} \
		-var FEDORA_BASE_IMAGE=${FEDORA_BASE_IMAGE} \
		conmon_images.json
	@echo ""
	@echo "Finished.  The images mentioned above, and in packer-manifest.json"
	@echo "can be used in .cirrus.yml as values for the 'image_name' keys"
	@echo ""



cidata.ssh:
	ssh-keygen -f $@ -P "" -q

cidata.ssh.pub: cidata.ssh
	touch $@

meta-data:
	echo "local-hostname: localhost.localdomain" > $@

user-data: cidata.ssh.pub
	bash make-user-data.sh

cidata.iso: user-data meta-data
	genisoimage -output cidata.iso -volid cidata -input-charset utf-8 -joliet -rock user-data meta-data

# This is intended to be run by a human, with admin access to the conmon GCE project.
.PHONY: conmon_base_images
conmon_base_images: base_image_checkenv conmon_base_images.json cidata.iso cidata.ssh packer
	@PACKER_CACHE_DIR=/tmp \
	GOOGLE_APPLICATION_CREDENTIALS="${GOOGLE_APPLICATION_CREDENTIALS}" \
	RHSM_COMMAND="${RHSM_COMMAND}" \
	GCP_PROJECT_ID=${GCP_PROJECT_ID} \
		./packer build \
			-var TIMESTAMP=${TIMESTAMP} \
			-var TTYDEV=${TTYDEV} \
			-var SRC=${SRC} \
			-var PACKER_BASE=${PACKER_BASE} \
			-var SCRIPT_BASE=${SCRIPT_BASE} \
			-var RHEL_BASE_IMAGE_NAME=$(shell basename ${RHEL_IMAGE_FILE} | tr -d '[[:space:]]' | sed -r -e 's/\.x86_64\.qcow2//' | tr '[[:upper:]]' '[[:lower:]]' | tr '[[:punct:]]' '-') \
			-var RHEL_IMAGE_FILE=${RHEL_IMAGE_FILE} \
			-var RHEL_CSUM_FILE=${RHEL_CSUM_FILE} \
			-only ${PACKER_BUILDS} \
			conmon_base_images.json
	@echo ""
	@echo "Finished.  The images mentioned above, and in packer-manifest.json"
	@echo "can be used in .cirrus.yml as values for the *_BASE_IMAGE keys."
	@echo ""
