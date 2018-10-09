/*
 * Copyright (C) Citrix Systems, Inc
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <assert.h>

#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <locale.h>

#include <xenctrl.h>
#include <xen/hvm/ioreq.h>
#include <xenstore.h>

#include <debug.h>
#include <handler.h>
#include <backend.h>

#include "io_port.h"
#include "option.h"

#define mb() asm volatile ("" : : : "memory")

#define XS_VARSTORED_PID_PATH "/local/domain/%u/varstored-pid"

enum {
    VARSTORED_OPT_DOMAIN,
    VARSTORED_OPT_RESUME,
    VARSTORED_OPT_NONPERSISTENT,
    VARSTORED_OPT_PIDFILE,
    VARSTORED_OPT_BACKEND,
    VARSTORED_OPT_ARG,
    VARSTORED_NR_OPTS
    };

static struct option varstored_option[] = {
    {"domain", 1, NULL, 0},
    {"resume", 0, NULL, 0},
    {"nonpersistent", 0, NULL, 0},
    {"pidfile", 1, NULL, 0},
    {"backend", 1, NULL, 0},
    {"arg", 1, NULL, 0},
    {NULL, 0, NULL, 0}
};

static const char *varstored_option_text[] = {
    "<domid>",
    NULL,
    NULL,
    "<pidfile>",
    "<backend>",
    "<name>:<val>",
};

static sig_atomic_t run_main_loop = 1;

static const char *prog;
struct backend *db;
bool opt_resume;
enum log_level log_level = LOG_LVL_INFO;

static void __attribute__((noreturn))
usage(void)
{
    int i;

    fprintf(stderr, "Usage: %s <options>\n\n", prog);

    for (i = 0; i < VARSTORED_NR_OPTS; i++) {
        if (varstored_option[i].has_arg) {
            fprintf(stderr, "\t--%s %s\n",
                    varstored_option[i].name,
                    varstored_option_text[i]);
        } else {
            fprintf(stderr, "\t--%s\n", varstored_option[i].name);
        }
    }

    fprintf(stderr, "\n");

    exit(2);
}

typedef enum {
    VARSTORED_SEQ_UNINITIALIZED = 0,
    VARSTORED_SEQ_INTERFACE_OPEN,
    VARSTORED_SEQ_SERVER_REGISTERED,
    VARSTORED_SEQ_SHARED_IOPAGE_MAPPED,
    VARSTORED_SEQ_BUFFERED_IOPAGE_MAPPED,
    VARSTORED_SEQ_SERVER_ENABLED,
    VARSTORED_SEQ_PORT_ARRAY_ALLOCATED,
    VARSTORED_SEQ_EVTCHN_OPEN,
    VARSTORED_SEQ_PORTS_BOUND,
    VARSTORED_SEQ_BUF_PORT_BOUND,
    VARSTORED_SEQ_PORT_INITIALIZED,
    VARSTORED_SEQ_WROTE_PID,
    VARSTORED_SEQ_INITIALIZED,
    VARSTORED_NR_SEQS
} varstored_seq_t;

typedef struct varstored_state {
    varstored_seq_t     seq;
    xc_interface        *xch;
    xc_evtchn           *xceh;
    domid_t             domid;
    unsigned int        vcpus;
    ioservid_t          ioservid;
    shared_iopage_t     *iopage;
    evtchn_port_t       *ioreq_local_port;
    buffered_iopage_t   *buffered_iopage;
    evtchn_port_t       buf_ioreq_port;
    evtchn_port_t       buf_ioreq_local_port;
} varstored_state_t;

static varstored_state_t varstored_state;

/*
 * Initialize various settings from xenstore.
 */
static void
initialize_settings(struct xs_handle *xsh, domid_t domid)
{
    char path[64];
    char *s;

    snprintf(path, sizeof(path), "/local/domain/%u/platform/secureboot", domid);
    s = xs_read(xsh, XBT_NULL, path, NULL);
    secure_boot_enable = s && !strcmp(s, "true");
    if (secure_boot_enable)
        INFO("SECURE_BOOT_ON\n");
    else
        INFO("SECURE_BOOT_OFF\n");
    free(s);

    snprintf(path, sizeof(path),
             "/local/domain/%u/platform/auth-enforce", domid);
    s = xs_read(xsh, XBT_NULL, path, NULL);

    auth_enforce = !s || strcmp(s, "false");
    free(s);

    INFO("Authenticated variables: %s\n",
         auth_enforce ? "enforcing" : "permissive");
}

static bool
xs_write_pid(struct xs_handle *xsh, domid_t domid)
{
    char *varstore_pid = NULL;
    char *key = NULL;
    bool ret = false;
    pid_t pid = getpid();

    /* pid needs to be written to /local/domain/<domid>/varstored-pid */
    if (asprintf(&varstore_pid, "%u", pid) != -1)
        if (asprintf(&key, XS_VARSTORED_PID_PATH, varstored_state.domid) != -1)
            ret = xs_write(xsh, 0, key, varstore_pid, strlen(varstore_pid));

    free(key);
    free(varstore_pid);
    return ret;
}

static bool
create_pidfile(const char *path)
{
    int fd, len;
    char *pid;

    fd = open(path, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0600);
    if (fd == -1) {
        ERR("Could not open pidfile '%s': %d, %s\n",
            path, errno, strerror(errno));
        return false;
    }

    if (lockf(fd, F_TLOCK, 0) == -1) {
        ERR("Failed to lock pidfile\n");
        close(fd);
        return false;
    }

    len = asprintf(&pid, "%u\n", getpid());
    if (len == -1) {
        ERR("Out of memory\n");
        close(fd);
        return false;
    }

    if (write(fd, pid, len) != len) {
        ERR("Failed to write to pidfile\n");
        free(pid);
        close(fd);
        return false;
    }
    free(pid);

    /* Leave pid file open and locked. */
    return true;
}

static void
handle_pio(ioreq_t *ioreq)
{
    if (ioreq->dir == IOREQ_READ) {
        DBG("IO request not WRITE. Doing nothing.\n");
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            io_port_write(ioreq->addr, ioreq->size, (uint32_t)ioreq->data);
        } else {
            assert(0);
        }
    }
}

static void
handle_ioreq(ioreq_t *ioreq)
{
    switch (ioreq->type) {
    case IOREQ_TYPE_PIO:
        handle_pio(ioreq);
        break;

    case IOREQ_TYPE_COPY:
        break;

    case IOREQ_TYPE_PCI_CONFIG:
        break;

    case IOREQ_TYPE_TIMEOFFSET:
        break;

    case IOREQ_TYPE_INVALIDATE:
        break;

    default:
        ERR("UNKNOWN (%02x)", ioreq->type);
        break;
    }
}

static void
varstored_seq_next(void)
{
    assert(varstored_state.seq < VARSTORED_SEQ_INITIALIZED);

    switch (++varstored_state.seq) {
    case VARSTORED_SEQ_INTERFACE_OPEN:
        INFO(">INTERFACE_OPEN\n");
        break;

    case VARSTORED_SEQ_SERVER_REGISTERED:
        INFO(">SERVER_REGISTERED\n");
        INFO("ioservid = %u\n", varstored_state.ioservid);
        break;

    case VARSTORED_SEQ_SHARED_IOPAGE_MAPPED:
        INFO(">SHARED_IOPAGE_MAPPED\n");
        INFO("iopage = %p\n", varstored_state.iopage);
        break;

    case VARSTORED_SEQ_BUFFERED_IOPAGE_MAPPED:
        INFO(">BUFFERED_IOPAGE_MAPPED\n");
        INFO("buffered_iopage = %p\n", varstored_state.buffered_iopage);
        break;

    case VARSTORED_SEQ_SERVER_ENABLED:
        INFO(">SERVER_ENABLED\n");
        break;

    case VARSTORED_SEQ_PORT_ARRAY_ALLOCATED:
        INFO(">PORT_ARRAY_ALLOCATED\n");
        break;

    case VARSTORED_SEQ_EVTCHN_OPEN:
        INFO(">EVTCHN_OPEN\n");
        break;

    case VARSTORED_SEQ_PORTS_BOUND: {
        int i;

        INFO(">EVTCHN_PORTS_BOUND\n");

        for (i = 0; i < varstored_state.vcpus; i++)
            INFO("VCPU%d: %u -> %u\n", i,
                varstored_state.iopage->vcpu_ioreq[i].vp_eport,
                varstored_state.ioreq_local_port[i]);

        break;
    }

    case VARSTORED_SEQ_BUF_PORT_BOUND:
        INFO(">EVTCHN_BUF_PORT_BOUND\n");

        INFO("%u -> %u\n",
            varstored_state.buf_ioreq_port,
            varstored_state.buf_ioreq_local_port);
        break;

    case VARSTORED_SEQ_PORT_INITIALIZED:
        INFO(">PORT_INITIALIZED\n");
        break;

    case VARSTORED_SEQ_WROTE_PID:
        INFO(">WROTE_PID\n");
        break;

    case VARSTORED_SEQ_INITIALIZED:
        INFO(">INITIALIZED\n");
        break;

    default:
        assert(0);
        break;
    }
}

static void
varstored_teardown(void)
{
    if (varstored_state.seq == VARSTORED_SEQ_INITIALIZED) {
        INFO("<INITIALIZED\n");

        varstored_state.seq = VARSTORED_SEQ_WROTE_PID;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_WROTE_PID) {
        char *key;
        struct xs_handle *xsh;

        INFO("<WROTE_PID\n");

        xsh = xs_open(0);
        if (xsh) {
            if (asprintf(&key, XS_VARSTORED_PID_PATH, varstored_state.domid) != -1) {
                xs_rm(xsh, 0, key);
                free(key);
            } else {
                INFO("Couldn't remove varstore pid from xenstore\n");
            }
            xs_close(xsh);
        } else {
            fprintf(stderr, "Couldn't open xenstore");
        }

        varstored_state.seq = VARSTORED_SEQ_PORT_INITIALIZED;
    }

    if (varstored_state.seq == VARSTORED_SEQ_PORT_INITIALIZED) {
        INFO("<PORT_INITIALIZED\n");
        io_port_deregister();

        varstored_state.seq = VARSTORED_SEQ_PORTS_BOUND;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_PORTS_BOUND) {
        INFO("<EVTCHN_BUF_PORT_BOUND\n");
        evtchn_port_t   port;

        port = varstored_state.buf_ioreq_local_port;

        INFO("%u\n", port);
        (void) xc_evtchn_unbind(varstored_state.xceh, port);

        varstored_state.seq = VARSTORED_SEQ_PORTS_BOUND;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_PORTS_BOUND) {
        INFO("<EVTCHN_PORTS_BOUND\n");

        varstored_state.seq = VARSTORED_SEQ_EVTCHN_OPEN;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_EVTCHN_OPEN) {
        int i;

        INFO("<EVTCHN_OPEN\n");

        for (i = 0; i < varstored_state.vcpus; i++) {
            evtchn_port_t   port;

            port = varstored_state.ioreq_local_port[i];

            if (port >= 0) {
                INFO("VCPU%d: %u\n", i, port);
                (void) xc_evtchn_unbind(varstored_state.xceh, port);
            }
        }

        xc_evtchn_close(varstored_state.xceh);

        varstored_state.seq = VARSTORED_SEQ_PORT_ARRAY_ALLOCATED;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_PORT_ARRAY_ALLOCATED) {
        INFO("<PORT_ARRAY_ALLOCATED\n");

        free(varstored_state.ioreq_local_port);

        varstored_state.seq = VARSTORED_SEQ_SERVER_ENABLED;
    }

    if (varstored_state.seq == VARSTORED_SEQ_SERVER_ENABLED) {
        INFO("<SERVER_ENABLED\n");
        (void) xc_hvm_set_ioreq_server_state(varstored_state.xch,
                                             varstored_state.domid,
                                             varstored_state.ioservid,
                                             0);

        varstored_state.seq = VARSTORED_SEQ_BUFFERED_IOPAGE_MAPPED;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_BUFFERED_IOPAGE_MAPPED) {
        INFO("<BUFFERED_IOPAGE_MAPPED\n");

        munmap(varstored_state.buffered_iopage, XC_PAGE_SIZE);

        varstored_state.seq = VARSTORED_SEQ_SHARED_IOPAGE_MAPPED;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_SHARED_IOPAGE_MAPPED) {
        INFO("<SHARED_IOPAGE_MAPPED\n");

        munmap(varstored_state.iopage, XC_PAGE_SIZE);

        varstored_state.seq = VARSTORED_SEQ_SERVER_REGISTERED;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_SERVER_REGISTERED) {
        INFO("<SERVER_REGISTERED\n");

        (void) xc_hvm_destroy_ioreq_server(varstored_state.xch,
                                           varstored_state.domid,
                                           varstored_state.ioservid);
        varstored_state.seq = VARSTORED_SEQ_INTERFACE_OPEN;
    }

    if (varstored_state.seq >= VARSTORED_SEQ_INTERFACE_OPEN) {
        INFO("<INTERFACE_OPEN\n");

        xc_interface_close(varstored_state.xch);

        varstored_state.seq = VARSTORED_SEQ_UNINITIALIZED;
    }
}

static struct sigaction sigterm_handler;

static void
varstored_sigterm(int num)
{
    INFO("%s\n", strsignal(num));

    varstored_teardown();

    if (num == SIGTERM)
        run_main_loop = 0;
    else
        exit(0);
}

static struct sigaction sigusr1_handler;

static void
varstored_sigusr1(int num)
{
    INFO("%s\n", strsignal(num));

    sigaction(SIGHUP, &sigusr1_handler, NULL);
}

static int
varstored_initialize(domid_t domid)
{
    int rc, i, subcount = 0, first = 1;
    uint64_t number = 0;
    xc_dominfo_t dominfo;
    unsigned long pfn;
    unsigned long buf_pfn;
    evtchn_port_t port;
    evtchn_port_t buf_port;
    struct xs_handle *xsh;

    varstored_state.domid = domid;

    varstored_state.xch = xc_interface_open(NULL, NULL, 0);
    if (varstored_state.xch == NULL)
        goto fail1;

    varstored_seq_next();

    rc = xc_domain_getinfo(varstored_state.xch, varstored_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != varstored_state.domid)
        goto fail2;

    varstored_state.vcpus = dominfo.max_vcpu_id + 1;

    INFO("%d vCPU(s)\n", varstored_state.vcpus);

    do {
        rc = xc_hvm_param_get(varstored_state.xch, varstored_state.domid,
                              HVM_PARAM_NR_IOREQ_SERVER_PAGES, &number);

        if (rc < 0) {
            ERR("xc_hvm_param_get failed: %d, %s", errno, strerror(errno));
            exit(1);
        }

        if (first || number > 0)
            INFO("HVM_PARAM_NR_IOREQ_SERVER_PAGES = %ld\n", number);
        first = 0;

        if (number == 0) {
            if (!subcount)
                INFO("Waiting for ioreq server");
            usleep(100000);
            subcount++;
            if (subcount > 10)
                subcount = 0;
        }
    } while (number == 0);

    rc = xc_hvm_create_ioreq_server(varstored_state.xch, varstored_state.domid, 1,
                                    &varstored_state.ioservid);
    if (rc < 0)
        goto fail3;

    varstored_seq_next();

    rc = xc_hvm_get_ioreq_server_info(varstored_state.xch, varstored_state.domid,
                                      varstored_state.ioservid, &pfn, &buf_pfn, &buf_port);
    if (rc < 0)
        goto fail4;

    varstored_state.iopage = xc_map_foreign_range(varstored_state.xch,
                                             varstored_state.domid,
                                             XC_PAGE_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             pfn);
    if (varstored_state.iopage == NULL)
        goto fail5;

    varstored_seq_next();

    varstored_state.buffered_iopage = xc_map_foreign_range(varstored_state.xch,
                                                      varstored_state.domid,
                                                      XC_PAGE_SIZE,
                                                      PROT_READ | PROT_WRITE,
                                                      buf_pfn);
    if (varstored_state.buffered_iopage == NULL)
        goto fail6;

    varstored_seq_next();

    rc = xc_hvm_set_ioreq_server_state(varstored_state.xch,
                                       varstored_state.domid,
                                       varstored_state.ioservid,
                                       1);
    if (rc != 0)
        goto fail7;

    varstored_seq_next();

    varstored_state.ioreq_local_port = malloc(sizeof (evtchn_port_t) *
                                         varstored_state.vcpus);
    if (varstored_state.ioreq_local_port == NULL)
        goto fail8;

    for (i = 0; i < varstored_state.vcpus; i++)
        varstored_state.ioreq_local_port[i] = -1;

    varstored_seq_next();

    varstored_state.xceh = xc_evtchn_open(NULL, 0);
    if (varstored_state.xceh == NULL)
        goto fail9;

    varstored_seq_next();

    for (i = 0; i < varstored_state.vcpus; i++) {
        port = varstored_state.iopage->vcpu_ioreq[i].vp_eport;

        rc = xc_evtchn_bind_interdomain(varstored_state.xceh, varstored_state.domid,
                                        port);
        if (rc < 0)
            goto fail10;

        varstored_state.ioreq_local_port[i] = rc;
    }

    varstored_seq_next();

    rc = xc_evtchn_bind_interdomain(varstored_state.xceh, varstored_state.domid,
                                    buf_port);
    if (rc < 0)
        goto fail11;

    varstored_state.buf_ioreq_local_port = rc;

    varstored_seq_next();

    rc = io_port_initialize(varstored_state.xch, varstored_state.domid,
                            varstored_state.ioservid);

    if (rc < 0)
        goto fail12;

    varstored_seq_next();

    xsh = xs_open(0);
    if (xsh == NULL) {
        fprintf(stderr, "Couldn't open xenstore");
        goto fail12;
    }

    initialize_settings(xsh, varstored_state.domid);

    if (opt_resume) {
        if (!db->resume())
            goto fail13;
    } else {
        enum backend_init_status status = db->init();

        if (status == BACKEND_INIT_FAILURE)
            goto fail13;

        if (!setup_variables())
            goto fail13;

        if (status == BACKEND_INIT_FIRSTBOOT) {
            if (!setup_keys())
                goto fail13;
        }
    }

    if (!xs_write_pid(xsh, varstored_state.domid))
        goto fail13;

    xs_close(xsh);

    varstored_seq_next();

    varstored_seq_next();

    assert(varstored_state.seq == VARSTORED_SEQ_INITIALIZED);
    return 0;

fail13:
    xs_close(xsh);
    ERR("fail13\n");

fail12:
    ERR("fail12\n");

fail11:
    ERR("fail11\n");

fail10:
    ERR("fail10\n");

fail9:
    ERR("fail9\n");

fail8:
    ERR("fail8\n");

fail7:
    ERR("fail7\n");

fail6:
    ERR("fail6\n");

fail5:
    ERR("fail5\n");

fail4:
    ERR("fail4\n");

fail3:
    ERR("fail3\n");

fail2:
    ERR("fail2\n");

fail1:
    ERR("fail1\n");

    warn("fail");
    return -1;
}

static void
varstored_poll_buffered_iopage(void)
{
    if (varstored_state.seq != VARSTORED_SEQ_INITIALIZED)
        return;

    for (;;) {
        unsigned int    read_pointer;
        unsigned int    write_pointer;

        read_pointer = varstored_state.buffered_iopage->read_pointer;
        write_pointer = varstored_state.buffered_iopage->write_pointer;

        if (read_pointer == write_pointer)
            break;

        while (read_pointer != write_pointer) {
            unsigned int    slot;
            buf_ioreq_t     *buf_ioreq;
            ioreq_t         ioreq;

            slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;

            buf_ioreq = &varstored_state.buffered_iopage->buf_ioreq[slot];

            ioreq.size = 1UL << buf_ioreq->size;
            ioreq.count = 1;
            ioreq.addr = buf_ioreq->addr;
            ioreq.data = buf_ioreq->data;
            ioreq.state = STATE_IOREQ_READY;
            ioreq.dir = buf_ioreq->dir;
            ioreq.df = 1;
            ioreq.type = buf_ioreq->type;
            ioreq.data_is_ptr = 0;

            read_pointer++;

            if (ioreq.size == 8) {
                slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;
                buf_ioreq = &varstored_state.buffered_iopage->buf_ioreq[slot];

                ioreq.data |= ((uint64_t)buf_ioreq->data) << 32;

                read_pointer++;
            }

            handle_ioreq(&ioreq);
            mb();
        }

        varstored_state.buffered_iopage->read_pointer = read_pointer;
        mb();
    }
}

static void
varstored_poll_iopage(unsigned int i)
{
    ioreq_t         *ioreq;

    if (varstored_state.seq != VARSTORED_SEQ_INITIALIZED)
        return;

    ioreq = &varstored_state.iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY) {
        fprintf(stderr, "IO request not ready\n");
        return;
    }
    mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    handle_ioreq(ioreq);
    mb();

    ioreq->state = STATE_IORESP_READY;
    mb();

    xc_evtchn_notify(varstored_state.xceh, varstored_state.ioreq_local_port[i]);
}

static void
varstored_poll_iopages(void)
{
    evtchn_port_t   port;
    int             i;

    if (varstored_state.seq != VARSTORED_SEQ_INITIALIZED)
        return;

    port = xc_evtchn_pending(varstored_state.xceh);
    if (port < 0)
        return;

    if (port == varstored_state.buf_ioreq_local_port) {
        xc_evtchn_unmask(varstored_state.xceh, port);
        varstored_poll_buffered_iopage();
    } else {
        for (i = 0; i < varstored_state.vcpus; i++) {
            if (port == varstored_state.ioreq_local_port[i]) {
                xc_evtchn_unmask(varstored_state.xceh, port);
                varstored_poll_iopage(i);
            }
        }
    }
}

int
main(int argc, char **argv, char **envp)
{
    char            *domain_str;
    char            *ptr;
    int             index;
    char            *end;
    domid_t         domid;
    sigset_t        block;
    struct pollfd   pfd;
    int             rc;

    prog = basename(argv[0]);

    domain_str = NULL;

    for (;;) {
        char    c;

        c = getopt_long(argc, argv, "", varstored_option, &index);
        if (c == -1)
            break;

        if (c != 0) {
            usage();
            /*NOTREACHED*/
        }

        INFO("--%s = '%s'\n", varstored_option[index].name, optarg);

        switch (index) {
        case VARSTORED_OPT_DOMAIN:
            domain_str = optarg;
            break;

        case VARSTORED_OPT_RESUME:
            opt_resume = true;
            break;

        case VARSTORED_OPT_NONPERSISTENT:
            persistent = false;
            break;

        case VARSTORED_OPT_PIDFILE:
            if (!create_pidfile(optarg))
                exit(1);
            break;

        case VARSTORED_OPT_BACKEND:
            if (!strcmp(optarg, "xapidb")) {
                db = &xapidb;
            } else {
                fprintf(stderr, "Invalid backend '%s'\n", optarg);
                usage();
            }
            break;

        case VARSTORED_OPT_ARG:
            if (!db) {
                fprintf(stderr, "Must set backend before backend args\n");
                usage();
            }
            ptr = strchr(optarg, ':');
            if (!ptr) {
                fprintf(stderr, "Invalid argument '%s'\n", optarg);
                usage();
            }
            *ptr = '\0';
            ptr++;
            if (!db->parse_arg(optarg, ptr)) {
                fprintf(stderr, "Invalid argument '%s:%s'\n", optarg, ptr);
                usage();
            }
            break;

        default:
            assert(0);
            break;
        }
    }

    if (domain_str == NULL ||
        db == NULL ||
        !db->check_args()) {
        usage();
        /*NOTREACHED*/
    }

    domid = (domid_t)strtol(domain_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid domain '%s'\n", domain_str);
        exit(1);
    }

    sigfillset(&block);

    memset(&sigterm_handler, 0, sizeof (struct sigaction));
    sigterm_handler.sa_handler = varstored_sigterm;

    sigaction(SIGTERM, &sigterm_handler, NULL);
    sigdelset(&block, SIGTERM);

    sigaction(SIGINT, &sigterm_handler, NULL);
    sigdelset(&block, SIGINT);

    sigaction(SIGHUP, &sigterm_handler, NULL);
    sigdelset(&block, SIGHUP);

    sigaction(SIGABRT, &sigterm_handler, NULL);
    sigdelset(&block, SIGABRT);

    memset(&sigusr1_handler, 0, sizeof (struct sigaction));
    sigusr1_handler.sa_handler = varstored_sigusr1;

    sigaction(SIGUSR1, &sigusr1_handler, NULL);
    sigdelset(&block, SIGUSR1);

    sigprocmask(SIG_BLOCK, &block, NULL);

    rc = varstored_initialize(domid);
    if (rc < 0) {
        varstored_teardown();
        exit(1);
    }

    pfd.fd = xc_evtchn_fd(varstored_state.xceh);
    pfd.events = POLLIN | POLLERR | POLLHUP;
    pfd.revents = 0;

    while (run_main_loop) {
        rc = poll(&pfd, 1, 5000);

        if (!run_main_loop)
            break;

        if (rc > 0 && pfd.revents & POLLIN)
            varstored_poll_iopages();

        if (rc < 0 && errno != EINTR)
            break;
    }

    if (!db->save())
        return 1;

    return 0;
}
