#include <config.h>
#include <errno.h>
#include <string.h>
#include <PI/proto/pi_server.h>
#include <PI/p4info.h>
#include <PI/int/pi_int.h>
#include <PI/pi.h>
#include <PI/target/pi_imp.h>

#include "netdev.h"
#include "p4rt.h"
#include "p4rt-provider.h"

#include "openvswitch/hmap.h"
#include "hash.h"
#include "openvswitch/vlog.h"
#include "ovs-rcu.h"
#include "sset.h"
#include "lib/dpif.h"

VLOG_DEFINE_THIS_MODULE(p4rt);

/* ## ------------------------------------- ## */
/* ## Global (shared) objects used by p4rt. ## */
/* ## ------------------------------------- ## */

/* Map from datapath name to struct p4rt, for use by unixctl commands. */
static struct hmap all_p4rts = HMAP_INITIALIZER(&all_p4rts);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

/* All registered p4rt classes, in probe order. */
static const struct p4rt_class **p4rt_classes;
static size_t n_p4rt_classes;
static size_t allocated_p4rt_classes;

/* Global lock that protects all flow table operations. */
struct ovs_mutex p4rt_mutex = OVS_MUTEX_INITIALIZER;

/* ## ------------------------- ## */
/* ## Prototypes for functions. ## */
/* ## ------------------------- ## */


struct p4port *p4rt_get_port(const struct p4rt *p4rt, ofp_port_t port_no);
int p4rt_class_register(const struct p4rt_class *new_class);
static void p4port_destroy(struct p4port *, bool del);
static void p4rt_destroy__(struct p4rt *p);

/* ## --------------------------------------- ## */
/* ## Private functions used locally by p4rt. ## */
/* ## --------------------------------------- ## */

static struct p4rt *
p4rt_lookup_by_dev_id(uint64_t dev_id)
{
    struct p4rt *p4rt;

    HMAP_FOR_EACH (p4rt, hmap_node, &all_p4rts) {
        if (p4rt->dev_id == dev_id) {
            return p4rt;
        }
    }
    return NULL;
}

static const struct p4rt_class *
p4rt_class_find__(const char *type)
{
    size_t i;

    for (i = 0; i < n_p4rt_classes; i++) {
        const struct p4rt_class *class = p4rt_classes[i];
        struct sset types;
        bool found;

        sset_init(&types);
        class->enumerate_types(&types);
        found = sset_contains(&types, type);
        sset_destroy(&types);

        if (found) {
            return class;
        }
    }
    VLOG_WARN("unknown datapath type %s", type);
    return NULL;
}

/* Registers a new p4rt class.  After successful registration, new p4rts
 * of that type can be created using p4rt_create(). */
int
p4rt_class_register(const struct p4rt_class *new_class)
{
    size_t i;

    for (i = 0; i < n_p4rt_classes; i++) {
        if (p4rt_classes[i] == new_class) {
            return EEXIST;
        }
    }

    if (n_p4rt_classes >= allocated_p4rt_classes) {
        p4rt_classes = x2nrealloc(p4rt_classes,
                                  &allocated_p4rt_classes, sizeof *p4rt_classes);
    }
    p4rt_classes[n_p4rt_classes++] = new_class;
    return 0;
}

static void
p4rt_port_destroy(struct p4rt_port *port)
{
    free(port->name);
    free(port->type);
}

static void
p4rt_destroy_defer__(struct p4rt *p)
    OVS_EXCLUDED(p4rt_mutex)
{
        ovsrcu_postpone(p4rt_destroy__, p);
}

static void
p4rt_destroy__(struct p4rt *p)
    OVS_EXCLUDED(p4rt_mutex)
{
    ovs_mutex_lock(&p4rt_mutex);
    hmap_remove(&all_p4rts, &p->hmap_node);
    ovs_mutex_unlock(&p4rt_mutex);

    free(p->name);
    free(p->type);
    hmap_destroy(&p->ports);

    p->p4rt_class->dealloc(p);
}

static void
p4rt_program_destroy(struct program *prog)
{
    if (prog) {
        prog->p4rt->p4rt_class->prog_del(prog);
        prog->p4rt->p4rt_class->prog_dealloc(prog);
    }
}

static int
p4rt_port_query_by_name(struct p4rt *p4rt, const char *name, struct p4rt_port *portp)
{
    int error;
    error = p4rt->p4rt_class->port_query_by_name(p4rt, name, portp);
    if (error) {
        memset(portp, 0, sizeof *portp);
    }
    return error;
}

static ofp_port_t
alloc_p4rt_port(struct p4rt *p4rt OVS_UNUSED, const char *netdev_name OVS_UNUSED)
{
    /* TODO: get next port number from the pool. */
    return u16_to_ofp(1);
}

static int
p4rt_port_open(struct p4rt *p4rt OVS_UNUSED,
               struct p4rt_port *p4port,
               struct netdev **p_netdev)
{
    int error;
    struct netdev *netdev;

    *p_netdev = NULL;
    error = netdev_open(p4port->name, p4port->type, &netdev);
    if (error) {
        VLOG_WARN_RL(&rl, "%s: ignoring port %s (%"PRIu32") because netdev %s "
                                                         "cannot be opened (%s)",
                p4port->name,
                p4port->name, p4port->port_no,
                p4port->name, ovs_strerror(error));
        return 0;
    }

    if (p4port->port_no == OFPP_NONE) {
        if (!strcmp(p4rt->name, p4port->name)) {
            p4port->port_no = OFPP_LOCAL;
        } else {
            ofp_port_t port_no = alloc_p4rt_port(p4rt, p4port->name);
            if (port_no == OFPP_NONE) {
                VLOG_WARN_RL(&rl, "%s: failed to allocate port number "
                                  "for %s.", p4rt->name, p4port->name);
                netdev_close(netdev);
                return ENOSPC;
            }
            p4port->port_no = port_no;
        }
    }

    *p_netdev = netdev;
    return 0;
}

static void
p4port_destroy__(struct p4port *port)
{
    struct p4rt *p4rt = port->p4rt;

    hmap_remove(&p4rt->ports, &port->hmap_node);

    netdev_close(port->netdev);
    p4rt->p4rt_class->port_dealloc(port);
}

static void
p4port_destroy(struct p4port *port, bool del)
{
    if (port) {
        port->p4rt->p4rt_class->port_destruct(port, del);
        p4port_destroy__(port);
    }
}

static void
p4port_remove(struct p4port *p4port)
{
    p4port_destroy(p4port, true);
}

static struct p4port *
p4rt_get_port_by_name(const struct p4rt *p4rt, const char *name)
{
    struct p4port *port;

    HMAP_FOR_EACH (port, hmap_node, &p4rt->ports) {
        if (!strcmp(netdev_get_name(port->netdev), name)) {
            return port;
        }
    }

    return NULL;
}

static int
p4port_install(struct p4rt *p4rt, struct netdev *netdev, ofp_port_t port_no)
{
    const char *netdev_name = netdev_get_name(netdev);
    struct p4port *p4port;
    int error;

    /* Create p4port. */
    p4port = p4rt->p4rt_class->port_alloc();
    if (!p4port) {
        error = ENOMEM;
        goto error;
    }

    p4port->p4rt = p4rt;
    p4port->netdev = netdev;
    p4port->port_no = port_no;
    p4port->created = time_msec();

    /* Add port to 'p'. */
    hmap_insert(&p4rt->ports, &p4port->hmap_node,
                hash_ofp_port(p4port->port_no));

    /* Let the p4rt_class initialize its private data. */
    error = p4rt->p4rt_class->port_construct(p4port);
    if (error) {
        goto error;
    }

    return 0;

    error:
    VLOG_INFO("%s: could not add port %s (%s)",
              p4rt->name, netdev_name, ovs_strerror(error));
    VLOG_WARN_RL(&rl, "%s: could not add port %s (%s)",
                 p4rt->name, netdev_name, ovs_strerror(error));
    if (p4port) {
        p4port_destroy__(p4port);
    } else {
        netdev_close(netdev);
    }
    return error;
}

static int
update_port(struct p4rt *p4rt, const char *name)
{
    struct p4rt_port p4rt_port;
    struct netdev *netdev;
    struct p4port *port;
    int error = 0;

    /* Fetch 'name''s location and properties from the datapath. */
    if (p4rt_port_query_by_name(p4rt, name, &p4rt_port)) {
        netdev = NULL;
    } else {
        error = p4rt_port_open(p4rt, &p4rt_port, &netdev);
    }

    if (netdev) {
        port = p4rt_get_port(p4rt, p4rt_port.port_no);
        if (port && !strcmp(netdev_get_name(port->netdev), name)) {

        } else {
            if (port) {
                p4port_remove(port);
            }
            error = p4port_install(p4rt, netdev, p4rt_port.port_no);
        }
    }
    p4rt_port_destroy(&p4rt_port);

    return error;
}


/* ## ------------------------------------- ## */
/* ## Functions exposed and used by bridge. ## */
/* ## ------------------------------------- ## */

int
p4rt_enumerate_names(const char *type, struct sset *names)
{
    const struct p4rt_class *class = p4rt_class_find__(type);
    return class ? class->enumerate_names(type, names) : EAFNOSUPPORT;
}

const char *
p4rt_port_open_type(const struct p4rt *p4rt OVS_UNUSED, const char *port_type)
{
    /* FIXME: So far, P4rt switch can only be implemented in userspace. */
    if (!strcmp(port_type, "internal")) {
        return "tap";
    }

    return port_type;
}

/* Clears 'types' and enumerates all registered p4rt types into it.  The
 * caller must first initialize the sset. */
void
p4rt_enumerate_types(struct sset *types)
{
    size_t i;

    sset_clear(types);
    for (i = 0; i < n_p4rt_classes; i++) {
        p4rt_classes[i]->enumerate_types(types);
    }
}

void
p4rt_init(void)
{
    p4rt_class_register(&p4rt_dpif_class);
    size_t i;
    for (i = 0; i < n_p4rt_classes; i++) {
        p4rt_classes[i]->init();
    }

    /* FIXME: Workaround as we cannot call DeviceMgr::init().
     * Remove it once this issue: https://github.com/p4lang/PI/issues/512 will be solved. */
    pi_init(256, NULL);
    PIGrpcServerRun();
}

void
p4rt_deinit(void)
{
    PIGrpcServerShutdown();
    PIGrpcServerCleanup();
}

int
p4rt_run(struct p4rt *p4rt)
{
    int error;

    error = p4rt->p4rt_class->run(p4rt);
    if (error && error != EAGAIN) {
        VLOG_ERR_RL(&rl, "%s: run failed (%s)", p4rt->name, ovs_strerror(error));
    }

    return error;
}

void
p4rt_wait(struct p4rt *p)
{
    p->p4rt_class->wait(p);
}

int
p4rt_create(const char *datapath_name, const char *datapath_type,
            struct p4rt **p4rtp)
    OVS_EXCLUDED(p4rt_mutex)
{
    const struct p4rt_class *class;
    int error;
    struct p4rt *p4rt;
    *p4rtp = NULL;

    datapath_type = dpif_normalize_type(datapath_type);
    class = p4rt_class_find__(datapath_type);
    if (!class) {
        VLOG_WARN("could not create datapath %s of unknown type %s",
                  datapath_name, datapath_type);
        return EAFNOSUPPORT;
    }

    p4rt = class->alloc();
    if (!p4rt) {
        VLOG_ERR("failed to allocate datapath %s of type %s",
                 datapath_name, datapath_type);
        return ENOMEM;
    }

    /* Initialize. */
    ovs_mutex_lock(&p4rt_mutex);
    memset(p4rt, 0, sizeof *p4rt);
    p4rt->p4rt_class = class;
    p4rt->name = xstrdup(datapath_name);

    p4rt->p4info = NULL;
    /* TODO: 0 is hardcoded, need to assign device id dynamically. */
    p4rt->dev_id = 0;
    p4rt->type = xstrdup(datapath_type);
    hmap_insert(&all_p4rts, &p4rt->hmap_node,
    hash_string(p4rt->name, 0));
    hmap_init(&p4rt->ports);
    ovs_mutex_unlock(&p4rt_mutex);

    error = p4rt->p4rt_class->construct(p4rt);
    if (error) {
        VLOG_ERR("failed to open datapath %s: %s",
                 datapath_name, ovs_strerror(error));
        p4rt_destroy__(p4rt);
        return error;
    }

    *p4rtp = p4rt;
    return error;
}

int
p4rt_initialize_datapath(struct p4rt *p, const char *filename)
{
    int error = 0;
    struct program *prog = NULL;

    if (p->prog) {
        /* P4 datapath is already initialized with P4 program */
        return error;
    }

    FILE *stream = !strcmp(filename, "-") ? stdin : fopen(filename, "r");
    if (stream == NULL) {
        error = ENOENT;
        VLOG_WARN_RL(&rl, "failed to initialize P4 datapath of %s "
                          "with binary from file '%s' (%s)",
                     p->name, filename, ovs_strerror(error));
        return error;
    }

    fseek(stream, 0L, SEEK_END);
    size_t length = ftell(stream);
    fseek(stream, 0L, SEEK_SET);

    char *program = xzalloc(length);
    if (fread(program, sizeof(char), length, stream) != length) {
        error = ferror(stream) ? errno : EOF;
        goto error;
    }
    fclose(stream);

    prog = p->p4rt_class->program_alloc();
    if (!prog) {
        error = ENOMEM;
        goto error;
    }

    *CONST_CAST(struct p4rt **, &prog->p4rt) = p;
    prog->data = program;
    prog->data_len = length;

    error = p->p4rt_class->program_insert(prog);
    if (error) {
        goto error;
    }

    p->prog = prog;

    return 0;

error:
    VLOG_WARN_RL(&rl, "failed to initialize P4 datapath of %s "
                      "with binary from file '%s' (%s)",
                 p->name, filename, ovs_strerror(error));
    if (prog) {
        /* TODO: dealloc program here. */
    } else {
        fclose(stream);
    }
    return error;
}

void
p4rt_destroy(struct p4rt *p, bool del)
{
    struct p4port *port, *next_port;

    if (!p) {
        return;
    }

    HMAP_FOR_EACH_SAFE (port, next_port, hmap_node, &p->ports) {
        p4port_destroy(port, del);
    }

    p4rt_program_destroy(p->prog);

    p->p4rt_class->destruct(p, del);

    /* Destroying rules is deferred, must have 'p4rt' around for them. */
    ovsrcu_postpone(p4rt_destroy_defer__, p);
}

int
p4rt_delete(const char *name, const char *type)
{
    const struct p4rt_class *class = p4rt_class_find__(type);
    return (!class ? EAFNOSUPPORT
            : !class->del ? EACCES
            : class->del(type, name));
}

int
p4rt_type_run(const char *datapath_type)
{
    const struct p4rt_class *class;
    int error;

    datapath_type = datapath_type && datapath_type[0] ? datapath_type : "system";
    class = p4rt_class_find__(datapath_type);

    error = class->type_run ? class->type_run(datapath_type) : 0;
    if (error && error != EAGAIN) {
        VLOG_ERR_RL(&rl, "%s: type_run failed (%s)",
                    datapath_type, ovs_strerror(error));
    }

    return error;
}

void
p4rt_type_wait(const char *datapath_type)
{
    const struct p4rt_class *class;

    datapath_type = dpif_normalize_type(datapath_type);
    class = p4rt_class_find__(datapath_type);

    if (class->type_wait) {
        class->type_wait(datapath_type);
    }
}

void
p4rt_get_ports(struct p4rt *p, struct sset *p4rt_ports)
{
    struct p4port *port, *next;

    HMAP_FOR_EACH_SAFE (port, next, hmap_node, &p->ports) {
        sset_add(p4rt_ports, netdev_get_name(port->netdev));
    }
}

struct p4port *
p4rt_get_port(const struct p4rt *p4rt, ofp_port_t port_no)
{
    struct p4port *port;

    HMAP_FOR_EACH_IN_BUCKET (port, hmap_node, hash_ofp_port(port_no),
                             &p4rt->ports) {
        if (port->port_no == port_no) {
            return port;
        }
    }

    return NULL;
}

int
p4rt_port_add(struct p4rt *p, struct netdev *netdev, ofp_port_t *ofp_portp)
{
    ofp_port_t ofp_port = ofp_portp ? *ofp_portp : OFPP_NONE;
    int error;

    error = p->p4rt_class->port_add(p, netdev, ofp_to_u16(ofp_port));
    if (!error) {
        const char *netdev_name = netdev_get_name(netdev);
        error = update_port(p, netdev_name);
    }

    if (ofp_portp) {
        *ofp_portp = OFPP_NONE;
        if (!error) {
            struct p4rt_port p4rt_port;

            error = p4rt_port_query_by_name(p,
                                            netdev_get_name(netdev),
                                            &p4rt_port);

            if (!error) {
                *ofp_portp = p4rt_port.port_no;
                p4rt_port_destroy(&p4rt_port);
            }
        }
    }

    return error;
}

int
p4rt_port_del(struct p4rt *p, const char *name)
{
    struct p4port *p4port = p4rt_get_port_by_name(p, name);
    int error;

    if (!p4port) {
        return ENODEV;
    }

    error = p->p4rt_class->port_del(p, ofp_to_u16(p4port->port_no));

    if (!error && p4port) {
        /* 'name' is the netdev's name and update_port() is going to close the
         * netdev.  Just in case update_port() refers to 'name' after it
         * destroys 'p4port', make a copy of it around the update_port()
         * call. */
        char *devname = xstrdup(name);
        update_port(p, devname);
        free(devname);
    }

    return error;
}

int
p4rt_prog_del(struct p4rt *p)
{
    struct program *prog = p->prog;
    p4rt_program_destroy(prog);
    return 0;
}

/* ## ------------- ## */
/* ## PI functions. ## */
/* ## ------------- ## */


pi_status_t _pi_assign_device(pi_dev_id_t dev_id, const pi_p4info_t *p4info,
                              pi_assign_extra_t *extra OVS_UNUSED) {
    VLOG_INFO("Assigning device: %lu", dev_id);

    struct p4rt *p4rt = p4rt_lookup_by_dev_id(dev_id);

    if (!p4rt) {
        /* P4 Device does not exist. */
        return PI_STATUS_DEV_NOT_ASSIGNED;
    }

    ovs_mutex_lock(&p4rt_mutex);
    p4rt->p4info = CONST_CAST(pi_p4info_t *, p4info);
    ovs_mutex_unlock(&p4rt_mutex);

    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_update_device_start(pi_dev_id_t dev_id,
                                    const pi_p4info_t *p4info OVS_UNUSED,
                                    const char *device_data,
                                    size_t device_data_size) {
    VLOG_INFO("Injecting config (size %lu) %s", device_data_size, device_data);
    int error;

    struct p4rt *p4rt = p4rt_lookup_by_dev_id(dev_id);
    if (!p4rt) {
        /* P4 Device does not exist. */
        return PI_STATUS_DEV_OUT_OF_RANGE;
    }
    struct program *prog;
    if (!p4rt->prog) {
        prog = p4rt->p4rt_class->program_alloc();
        if (!prog) {
            error = ENOMEM;
            goto error;
        }
    } else {
        prog = p4rt->prog;
    }

    *CONST_CAST(struct p4rt **, &prog->p4rt) = p4rt;
    prog->data = CONST_CAST(char *, device_data);
    prog->data_len = device_data_size;

    error = p4rt->p4rt_class->program_insert(prog);
    if (error) {
        goto error;
    }

    p4rt->prog = prog;

    VLOG_INFO("P4 datapath initialized!");

    return PI_STATUS_SUCCESS;

error:
    VLOG_WARN_RL(&rl, "failed to initialize P4 datapath of device %lu (%s)",
                 dev_id, ovs_strerror(error));
    if (!p4rt->prog && prog) {
        p4rt->p4rt_class->prog_dealloc(prog);
    }

    return PI_STATUS_TARGET_ERROR;
}

/* ## ------------------------------- ## */
/* ## Functions exposed to ovs-p4ctl. ## */
/* ## ------------------------------- ## */

static struct p4rt *
p4rt_lookup(const char *name)
{
    struct p4rt *p4rt;

    HMAP_FOR_EACH_WITH_HASH (p4rt, hmap_node, hash_string(name, 0),
                             &all_p4rts) {
        if (!strcmp(p4rt->name, name)) {
            return p4rt;
        }
    }
    return NULL;
}

int
p4rt_query_switch_features(const char *name, struct p4rt_switch_features *features)
{
    struct p4rt *p4rt = p4rt_lookup(name);
    if (!p4rt) {
        return ENODEV;
    }

    features->n_tables = 0; /* TODO: query no of tables from datapath or save it while inserting new program */
    features->n_ports = hmap_count(&p4rt->ports);

    return 0;
}