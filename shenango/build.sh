#!/bin/bash
set -euo pipefail

NO_NET=${NO_NET:-1}

if [[ "$NO_NET" == "1" ]]; then
  SHENANGO_ARGS=(DIRECTPATH=0 EXTRA_CFLAGS="-UDIRECTPATH -UMLX4 -UMLX5")
else
  SHENANGO_ARGS=()
fi

cd ksched
make clean
make
cd ..

# skip dpdk/rdma if NO_NET=1
if [[ "$NO_NET" != "1" ]]; then ./dpdk.sh; fi
if [[ "$NO_NET" != "1" ]]; then ./rdma-core.sh; fi

make clean
make -j"$(nproc)" "${SHENANGO_ARGS[@]}"

cd bindings/cc
make clean
make -j"$(nproc)" "${SHENANGO_ARGS[@]}"
cd ../..

if [[ "$NO_NET" != "1" ]]; then
  sudo ./scripts/setup_machine.sh
else
  echo "[info] Skipping NIC/DPDK setup (NO_NET=1)."
fi
