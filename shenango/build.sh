#!/bin/bash

# default: skip DPDK/RDMA + NIC setup
NO_NET=${NO_NET:-1} 
SHENANGO_MAKEFLAGS=""
if [[ "${NO_NET}" == "1" ]]; then
  SHENANGO_MAKEFLAGS="DIRECTPATH=0 EXTRA_CFLAGS='-UDIRECTPATH -UMLX4 -UMLX5'"
fi

cd ksched
make clean
make
cd ..

if [[ "${NO_NET}" != "1" ]]; then
  ./dpdk.sh || { echo 'Failed to build DPDK.'; exit 1; }
fi

if [[ "${NO_NET}" != "1" ]]; then
  ./rdma-core.sh || { echo 'Failed to build RDMA core.'; exit 1; }
fi

make clean
make -j"$(nproc)" ${SHENANGO_MAKEFLAGS}

cd bindings/cc
make clean
make -j"$(nproc)" ${SHENANGO_MAKEFLAGS}
cd ../..

if [[ "${NO_NET}" != "1" ]]; then
  sudo ./scripts/setup_machine.sh || { echo 'Failed to setup Shenango.'; exit 1; }
else
  echo "[info] Skipping NIC/DPDK setup (NO_NET=1)."
fi
