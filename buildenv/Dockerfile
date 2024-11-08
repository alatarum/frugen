ARG osver=noble
FROM ubuntu:$osver
RUN apt update
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get -y install \
      git \
      gcc \
      make \
      binutils \
      cmake \
      libjson-c-dev \
      doxygen \
      pkg-config \
      graphviz
