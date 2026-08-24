# base image dockerfile is defined in https://github.com/srl-labs/vrnetlab
# (it provides qemu, the vrnetlab python env, ENTRYPOINT ["uv", "run", "/launch.py"] and the healthcheck)
FROM ghcr.io/srl-labs/vrnetlab-base:0.3.0

ARG IMAGE
COPY $IMAGE* /
COPY *.py /

EXPOSE 22 5000
