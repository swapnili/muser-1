/*
 * Copyright (c) 2020 Nutanix Inc. All rights reserved.
 *
 * Authors: Thanos Makatos <thanos@nutanix.com>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *      * Neither the name of Nutanix nor the names of its contributors may be
 *        used to endorse or promote products derived from this software without
 *        specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *  DAMAGE.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <time.h>
#include <err.h>
#include <assert.h>
#include <sys/stat.h>

#include "../lib/muser.h"
#include "../lib/muser_priv.h"
#include "../lib/common.h"

static int
init_sock(const char *path)
{
    int ret, sock;
	struct sockaddr_un addr = {.sun_family = AF_UNIX};

	/* TODO path should be defined elsewhere */
	ret = snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		err(EXIT_FAILURE, "failed to open socket %s", path);
	}

	if ((ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr))) == -1) {
		err(EXIT_FAILURE, "failed to connect server");
	}
	return sock;
}

static void
set_version(int sock, int client_max_fds, int *server_max_fds, size_t *pgsize)
{
    int ret, mj, mn;
    uint16_t msg_id;
    char *client_caps = NULL;

    assert(server_max_fds != NULL);
    assert(pgsize != NULL);

    ret = recv_version(sock, &mj, &mn, &msg_id, false, server_max_fds, pgsize);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to receive version from server: %s",
             strerror(-ret));
    }

    if (mj != LIB_MUSER_VFIO_USER_VERS_MJ || mn != LIB_MUSER_VFIO_USER_VERS_MN) {
        errx(EXIT_FAILURE, "bad server version %d.%d\n", mj, mn);
    }

    if (asprintf(&client_caps, "{max_fds: %d, migration: {pgsize: %lu}}",
                 client_max_fds, sysconf(_SC_PAGESIZE)) == -1) {
        err(EXIT_FAILURE, NULL);
    }

    ret = send_version(sock, mj, mn, msg_id, true, client_caps);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to send version to server: %s",
             strerror(-ret));
    }
    free(client_caps);
}

static void
send_device_reset(int sock)
{
    int ret = send_recv_vfio_user_msg(sock, 1, VFIO_USER_DEVICE_RESET,
                                      NULL, 0, NULL, 0, NULL, NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to reset device: %s\n", strerror(-ret));
    }
}

static void
get_region_vfio_caps(int sock, size_t cap_sz)
{
    struct vfio_info_cap_header *header, *_header;
    struct vfio_region_info_cap_type *type;
    struct vfio_region_info_cap_sparse_mmap *sparse;
    unsigned int i;
    ssize_t ret;

    header = _header = calloc(cap_sz, 1);
    if (header == NULL) {
        err(EXIT_FAILURE, NULL);
    }

    ret = recv(sock, header, cap_sz, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to receive VFIO cap info");
    }
    assert((size_t)ret == cap_sz);

    while (true) {
        switch (header->id) {
            case VFIO_REGION_INFO_CAP_SPARSE_MMAP:
                sparse = (struct vfio_region_info_cap_sparse_mmap*)header;
                printf("%s: Sparse cap nr_mmap_areas %d\n", __func__,
                       sparse->nr_areas);
                for (i = 0; i < sparse->nr_areas; i++) {
                    printf("%s: area %d offset %#llx size %llu\n", __func__,
                           i, sparse->areas[i].offset, sparse->areas[i].size);
                }
                break;
            case VFIO_REGION_INFO_CAP_TYPE:
                type = (struct vfio_region_info_cap_type*)header;
                if (type->type != VFIO_REGION_TYPE_MIGRATION ||
                    type->subtype != VFIO_REGION_SUBTYPE_MIGRATION) {
                    errx(EXIT_FAILURE, "bad region type %d/%d", type->type,
                         type->subtype);
                }
                printf("migration region\n");
                break;
            default:
                errx(EXIT_FAILURE, "bad VFIO cap ID %#x", header->id);
        }
        if (header->next == 0) {
            break;
        }
        header = (struct vfio_info_cap_header*)((char*)header + header->next - sizeof(struct vfio_region_info));
    }
    free(_header);
}

static void
get_device_region_info(int sock, struct vfio_device_info *client_dev_info)
{
    struct vfio_region_info region_info;
    uint16_t msg_id = 0;
    size_t cap_sz;
    int ret;
    unsigned int i;

    msg_id = 1;
    for (i = 0; i < client_dev_info->num_regions; i++) {
        memset(&region_info, 0, sizeof(region_info));
        region_info.argsz = sizeof(region_info);
        region_info.index = i;
        msg_id++;
        ret = send_recv_vfio_user_msg(sock, msg_id,
                                      VFIO_USER_DEVICE_GET_REGION_INFO,
                                      &region_info, sizeof region_info,
                                      NULL, 0, NULL,
                                      &region_info, sizeof(region_info));
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to get device region info: %s",
                    strerror(-ret));
        }

	    cap_sz = region_info.argsz - sizeof(struct vfio_region_info);
        printf("%s: region_info[%d] offset %#llx flags %#x size %llu "
               "cap_sz %lu\n", __func__, i, region_info.offset,
               region_info.flags, region_info.size, cap_sz);
	    if (cap_sz) {
            get_region_vfio_caps(sock, cap_sz);
	    }
    }
}

static void
get_device_info(int sock, struct vfio_device_info *dev_info)
{
    uint16_t msg_id;
    int ret;

    dev_info->argsz = sizeof(*dev_info);
    msg_id = 1;
    ret = send_recv_vfio_user_msg(sock, msg_id, VFIO_USER_DEVICE_GET_INFO,
                                  dev_info, sizeof(*dev_info), NULL, 0, NULL,
                                  dev_info, sizeof(*dev_info));
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to get device info: %s", strerror(-ret));
    }

    if (dev_info->num_regions != 10) {
        errx(EXIT_FAILURE, "bad number of device regions %d",
                dev_info->num_regions);
    }

    printf("devinfo: flags %#x, num_regions %d, num_irqs %d\n",
           dev_info->flags, dev_info->num_regions, dev_info->num_irqs);
}

static int
configure_irqs(int sock)
{
    int i, ret;
    struct vfio_irq_set irq_set;
    uint16_t msg_id = 1;
    int irq_fd;

    for (i = 0; i < LM_DEV_NUM_IRQS; i++) { /* TODO move body of loop into function */
        struct vfio_irq_info vfio_irq_info = {
            .argsz = sizeof vfio_irq_info,
            .index = i
        };
        ret = send_recv_vfio_user_msg(sock, msg_id,
                                      VFIO_USER_DEVICE_GET_IRQ_INFO,
                                      &vfio_irq_info, sizeof vfio_irq_info,
                                      NULL, 0, NULL,
                                      &vfio_irq_info, sizeof vfio_irq_info);
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to get  %s info: %s", irq_to_str[i],
                 strerror(-ret));
        }
        if (vfio_irq_info.count > 0) {
            printf("IRQ %s: count=%d flags=%#x\n",
                   irq_to_str[i], vfio_irq_info.count, vfio_irq_info.flags);
        }
    }

    msg_id++;

    irq_set.argsz = sizeof irq_set;
    irq_set.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set.index = 0;
    irq_set.start = 0;
    irq_set.count = 1;
    irq_fd = eventfd(0, 0);
    if (irq_fd == -1) {
        err(EXIT_FAILURE, "failed to create eventfd");
    }
    ret = send_recv_vfio_user_msg(sock, msg_id, VFIO_USER_DEVICE_SET_IRQS,
                                  &irq_set, sizeof irq_set, &irq_fd, 1, NULL,
                                  NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to send configure IRQs message: %s",
             strerror(-ret));
    }

    return irq_fd;
}

static int
access_region(int sock, int region, bool is_write, uint64_t offset,
            void *data, size_t data_len)
{
    struct vfio_user_region_access send_region_access = {
        .offset = offset,
        .region = region,
        .count = data_len
    };
    struct iovec send_iovecs[3] = {
        [1] = {
            .iov_base = &send_region_access,
            .iov_len = sizeof send_region_access
        },
        [2] = {
            .iov_base = data,
            .iov_len = data_len
        }
    };
    struct {
        struct vfio_user_region_access region_access;
        char data[data_len];
    } __attribute__((packed)) recv_data;
    int op, ret;
    size_t nr_send_iovecs, recv_data_len;

    if (is_write) {
        op = VFIO_USER_REGION_WRITE;
        nr_send_iovecs = 3;
        recv_data_len = sizeof(recv_data.region_access);
    } else {
        op = VFIO_USER_REGION_READ;
        nr_send_iovecs = 2;
        recv_data_len = sizeof(recv_data);
    }

    ret = _send_recv_vfio_user_msg(sock, 0, op,
                                   send_iovecs, nr_send_iovecs,
                                   NULL, 0, NULL,
                                   &recv_data, recv_data_len);
    if (ret != 0) {
        warnx("failed to %s region %d %#lx-%#lx: %s",
             is_write ? "write to" : "read from", region, offset,
             offset + data_len - 1, strerror(-ret));
        return ret;
    }
    if (recv_data.region_access.count != data_len) {
        warnx("bad %s data count, expected=%lu, actual=%d",
             is_write ? "write" : "read", data_len,
             recv_data.region_access.count);
        return -EINVAL;
    }

    /*
     * TODO we could avoid the memcpy if _sed_recv_vfio_user_msg received the
     * response into an iovec, but it's some work to implement it.
     */
    if (!is_write) {
        memcpy(data, recv_data.data, data_len);
    }
    return 0;
}

static void
wait_for_irqs(int sock, int irq_fd)
{
    int ret;
    uint64_t val;
    size_t size;
    struct vfio_user_irq_info vfio_user_irq_info;
    struct vfio_user_header hdr;
    uint16_t msg_id = 1;

    if (read(irq_fd, &val, sizeof val) == -1) {
        err(EXIT_FAILURE, "failed to read from irqfd");
    }
    printf("INTx triggered!\n");

    size = sizeof(vfio_user_irq_info);
    ret = recv_vfio_user_msg(sock, &hdr, false, &msg_id, &vfio_user_irq_info,
                             &size);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to receive IRQ message: %s",
             strerror(-ret));
    }
    if (vfio_user_irq_info.subindex >= 1) {
        errx(EXIT_FAILURE, "bad IRQ %d, max=1\n", vfio_user_irq_info.subindex);
    }

    ret = send_vfio_user_msg(sock, msg_id, true, VFIO_USER_VM_INTERRUPT,
                             NULL, 0, NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE,
             "failed to send reply for VFIO_USER_VM_INTERRUPT: %s",
             strerror(-ret));
    }
    printf("INTx messaged triggered!\n");
}

static void
access_bar0(int sock, int irq_fd, time_t *t)
{
    int ret;

    assert(t != NULL);

    ret = access_region(sock, LM_DEV_BAR0_REG_IDX, true, 0, t, sizeof *t);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to write to BAR0: %s", strerror(-ret));
    }

    printf("wrote to BAR0: %ld\n", *t);

    ret = access_region(sock, LM_DEV_BAR0_REG_IDX, false, 0, t, sizeof *t);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to read from BAR0: %s", strerror(-ret));
    }

    printf("read from BAR0: %ld\n", *t);

    wait_for_irqs(sock, irq_fd);
}

static void
handle_dma_write(int sock, struct vfio_user_dma_region *dma_regions,
                 int nr_dma_regions, int *dma_region_fds)
{
    struct vfio_user_dma_region_access dma_access;
    struct vfio_user_header hdr;
    int ret, i;
    size_t size = sizeof(dma_access);
    uint16_t msg_id;
    void *data;

    msg_id = 1;
    ret = recv_vfio_user_msg(sock, &hdr, false, &msg_id, &dma_access, &size);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to receive DMA read: %s", strerror(-ret));
    }

    data = calloc(dma_access.count, 1);
    if (data == NULL) {
        err(EXIT_FAILURE, NULL);
    }

    if (recv(sock, data, dma_access.count, 0) == -1) {
        err(EXIT_FAILURE, "failed to recieve DMA read data");
    }

    for (i = 0; i < nr_dma_regions; i++) {
        if (dma_regions[i].addr == dma_access.addr) {
            ret = pwrite(dma_region_fds[i], data, dma_access.count,
                         dma_regions[i].offset);
            if (ret < 0) {
                err(EXIT_FAILURE,
                    "failed to write data to fd=%d at %#lx-%#lx",
                        dma_region_fds[i],
                        dma_regions[i].offset,
                        dma_regions[i].offset + dma_access.count - 1);
            }
            break;
	    }
    }

    dma_access.count = 0;
    ret = send_vfio_user_msg(sock, msg_id, true, VFIO_USER_DMA_WRITE,
                             &dma_access, sizeof dma_access, NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to send reply of DMA write: %s",
             strerror(-ret));
    }
    free(data);
}

static void
handle_dma_read(int sock, struct vfio_user_dma_region *dma_regions,
                int nr_dma_regions, int *dma_region_fds)
{
    struct vfio_user_dma_region_access dma_access, *response;
    struct vfio_user_header hdr;
    int ret, i, response_sz;
    size_t size = sizeof(dma_access);
    uint16_t msg_id;
    void *data;

    msg_id = 1;
    ret = recv_vfio_user_msg(sock, &hdr, false, &msg_id, &dma_access, &size);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to recieve DMA read");
    }

    response_sz = sizeof(dma_access) + dma_access.count;
    response = calloc(response_sz, 1);
    if (response == NULL) {
        err(EXIT_FAILURE, NULL);
    }
    response->count = dma_access.count;
    data = (char *)response->data;

    for (i = 0; i < nr_dma_regions; i++) {
        if (dma_regions[i].addr == dma_access.addr) {
            if (pread(dma_region_fds[i], data, dma_access.count, dma_regions[i].offset) == -1) {
                err(EXIT_FAILURE, "failed to write data at %#lx-%#lx",
                    dma_regions[i].offset,
                    dma_regions[i].offset + dma_access.count);
            }
            break;
	    }
    }

    ret = send_vfio_user_msg(sock, msg_id, true, VFIO_USER_DMA_READ,
                             response, response_sz, NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to send reply of DMA write: %s",
             strerror(-ret));
    }
    free(response);
}

static void
handle_dma_io(int sock, struct vfio_user_dma_region *dma_regions,
              int nr_dma_regions, int *dma_region_fds)
{
    handle_dma_write(sock, dma_regions, nr_dma_regions, dma_region_fds);
    handle_dma_read(sock, dma_regions, nr_dma_regions, dma_region_fds);
}

static void
get_dirty_bitmaps(int sock, struct vfio_user_dma_region *dma_regions,
                  UNUSED int nr_dma_regions)
{
    struct vfio_iommu_type1_dirty_bitmap dirty_bitmap = {0};
    struct vfio_iommu_type1_dirty_bitmap_get bitmaps[2];
    int ret;
    size_t i;
    struct iovec iovecs[4] = {
        [1] = {
            .iov_base = &dirty_bitmap,
            .iov_len = sizeof dirty_bitmap
        }
    };
    struct vfio_user_header hdr = {0};
    char data[ARRAY_SIZE(bitmaps)];

    assert(dma_regions != NULL);
    //FIXME: Is below assert correct?
    //assert(nr_dma_regions >= (int)ARRAY_SIZE(bitmaps));

    for (i = 0; i < ARRAY_SIZE(bitmaps); i++) {
        bitmaps[i].iova = dma_regions[i].addr;
        bitmaps[i].size = dma_regions[i].size;
        bitmaps[i].bitmap.size = 1; /* FIXME calculate based on page and IOVA size, don't hardcode */
        bitmaps[i].bitmap.pgsize = sysconf(_SC_PAGESIZE);
        iovecs[(i + 2)].iov_base = &bitmaps[i]; /* FIXME the +2 is because iovecs[0] is the vfio_user_header and iovecs[1] is vfio_iommu_type1_dirty_bitmap */
        iovecs[(i + 2)].iov_len = sizeof(struct vfio_iommu_type1_dirty_bitmap_get);
    }

    /*
     * FIXME there should be at least two IOVAs. Send single message for two
     * IOVAs and ensure only one bit is set in first IOVA.
     */
    dirty_bitmap.argsz = sizeof(dirty_bitmap) + ARRAY_SIZE(bitmaps) * sizeof(struct vfio_iommu_type1_dirty_bitmap_get);
    dirty_bitmap.flags = VFIO_IOMMU_DIRTY_PAGES_FLAG_GET_BITMAP;
    ret = _send_recv_vfio_user_msg(sock, 0, VFIO_USER_DIRTY_PAGES,
                                  iovecs, ARRAY_SIZE(iovecs),
                                  NULL, 0,
                                  &hdr, data, ARRAY_SIZE(data));
    if (ret != 0) {
        errx(EXIT_FAILURE, "failed to start dirty page logging: %s",
             strerror(-ret));
    }

    for (i = 0; i < ARRAY_SIZE(bitmaps); i++) {
        printf("%#llx-%#llx\t%hhu\n", bitmaps[i].iova,
               bitmaps[i].iova + bitmaps[i].size - 1, data[i]);
    }
}

enum migration {
    NO_MIGRATION,
    MIGRATION_SOURCE,
    MIGRATION_DESTINATION,
};

static void
usage(char *path) {
    fprintf(stderr, "Usage: %s [-h] [-m src|dst] /path/to/socket\n",
            basename(path));
}

static void
migrate_from(int sock, void **data, __u64 *len)
{
    __u32 device_state = VFIO_DEVICE_STATE_SAVING;
    __u64 pending_bytes, data_offset, data_size;

    /* XXX set device state to stop-and-copy */
    int ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, true,
                            offsetof(struct vfio_device_migration_info, device_state),
                            &device_state, sizeof(device_state));
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to write to device state: %s",
             strerror(-ret));
    }

    /* XXX read pending_bytes */
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, false,
                        offsetof(struct vfio_device_migration_info, pending_bytes),
                        &pending_bytes, sizeof pending_bytes);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to read pending_bytes: %s",
             strerror(-ret));
    }

    /* We do expect some migration data. */
    assert(pending_bytes > 0);

    /*
     * The only expectation about pending_bytes is whether it's zero or
     * non-zero, therefore it must be considered volatile, even acrosss
     * iterantions. In the sample server we know it's static so it's fairly
     * straightforward.
     */
    *len = pending_bytes;
    *data = malloc(*len);
    if (*data == NULL) {
        err(EXIT_FAILURE, "failed to allocate migration buffer");
    }

    while (pending_bytes > 0) {

        /* XXX read data_offset and data_size */
        ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, false,
                            offsetof(struct vfio_device_migration_info, data_offset),
                            &data_offset, sizeof data_offset);
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to read data_offset: %s",
                 strerror(-ret));
        }

        ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, false,
                            offsetof(struct vfio_device_migration_info, data_size),
                            &data_size, sizeof data_size);
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to read data_size: %s",
                 strerror(-ret));
        }

        assert(data_offset - sizeof(struct vfio_device_migration_info) + data_size <= *len);

        /* XXX read migration data */
        ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, false, data_offset,
                            (char*)*data + data_offset - sizeof(struct vfio_device_migration_info),
                            data_size);
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to read migration data: %s",
                 strerror(-ret));
        }

        /* FIXME send migration data to the destination client process */

        /*
         * XXX read pending_bytes again to indicate to the sever that the
         * migration data have been consumed.
         */
        ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, false,
                            offsetof(struct vfio_device_migration_info, pending_bytes),
                            &pending_bytes, sizeof pending_bytes);
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to read pending_bytes: %s",
                 strerror(-ret));
        }
    }

    /* XXX read device state, migration must have finished now */
    device_state = VFIO_DEVICE_STATE_STOP;
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, true,
                        offsetof(struct vfio_device_migration_info, device_state),
                        &device_state, sizeof(device_state));
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to write to device state: %s",
             strerror(-ret));
    }
}

static int
migrate_to(char *old_sock_path, int client_max_fds, int *server_max_fds,
           size_t *pgsize, void *migr_data, __u64 migr_data_len)
{
    int ret, sock;
    char *sock_path;
    struct stat sb;
    __u32 device_state = VFIO_DEVICE_STATE_RESUMING;
    __u64 data_offset;

    assert(old_sock_path != NULL);

    ret = asprintf(&sock_path, "%s_migrated", old_sock_path);
    if (ret == -1) {
        err(EXIT_FAILURE, "failed to asprintf");
    }

    ret = fork();
    if (ret == -1) {
        err(EXIT_FAILURE, "failed to fork");
    }
    if (ret > 0) { /* child (destination server) */
        char *_argv[] = {
            "build/dbg/samples/server",
            "-v",
            sock_path,
            NULL
        };    
        ret = execvp(_argv[0] , _argv);
        if (ret != 0) {
            err(EXIT_FAILURE, "failed to start destination sever");
        }
    }

    /* parent (client) */

    /* wait for the server to come up */
    while (stat(sock_path, &sb) == -1) {
        if (errno != ENOENT) {
            err(EXIT_FAILURE, "failed to stat %s", sock_path);
        }
    }
   if ((sb.st_mode & S_IFMT) != S_IFSOCK) {
       errx(EXIT_FAILURE, "%s: not a socket", sock_path);
   }

    /* connect to the destination server */
    sock = init_sock(sock_path);

    set_version(sock, client_max_fds, server_max_fds, pgsize);

    /* XXX set device state to resuming */
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, true,
                            offsetof(struct vfio_device_migration_info, device_state),
                            &device_state, sizeof(device_state));
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to set device state to resuming: %s",
             strerror(-ret));
    }

    /* XXX read data offset */
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, false,
                            offsetof(struct vfio_device_migration_info, data_offset),
                            &data_offset, sizeof(data_offset));
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to read data offset: %s", strerror(-ret));
    }

    /* XXX write migration data */

    /*
     * TODO write half of migration data via regular write and other half via
     * memopy map.
     */
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, true,
                        data_offset, migr_data, migr_data_len);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to write migration data: %s",
             strerror(-ret));
    }

    /* XXX write data_size */
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, true,
                        offsetof(struct vfio_device_migration_info, data_size),
                        &migr_data_len, sizeof migr_data_len);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to write data size: %s", strerror(-ret));
    }

    /* XXX set device state to running */
    device_state = VFIO_DEVICE_STATE_RUNNING;
    ret = access_region(sock, LM_DEV_MIGRATION_REG_IDX, true,
                            offsetof(struct vfio_device_migration_info, device_state),
                            &device_state, sizeof(device_state));
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to set device state to running: %s",
             strerror(-ret));
    }

    return sock;
}

static void
map_dma_regions(int sock, int max_fds, struct vfio_user_dma_region *dma_regions,
                int *dma_region_fds, int nr_dma_regions) 
{
    int i, ret;

    for (i = 0; i < nr_dma_regions / max_fds; i++) {
        ret = send_recv_vfio_user_msg(sock, i, VFIO_USER_DMA_MAP,
                                      dma_regions + (i * max_fds),
                                      sizeof(*dma_regions) * max_fds,
                                      dma_region_fds + (i * max_fds),
                                      max_fds, NULL, NULL, 0);
        if (ret < 0) {
            errx(EXIT_FAILURE, "failed to map DMA regions: %s", strerror(-ret));
        }
    }
}

int main(int argc, char *argv[])
{
	int ret, sock, irq_fd;
    struct vfio_user_dma_region *dma_regions;
    struct vfio_device_info client_dev_info = {0};
    int *dma_region_fds;
    uint16_t msg_id = 1;
    int i;
    FILE *fp;
    const int client_max_fds = 32;
    int server_max_fds;
    size_t pgsize;
    int nr_dma_regions;
    struct vfio_iommu_type1_dirty_bitmap dirty_bitmap = {0};
    int opt;
    time_t t;
    void *migr_data;
    __u64 migr_data_len;

    while ((opt = getopt(argc, argv, "h")) != -1) {
        switch (opt) {
            case 'h':
                usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (argc != optind + 1) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    sock = init_sock(argv[optind]);

    /*
     * XXX VFIO_USER_VERSION
     *
     * The server proposes version upon connection, we need to send back the
     * version the version we support.
     */
    set_version(sock, client_max_fds, &server_max_fds, &pgsize);

    /* try to access a bogus region, we should het an error */
    ret = access_region(sock, 0xdeadbeef, false, 0, &ret, sizeof ret);
    if (ret != -EINVAL) {
        errx(EXIT_FAILURE,
             "expected -EINVAL accessing bogus region, got %d instead",
             ret);
    }

    /* XXX VFIO_USER_DEVICE_GET_INFO */
    get_device_info(sock, &client_dev_info);
   
    /* XXX VFIO_USER_DEVICE_GET_REGION_INFO */
    get_device_region_info(sock, &client_dev_info);
   
    /* XXX VFIO_USER_DEVICE_RESET */
    send_device_reset(sock);

    /*
     * XXX VFIO_USER_DMA_MAP
     *
     * Tell the server we have some DMA regions it can access. Each DMA region
     * is accompanied by a file descriptor, so let's create more (2x) DMA
     * regions that can fit in a message that can be handled by the server.
     */
    nr_dma_regions = server_max_fds << 1;

    if ((fp = tmpfile()) == NULL) {
        err(EXIT_FAILURE, "failed to create DMA file");
    }

    if ((ret = ftruncate(fileno(fp), nr_dma_regions * sysconf(_SC_PAGESIZE))) == -1) {
        err(EXIT_FAILURE, "failed to truncate file");
    }

    dma_regions = alloca(sizeof *dma_regions * nr_dma_regions);
    dma_region_fds = alloca(sizeof *dma_region_fds * nr_dma_regions);

    for (i = 0; i < nr_dma_regions; i++) {
        dma_regions[i].addr = i * sysconf(_SC_PAGESIZE);
        dma_regions[i].size = sysconf(_SC_PAGESIZE);
        dma_regions[i].offset = dma_regions[i].addr;
        dma_regions[i].prot = PROT_READ | PROT_WRITE;
        dma_regions[i].flags = VFIO_USER_F_DMA_REGION_MAPPABLE;
        dma_region_fds[i] = fileno(fp);
    }

    map_dma_regions(sock, server_max_fds, dma_regions, dma_region_fds,
                    nr_dma_regions);

    /*
     * XXX VFIO_USER_DEVICE_GET_IRQ_INFO and VFIO_IRQ_SET_ACTION_TRIGGER
     * Query interrupts and configure an eventfd to be associated with INTx.
     */
    irq_fd = configure_irqs(sock);

    dirty_bitmap.argsz = sizeof dirty_bitmap;
    dirty_bitmap.flags = VFIO_IOMMU_DIRTY_PAGES_FLAG_START;
    ret = send_recv_vfio_user_msg(sock, 0, VFIO_USER_DIRTY_PAGES,
                                  &dirty_bitmap, sizeof dirty_bitmap,
                                  NULL, 0, NULL, NULL, 0);
    if (ret != 0) {
        errx(EXIT_FAILURE, "failed to start dirty page logging: %s",
             strerror(-ret));
    }

    /*
     * XXX VFIO_USER_REGION_READ and VFIO_USER_REGION_WRITE
     *
     * BAR0 in the server does not support memory mapping so it must be accessed
     * via explicit messages.
     */
    t = time(NULL) + 1;
    access_bar0(sock, irq_fd, &t);
    
    /* FIXME check that above took at least 1s */

    handle_dma_io(sock, dma_regions, nr_dma_regions, dma_region_fds);

    get_dirty_bitmaps(sock, dma_regions, nr_dma_regions);

    dirty_bitmap.argsz = sizeof dirty_bitmap;
    dirty_bitmap.flags = VFIO_IOMMU_DIRTY_PAGES_FLAG_STOP;
    ret = send_recv_vfio_user_msg(sock, 0, VFIO_USER_DIRTY_PAGES,
                                  &dirty_bitmap, sizeof dirty_bitmap,
                                  NULL, 0, NULL, NULL, 0);
    if (ret != 0) {
        errx(EXIT_FAILURE, "failed to stop dirty page logging: %s",
             strerror(-ret));
    }

    /* BAR1 can be memory mapped and read directly */

    /*
     * XXX VFIO_USER_DMA_UNMAP
     *
     * unmap the first group of the DMA regions
     */
    ret = send_recv_vfio_user_msg(sock, msg_id, VFIO_USER_DMA_UNMAP,
                                  dma_regions, sizeof *dma_regions * server_max_fds,
                                  NULL, 0, NULL, NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to unmap DMA regions: %s", strerror(-ret));
    }

    /*
     * Schedule an interrupt in 2 seconds from now in the old server and then
     * immediatelly migrate the device. The new server should deliver the
     * interrupt. Hopefully 2 seconds should be enough for migration to finish.
     * TODO make this value a command line option.
     */
    t = time(NULL) + 2;
    ret = access_region(sock, LM_DEV_BAR0_REG_IDX, true, 0, &t, sizeof t);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to write to BAR0: %s", strerror(-ret));
    }

    /*
     * By sleeping here for 1s after migration finishes on the source server
     * (but not yet started on the destination server), the timer should be be
     * armed on the destination server for 2-1=1 seconds. If we don't sleep
     * then it will be armed for 2 seconds, which isn't as interesting.
     */
    sleep(1);

    migrate_from(sock, &migr_data, &migr_data_len);

    /*
     * Normally the client would now send the device state to the destination
     * client and then exit. We don't demonstrate how this works as this is a
     * client implementation detail. Instead, the client starts the destination
     * server and then applies the mgiration data.
     */

    sock = migrate_to(argv[optind], client_max_fds, &server_max_fds, &pgsize,
                      migr_data, migr_data_len);

    /*
     * Now we must reconfigure the destination server.
     */

    /*
     * XXX reconfigure DMA regions, note that the first half of the has been
     * unmapped.
     */
    map_dma_regions(sock, server_max_fds, dma_regions + server_max_fds,
                    dma_region_fds + server_max_fds,
                    nr_dma_regions - server_max_fds);

    /* 
     * XXX reconfigure IRQs.
     * FIXME is this something the client needs to do? I would expect so since
     * it's the client that creates and provides the FD. Do we need to save some
     * state in the migration data?
     */
    ret = configure_irqs(sock);
    if (ret < 0) {
        errx(EXIT_FAILURE, "failed to configure IRQs on destination server: %s",
             strerror(-ret));
    }
    irq_fd = ret;

    wait_for_irqs(sock, irq_fd);

    handle_dma_io(sock, dma_regions + server_max_fds,
                  nr_dma_regions - server_max_fds,
                  dma_region_fds + server_max_fds);
    return 0;
}

/* ex: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */
