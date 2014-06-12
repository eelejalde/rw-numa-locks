#!/bin/bash

echo "Compiling..."
gcc -g -DNUMA_NODES=2 -std=c11 -o ../rw_ptkt_tkt rw_ptkt_tkt.c -pthread -lrt
gcc -g -DNUMA_NODES=2 -std=c11 -o ../rw_tkt_tkt rw_tkt_tkt.c -pthread -lrt
gcc -g -DNUMA_NODES=2 -std=c11 -o ../rw_tkt_bo rw_tkt_bo.c -pthread -lrt
gcc -g -DNUMA_NODES=2 -std=c11 -o ../rw_bo_bo rw_bo_bo.c -pthread -lrt
gcc -g -DNUMA_NODES=2 -std=c11 -o ../rw_bo_mcs rw_bo_mcs.c -pthread -lrt
gcc -g -DNUMA_NODES=2 -std=c11 -o ../rw_mcs_mcs rw_mcs_mcs.c -pthread -lrt
echo "Done!!!"
