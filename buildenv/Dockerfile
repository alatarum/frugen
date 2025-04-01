#
# This file is for creating the build environment docker image
# for frugen.
#
ARG osver=noble
FROM ubuntu:$osver
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update
RUN apt-get -y install \
      git \
      build-essential \
      mingw-w64 \
      pkg-config \
      cmake \
      libjson-c-dev \
      doxygen \
      graphviz
