# conmon Installation Instructions

In most cases, conmon should be packaged with your favorite container manager.
However, if you'd like to try building it from source, follow the steps below.

## Fedora, CentOS, RHEL, and related distributions:

```bash
sudo yum install -y \
  gcc \
  git \
  glib2-devel \
  glibc-devel \
  make \
  pkgconfig \
  runc
```

## Debian, Ubuntu, and related distributions:

```bash
sudo apt-get install \
  gcc \
  git \
  libc6-dev \
  libglib2.0-dev \
  pkg-config \
  make \
  runc
```

## Clone the repository and compile:

``` bash
git clone https://github.com/containers/conmon.git
cd conmon
make
```

## Install with Ansible

An [Ansible Role](https://github.com/alvistack/ansible-role-conmon) is also available to automate the above steps:

``` bash
sudo su -
mkdir -p ~/.ansible/roles
cd ~/.ansible/roles
git clone https://github.com/alvistack/ansible-role-conmon.git conmon
cd ~/.ansible/roles/conmon
pip3 install --upgrade --ignore-installed --requirement requirements.txt
molecule converge
molecule verify
```

## Install to the directory you want:
There are three options for installation, depending on your environment. Each can have the PREFIX overridden. The PREFIX defaults to `/usr/local` for most Linux distributions.

- `make install` installs to `$PREFIX/bin`, for adding conmon to the path.
- `make podman` installs to `$PREFIX/libexec/podman`, which is used to override the conmon version that Podman uses.
- `make crio` installs to `$PREFIX/libexec/crio`, which is used to override the conmon version that CRI-O uses.


* Note, to run conmon, you'll also need to have an OCI compliant runtime installed, like [runc](https://github.com/opencontainers/runc) or [crun](https://github.com/giuseppe/crun).
