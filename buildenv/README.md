== Build Environment

This directory contains docker configuration for building the environment
in which frugen/libfru builds can be tested

Example:
```
OSVER=noble
docker --build-arg osver=$OSVER -t frugen-buildenv:$OSVER .
```

The above example builds an image based on Ubuntu Noble Numbat
