# -*- Dockerfile -*-
FROM ubuntu:18.04
MAINTAINER Christian Hopps <chopps@labn.net>

ENV LANG=en_US.UTF-8 \
    LC_CTYPE=en_US.UTF-8 \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN sed -i -e '/# deb http.* universe/s,^# ,,' /etc/apt/sources.list && \
    sed -i -e '/# deb http.* multiverse/s,^# ,,' /etc/apt/sources.list && \
    apt-get update -qy && apt-get upgrade -y && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        bash bash-completion bird bsdtar cpio curl \
        git iperf iproute2 iputils-ping gettext jq kmod logrotate \
        libc-ares2 libev4 libxml2-utils locales lvm2 make net-tools \
        netcat-openbsd nfs-kernel-server ntp \
        openjdk-8-jre openjdk-8-jdk \
        python python-dev python-ipaddr python-pip \
        python3 python3-dev python3-pip python3-venv\
        rsync sudo ssh snmp tappy tidy udev vim xsltproc \
        virtualbox-guest-dkms virtualbox-guest-utils \
        autoconf build-essential libev-dev libxslt-dev \
        libffi-dev libssl-dev zlib1g-dev \
        libgmp-dev zlib1g-dev libedit-dev && \
        echo en_US.UTF-8 UTF-8 >> /etc/locale.gen && \
        locale-gen
RUN python3 -m pip install -U \
        coverage cryptography docker exabgp lxml netconf nose pylint pysnmp \
        pytest pyyaml ryu tox twine wheel && \
        apt-get autoremove -y && apt-get clean -y

COPY docker/run-iptfs.sh /etc/run-iptfs.sh
COPY . /iptfs
WORKDIR /iptfs
RUN python3 setup.py install
