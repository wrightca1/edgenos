# vrnetlab package for the EdgeNOS x86_64 virtual switch.
# Drop this directory into a hellt/vrnetlab checkout (as ./edgenos) next to the other
# vendors, copy the qcow2 here, and `make` -> docker image vrnetlab/edgenos_vswitch:<ver>.
# Or just run tools/containerlab/build-clab-image.sh which does all of that.
VENDOR=edgenos
NAME=vswitch
IMAGE_FORMAT=qcow
IMAGE_GLOB=EdgeNOS-*.qcow2

# EdgeNOS-0.2.1-x86_64-kvm_x86_64-r0.qcow2 -> 0.2.1
VERSION=$(shell echo $(IMAGE) | sed -E 's/^EdgeNOS-([^-]+)-.*/\1/')

-include ../makefile-sanity.include
-include ../makefile.include
