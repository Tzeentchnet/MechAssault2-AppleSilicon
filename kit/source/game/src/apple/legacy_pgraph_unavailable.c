#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t parameter)
{
    (void)subchannel;
    (void)method;
    (void)parameter;
    fputs("The legacy D3D8/D3D11 PGRAPH translator is unavailable in the "
          "macOS game graph; use the MA2 NV2A executor/backend instead.\n", stderr);
    exit(EXIT_FAILURE);
}
