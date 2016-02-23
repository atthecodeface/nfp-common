/** Copyright (C) 2015,  Gavin J Stark.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @file          pktgencap.c
 * @brief         Test of packet generator/capture
 *
 */

/** Includes
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> 
#include <string.h> 
#include <inttypes.h>
#include "nfp_support.h"
#include "pktgen_mem.h"
#include "nfp_ipc.h"
#include "firmware/pktgen.h"
#include "firmware/pcap.h"
#include "pktgencap.h"

/** Defines
 */
#define MAX_PAGES 2
#define PCIE_HUGEPAGE_SIZE (1<<20)
#define MAX_NFP_IPC_CLIENTS 32

/** struct pktgen_nfp
 */
struct pktgen_nfp {
    struct nfp *nfp;
    struct nfp_cppid pktgen_cls_host;
    struct nfp_cppid pktgen_cls_ring;
    struct nfp_cppid pktgen_emu_buffer0;
    struct nfp_cppid pcap_cls_host;
    struct nfp_cppid pcap_cls_ring;
    struct {
        char *base;
        size_t size;
        uint64_t phys_addr[MAX_PAGES];
        struct nfp_ipc *nfp_ipc;
    } shm;
    struct {
        uint32_t ring_mask;
        uint32_t wptr;
        uint32_t rptr;
        uint32_t ack;
    } host;
    struct {
        int num_buffers;
        int ring_wptr;
        int ring_entries;
        int ring_rptr;
        int buffers_given[PCAP_HOST_CLS_RING_SIZE_ENTRIES];
    } pcap;
    struct pktgen_mem_layout *mem_layout;
};

/** Static variables
 */
static const char *shm_filename="/tmp/nfp_shm.lock";
static int shm_key = 'x';

/** mem_dump
 */
static void mem_dump(void *addr, int size)
{
    int i;
    int repetitions;
    unsigned char text[17];

    repetitions = 0;
    for (i=0; i<size; i++) {
        unsigned char ch;
        int pos;
        ch = ((unsigned char *)addr)[i];
        pos = i % 16;
        if (pos == 0) {
            if (i != 0) {
                if (repetitions == 0)
                    printf(" : %s\n", text);
                if (memcmp(((unsigned char *)addr)+i,
                           ((unsigned char *)addr)+i-16,
                           16)==0) {
                    i+=15;
                    repetitions++;
                    continue;
                }
                if (repetitions!=0)
                    printf("%04x: *\n", i-16);
                repetitions = 0;
            }
            printf("%04x:", i);
        }
        printf(" %02x", ch);
        text[pos] = ch;
        if ((ch < 0x20) || (ch > 0x7e)) {
            text[pos] = '.';
        }
        text[pos+1]=0;
    }
    if (repetitions==0) {
        for (; (i % 16)!= 0; i++)
            printf("   ");
        printf(" : %s\n", text);
    } else {
        printf("%04x: *\n", i-16);
    }
}

/** pktgen_load_nfp
 *
 * Initialize NFP, load NFP firmware and retrieve symbol table
 *
 * @param pktgen_nfp     Packet generator NFP structure
 * @param dev_num        NFP device number to load
 * @param nffw_filename  Firmware filename to load
 *
 * Return 0 on success, 1 on error
 *
 */
static int
pktgen_load_nfp(struct pktgen_nfp *pktgen_nfp,
                int dev_num,
                const char *nffw_filename)
{
    pktgen_nfp->nfp = nfp_init(dev_num);
    if (!pktgen_nfp->nfp) {
        fprintf(stderr, "Failed to open NFP\n");
        return 1;
    }

    if (nfp_fw_load(pktgen_nfp->nfp, nffw_filename) < 0) {
        fprintf(stderr, "Failed to load NFP firmware\n");
        return 1;
    }
    nfp_show_rtsyms(pktgen_nfp->nfp);
    if ((nfp_get_rtsym_cppid(pktgen_nfp->nfp,
                             "i4.pktgen_cls_host",
                             &pktgen_nfp->pktgen_cls_host) < 0) ||
        (nfp_get_rtsym_cppid(pktgen_nfp->nfp,
                             "i4.pktgen_cls_ring",
                             &pktgen_nfp->pktgen_cls_ring) < 0) ||
        (nfp_get_rtsym_cppid(pktgen_nfp->nfp,
                             "pcap_cls_host_shared_data",
                             &pktgen_nfp->pcap_cls_host) < 0) ||
        (nfp_get_rtsym_cppid(pktgen_nfp->nfp,
                             "pcap_cls_host_ring_base",
                             &pktgen_nfp->pcap_cls_ring) < 0) ||
        (nfp_get_rtsym_cppid(pktgen_nfp->nfp,
                             "pktgen_emu_buffer0",
                             &pktgen_nfp->pktgen_emu_buffer0) < 0) ||
        0) {
        fprintf(stderr, "Failed to find necessary symbols\n");
        return 1;
    }
    pktgen_nfp->host.ring_mask = (PKTGEN_CLS_RING_SIZE >> 4) - 1;// packet GEN ring is 16B per entry
    return 0;
}

/** pktgen_alloc_shm
 */
static int
pktgen_alloc_shm(struct pktgen_nfp *pktgen_nfp)
{
    pktgen_nfp->shm.size = PCIE_HUGEPAGE_SIZE * MAX_PAGES;
    if (nfp_shm_alloc(pktgen_nfp->nfp,
                      shm_filename, shm_key,
                      pktgen_nfp->shm.size, 1)==0) {
        return -1;
    }

    pktgen_nfp->shm.base = nfp_shm_data(pktgen_nfp->nfp);
    memset(pktgen_nfp->shm.base, 0, pktgen_nfp->shm.size);
    pktgen_nfp->shm.nfp_ipc = (struct nfp_ipc *)pktgen_nfp->shm.base;
    pktgen_nfp->shm.phys_addr[0] = nfp_huge_physical_address(pktgen_nfp->nfp,
                                                             pktgen_nfp->shm.base,
                                                             0);
    if (pktgen_nfp->shm.phys_addr[0] == 0) {
        fprintf(stderr, "Failed to find linux page mapping in /proc/self/pagemap\n");
        return -1;
    }
    return 0;
}

/** pktgen_issue_cmd
 * 
 * Issue a command to the packet generator firmware
 *
 * @param pktgen_nfp  Packet generator NFP structure
 * @param host_cmd    Host command to issue to packet generator firmware
 *
 * Returns 0 on success, non-zero on failure
 *
 */
static int
pktgen_issue_cmd(struct pktgen_nfp *pktgen_nfp,
                 struct pktgen_host_cmd *host_cmd)
{
    int ofs;

    ofs = pktgen_nfp->host.wptr & pktgen_nfp->host.ring_mask;
    ofs = ofs << 4;

    pktgen_nfp->host.wptr++;
    fprintf(stderr,"%x:%d:%d:%02x, %016"PRIx64", %d\n",
            (pktgen_nfp->pktgen_cls_ring.cpp_id>>24)&0xff,
            (pktgen_nfp->pktgen_cls_ring.cpp_id>>16)&0xff,
            (pktgen_nfp->pktgen_cls_ring.cpp_id>>8)&0xff,
            (pktgen_nfp->pktgen_cls_ring.cpp_id>>0)&0xff,
            pktgen_nfp->pktgen_cls_ring.addr,
            ofs );

    if (nfp_write(pktgen_nfp->nfp,
                  &pktgen_nfp->pktgen_cls_ring,
                  ofs,
                  host_cmd,
                  sizeof(*host_cmd)) != 0)
        return 1;
    if (nfp_write(pktgen_nfp->nfp,
                  &pktgen_nfp->pktgen_cls_host,
                  offsetof(struct pktgen_cls_host, wptr),
                  (void *)&pktgen_nfp->host.wptr,
                  sizeof(pktgen_nfp->host.wptr)) != 0)
        return 1;
    return 0;
}

/** pktgen_issue_ack_and_wait
 * 
 * Issue an ack to the packet generator firmware and wait for it
 *
 * @param pktgen_nfp  Packet generator NFP structure
 *
 * Returns 0 on success, non-zero on failure
 *
 */
static int
pktgen_issue_ack_and_wait(struct pktgen_nfp *pktgen_nfp)
{
    struct pktgen_host_cmd host_cmd;
    host_cmd.ack_cmd.cmd_type = PKTGEN_HOST_CMD_ACK;
    host_cmd.ack_cmd.data     = ++pktgen_nfp->host.ack;
    if (pktgen_issue_cmd(pktgen_nfp, &host_cmd) != 0)
        return 1;
    for (;;) {
        uint32_t ack_data;
        if (nfp_read(pktgen_nfp->nfp,
                     &pktgen_nfp->pktgen_cls_host,
                     offsetof(struct pktgen_cls_host, ack_data),
                     (void *)&ack_data,
                     sizeof(ack_data)) != 0)
            return 1;
        if (ack_data == pktgen_nfp->host.ack)
            return 0;
        /* Backoff ?*/
    }
    return 1;
}

/** pcap_give_pcie_buffer
 */
static int pcap_give_pcie_buffer(struct pktgen_nfp *pktgen_nfp, int buffer)
{
    int ring_offset;
    int err;
    uint64_t offset;
    uint64_t phys_addr;

    if ((buffer<0) || (buffer>pktgen_nfp->pcap.num_buffers)) {
        fprintf(stderr,"Attempt to add a buffer to the PCIe that is out of range\n");
        return 1;
    }
    offset = PCIE_HUGEPAGE_SIZE;
    offset += buffer << 18;
    ring_offset = pktgen_nfp->pcap.ring_wptr % (PCAP_HOST_CLS_RING_SIZE_ENTRIES);

    memset(pktgen_nfp->shm.base+offset,0,sizeof(struct pcap_buffer));
    phys_addr = nfp_huge_physical_address(pktgen_nfp->nfp,
                                          pktgen_nfp->shm.base,
                                          offset);
                                              
    err = nfp_write(pktgen_nfp->nfp,
                    &pktgen_nfp->pcap_cls_ring,
                    ring_offset*sizeof(phys_addr),
                    (void *)&phys_addr, sizeof(phys_addr));

    if (err)
        return err;

    pktgen_nfp->pcap.buffers_given[ring_offset] = buffer;

    pktgen_nfp->pcap.ring_wptr++;
    pktgen_nfp->pcap.ring_entries++;
    return 0;
}

/** pcap_commit_pcie_buffers
 */
static int pcap_commit_pcie_buffers(struct pktgen_nfp *pktgen_nfp)
{
    int wptr;
    wptr = pktgen_nfp->pcap.ring_wptr;

    if (nfp_write(pktgen_nfp->nfp,
                  &pktgen_nfp->pcap_cls_host,offsetof(struct pcap_cls_host,wptr),
                  (void *)&wptr,sizeof(wptr)) != 0) {
        fprintf(stderr,"Failed to write buffers etc to NFP memory\n");
        return 1;
    }
    return 0;
}

/** pcap_give_pcie_buffers
 */
static int pcap_give_pcie_buffers(struct pktgen_nfp *pktgen_nfp)
{
    int err;
    int i;

    pktgen_nfp->pcap.ring_wptr = 0;
    pktgen_nfp->pcap.ring_rptr = 0;
    pktgen_nfp->pcap.ring_entries = 0;
    pktgen_nfp->pcap.num_buffers = (pktgen_nfp->shm.size - PCIE_HUGEPAGE_SIZE)>>18;

    if (pktgen_nfp->pcap.num_buffers*sizeof(uint64_t) >= PCAP_HOST_CLS_RING_SIZE) {
        pktgen_nfp->pcap.num_buffers = PCAP_HOST_CLS_RING_SIZE/sizeof(uint64_t);
    }

    for (i=0; i<pktgen_nfp->pcap.num_buffers; i++) {
        err = pcap_give_pcie_buffer(pktgen_nfp, i);
        if (err) break;
    }
    if (err) {
        (void) pcap_commit_pcie_buffers(pktgen_nfp);
        return err;
    }

    return pcap_commit_pcie_buffers(pktgen_nfp);
}

/** pcap_dump_pcie_buffers
 */
static void pcap_dump_pcie_buffers(struct pktgen_nfp *pktgen_nfp)
{
    int i;
    uint64_t phys_offset;

    phys_offset = PCIE_HUGEPAGE_SIZE;
    for (i=0; i<pktgen_nfp->pcap.num_buffers; i++) {
        if (1) {
            uint64_t phys_addr;
            phys_addr = nfp_huge_physical_address(pktgen_nfp->nfp,
                                                  pktgen_nfp->shm.base,
                                                  phys_offset);
            printf("Phys %"PRIx64"\n",phys_addr);
        }
        if (1) {
            mem_dump( pktgen_nfp->shm.base + phys_offset, 20000 );
        }
        if (1) {
            int j;
            struct pcap_buffer *pcap_buffer;
            pcap_buffer = (struct pcap_buffer *)(pktgen_nfp->shm.base + phys_offset);
            for (j=0; j<PCAP_BUF_MAX_PKT; j++) {
                if (pcap_buffer->pkt_desc[j].offset==0)
                    break;
                printf("%d: %04x %04x %08x\n",j,
                       pcap_buffer->pkt_desc[j].offset,
                       pcap_buffer->pkt_desc[j].num_blocks,
                       pcap_buffer->pkt_desc[j].seq
                    );
                mem_dump(((char *)pcap_buffer) + (pcap_buffer->pkt_desc[j].offset<<6), 64);
            }
        }
        phys_offset += 1 << 18;
    }
}

/** pcap_show_pcie_buffer_headers
 */
static void pcap_show_pcie_buffer_headers(struct pktgen_nfp *pktgen_nfp)
{
    int i;
    uint64_t phys_offset;

    phys_offset = PCIE_HUGEPAGE_SIZE;
    printf("Showing PCIe buffers (total %d)\n",pktgen_nfp->pcap.num_buffers);
    for (i=0; i<pktgen_nfp->pcap.num_buffers; i++) {
        if (1) {
            uint64_t phys_addr;
            phys_addr = nfp_huge_physical_address(pktgen_nfp->nfp,
                                                  pktgen_nfp->shm.base,
                                                  phys_offset);
            printf("Phys %"PRIx64"\n",phys_addr);
        }
        if (1) {
            mem_dump( pktgen_nfp->shm.base + phys_offset, 1024 );
        }
        phys_offset += 1 << 18;
    }
}

/** mem_alloc_callback
 * 
 * Allocate memory for a packet generator memory layout structure
 *
 * @param handle          Packet generator NFP structure
 * @param size            Size to allocate
 * @param min_break_size  Minimum size to break allocations into
 * @param memory_mask     Mask of memories to allocate the structure in
 * @param data            Array to store resultant allocations
 *
 * Returns 0 on success, non-zero on failure
 *
 * The size to allocate can be spread across all the memories whose
 * mask bit is 1.
 *
 * The size should not be broken into pieces smaller than
 * 'min_break_size'.
 *
 * An allocation MAY exceed that requested if the minimum allocation
 * for a memory requires it.
 *
 */
static uint64_t emem0_base=0; // Was hard-coded to (4L << 35) | 0x900000;
static int 
mem_alloc_callback(void *handle,
                   uint64_t size,
                   uint64_t min_break_size,
                   int memory_mask,
                   struct pktgen_mem_data *data)
{

    if (memory_mask==0) return 0;
    if ((memory_mask & 1) == 0 ) return 0;

    size = ((size+4095)/4096)*4096;
    data[0].size = size;
    data[0].mu_base_s8 = emem0_base >> 8;
    emem0_base += size;
    printf("Allocated memory size %"PRId64" base %08"PRIx32"00\n",
           size,
           data[0].mu_base_s8);
    return 0;
}

/** mem_load_callback
 * 
 * Load a memory allocation from host memory to an NFP memory
 *
 * @param handle  Packet generator NFP structure
 * @param layout  Packet generatore memory layout 
 * @param data    Descriptor of which host memory, NFP memory and size to load
 *
 * Returns 0 on success, non-zero on failure
 *
 */
static int 
mem_load_callback(void *handle,
                  struct pktgen_mem_layout *layout,
                  struct pktgen_mem_data *data)
{
    struct pktgen_nfp *pktgen_nfp;
    uint32_t mu_base_s8;
    uint64_t size;
    const char *mem;
    int err;

    printf("Load data from %p to %010"PRIx64" size %"PRId64"\n",
           data->base,
           ((uint64_t)data->mu_base_s8)<<8,
           data->size);

    pktgen_nfp = (struct pktgen_nfp *)handle;
    mu_base_s8 = data->mu_base_s8;
    size       = data->size;
    mem        = data->base;

    while (size>0) {
        uint64_t size_to_do;
        uint64_t data_phys;
        char     *data;
        struct pktgen_host_cmd host_cmd;

        data      = pktgen_nfp->shm.base + 512*1024;
        data_phys = pktgen_nfp->shm.phys_addr[0] + 512*1024;

        size_to_do = size;
        if (size_to_do > 512*1024)
            size_to_do = 512*1024;

        host_cmd.dma_cmd.cmd_type = PKTGEN_HOST_CMD_DMA;
        host_cmd.dma_cmd.length = size_to_do;
        host_cmd.dma_cmd.mu_base_s8     = mu_base_s8;
        host_cmd.dma_cmd.pcie_base_low  = data_phys;
        host_cmd.dma_cmd.pcie_base_high = data_phys >> 32;

        memcpy(data, mem, size_to_do);
        if (0) {
            int i;
            for (i=0; i<size_to_do; i+=4) {
                const uint32_t *m;
                m = (const uint32_t *)mem+i;
                printf("%d: %08x %08x %08x %08x\n",
                       i,m[0],m[1],m[2],m[3]);
            }
        }
        err = pktgen_issue_cmd(pktgen_nfp, &host_cmd);
        if (err == 0)
            err = pktgen_issue_ack_and_wait(pktgen_nfp);
        if (err != 0)
            return err;

        mu_base_s8 += size_to_do >> 8;
        mem += size_to_do; 
        size -= size_to_do;
    }
    return 0;
}

/** Main
    For this we load the firmware and give it packets.
 */
extern int
main(int argc, char **argv)
{
    struct pktgen_nfp pktgen_nfp;
    int pktgen_loaded;

    if (pktgen_load_nfp(&pktgen_nfp, 0, "firmware/nffw/pktgencap.nffw")!=0) {
        fprintf(stderr,"Failed to open and load up NFP with ME code\n");
        return 4;
    }

    pktgen_nfp.mem_layout = pktgen_mem_alloc(&pktgen_nfp,
                                             mem_alloc_callback,
                                             mem_load_callback,
                                             NULL );
    if (pktgen_alloc_shm(&pktgen_nfp)!=0) {
        fprintf(stderr,"Failed to allocate memory\n");
        return 4;
    }

    if (pcap_give_pcie_buffers(&pktgen_nfp) != 0) {
        fprintf(stderr,"Failed to give PCIe pcap buffers\n");
        return 4;
    }

    if (nfp_fw_start(pktgen_nfp.nfp)<0) {
        fprintf(stderr,"Failed to start NFP firmware\n");
        return 4;
    }

    pktgen_loaded = 0;

    struct nfp_ipc_server_desc nfp_ipc_server_desc;
    nfp_ipc_server_desc.max_clients = MAX_NFP_IPC_CLIENTS;
    nfp_ipc_init(pktgen_nfp.shm.nfp_ipc, &nfp_ipc_server_desc);

    for (;;) {
        int poll;
        struct nfp_ipc_event event;

        poll = nfp_ipc_server_poll(pktgen_nfp.shm.nfp_ipc, 0, &event);
        if (poll==NFP_IPC_EVENT_SHUTDOWN)
            break;

        if (poll==NFP_IPC_EVENT_MESSAGE) {
            struct pktgen_ipc_msg *msg;
            msg = (struct pktgen_ipc_msg *)&event.msg->data[0];
            if (msg->reason == PKTGEN_IPC_SHUTDOWN) {
                msg->ack = 1;
                nfp_ipc_server_send_msg(pktgen_nfp.shm.nfp_ipc, event.client, event.msg);
                break;
            } else if (msg->reason == PKTGEN_IPC_LOAD) {
                pktgen_loaded = 0;
                emem0_base  = ((pktgen_nfp.pktgen_emu_buffer0.cpp_id&0xff)-20L) << 35;
                emem0_base |= pktgen_nfp.pktgen_emu_buffer0.addr;
                if (pktgen_mem_open_directory(pktgen_nfp.mem_layout,
                                              "../pktgen_data/") != 0) {
                    fprintf(stderr,"ERROR: Failed to load packet generation data\n");
                    msg->ack = -2;
                } else if (pktgen_mem_load(pktgen_nfp.mem_layout) != 0) {
                    fprintf(stderr,"ERROR: Failed to load generator memory\n");
                    msg->ack = -3;
                } else {
                    pktgen_loaded = 1;
                }
            } else if (msg->reason == PKTGEN_IPC_HOST_CMD) {
                if (!pktgen_loaded) {
                    fprintf(stderr,"ERROR: Attempt to generate packets when not loaded\n");
                    msg->ack = -2;
                } else {
                    msg->ack = 1;
                    struct pktgen_host_cmd host_cmd;
                    host_cmd.pkt_cmd.cmd_type = PKTGEN_HOST_CMD_PKT;
                    host_cmd.pkt_cmd.base_delay = msg->generate.base_delay;
                    host_cmd.pkt_cmd.total_pkts = msg->generate.total_pkts;
                    host_cmd.pkt_cmd.mu_base_s8 = pktgen_mem_get_mu(pktgen_nfp.mem_layout,0,0)>>8;
                    (void) pktgen_issue_cmd(&pktgen_nfp, &host_cmd);
                }
            } else if (msg->reason == PKTGEN_IPC_DUMP_BUFFERS) {
                pcap_dump_pcie_buffers(&pktgen_nfp);
                msg->ack = 1;
            } else if (msg->reason == PKTGEN_IPC_SHOW_BUFFER_HEADERS) {
                pcap_show_pcie_buffer_headers(&pktgen_nfp);
                msg->ack = 1;
            } else if (msg->reason == PKTGEN_IPC_RETURN_BUFFERS) {
                if (msg->return_buffers.buffers[0]>=0) {
                    pcap_give_pcie_buffer(&pktgen_nfp,msg->return_buffers.buffers[0]);
                    if (msg->return_buffers.buffers[1]>=0) {
                        pcap_give_pcie_buffer(&pktgen_nfp,msg->return_buffers.buffers[1]);
                    }
                    pcap_commit_pcie_buffers(&pktgen_nfp);
                }
                msg->return_buffers.buffers[0] = -1;
                msg->return_buffers.buffers[1] = -1;
                if (msg->return_buffers.buffers_to_claim>0) {
                    int ring_offset;
                    if (pktgen_nfp.pcap.ring_entries>0) {
                        ring_offset = pktgen_nfp.pcap.ring_rptr;
                        msg->return_buffers.buffers[0] = pktgen_nfp.pcap.buffers_given[ring_offset];
                        ring_offset = (ring_offset+1) % (PCAP_HOST_CLS_RING_SIZE/sizeof(uint64_t));
                        pktgen_nfp.pcap.ring_rptr = ring_offset;
                        pktgen_nfp.pcap.ring_entries--;
                    }
                }
                msg->ack = 1;
            } else {
                msg->ack = -1;
            }
            nfp_ipc_server_send_msg(pktgen_nfp.shm.nfp_ipc, event.client, event.msg);
        }
    }

    nfp_ipc_shutdown(pktgen_nfp.shm.nfp_ipc, 5*1000*1000);

    nfp_shutdown(pktgen_nfp.nfp);
    return 0;
}
