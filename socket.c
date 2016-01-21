#include "rum.h"

extern struct event_base *event_base;
extern struct destination *first_destination;
extern char *mysql_cdb_file;
extern char *postgresql_cdb_file;

extern char *cache_mysql_init_packet;
extern int cache_mysql_init_packet_len;
extern char *cache_mysql_init_packet_scramble;

int logfd;

/*
 * create_listen_socket return O_NONBLOCK socket ready for accept()
 * arg - tcp:blah:blah alebo sock:blah
 */
int
create_listen_socket (char *arg)
{
    int sock, sockopt;
    char *arg_copy;
    struct sockaddr *s = NULL;
    struct sockaddr_in sin;
    struct sockaddr_un sun;
    socklen_t socklen;
    uint16_t port;
    char type;
    int domain;
    char *host_str, *port_str, *sockfile_str;
    int i,ok;

    arg_copy = strdup (arg);
    /* parse string arg_copy into variables
     * arg_copy is modified
     */
    parse_arg (arg_copy, &type, &sin, &sun, &socklen, &port, &host_str,
               &port_str, &sockfile_str, 1);

    if (type == SOCKET_TCP) {
        s = (struct sockaddr *) &sin;
        domain = PF_INET;
    } else if (type == SOCKET_UNIX) {
        s = (struct sockaddr *) &sun;
        domain = PF_UNIX;
    } else {
        usage ();
        _exit (-1);
    }

    if ((sock = socket (domain, SOCK_STREAM, 0)) == -1) {
        perror ("socket");
        _exit (-1);
    }

    sockopt = 1;
    setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof (sockopt));

    for (i=0,ok=0;i<25;i++) {
        if (bind (sock, s, socklen) == -1) {
            /* if cannot bind sleep 200ms and retry 25x */
            fprintf(stderr,"bind() to %s failed\n", arg);
            usleep(200*1000);
        } else {
            ok=1;
            break;
        }
    }
    if (ok==0) {
        fprintf(stderr,"bind() to %s failed, exiting\n", arg);
        _exit (-1);
    }

    for (i=0,ok=0;i<20;i++) {
        if (listen (sock, 255) == -1) {
            /* if cannot bind sleep 200ms and retry 20x */
            fprintf(stderr,"listen() to %s failed", arg);
            usleep(200*1000);
        } else {
            ok=1;
            break;
        }
    }
    if (ok==0) {
        fprintf(stderr,"bind() to %s failed, exiting", arg);
        _exit (-1);
    }

    fcntl (sock, F_SETFL, O_RDWR | O_NONBLOCK);

    if (type == SOCKET_TCP) {
        printf ("listening on tcp:%s:%s\n", host_str, port_str);
    } else if (type == SOCKET_UNIX) {
        chmod (sockfile_str, 0777);
        printf ("listening on sock:%s\n", sockfile_str);
    } else {
        usage ();
        _exit (-1);
    }

    return sock;
}

/* fill destination->sin or destination->sun and destination->socklen
 */
void
prepareclient (char *arg, struct destination *destination)
{
    char *arg_copy;
    uint16_t port;
    char *host_str, *port_str, *sockfile_str;
    char type;

    arg_copy = strdup (arg);

    destination->s = strdup (arg);
    destination->next = NULL;

    parse_arg (arg_copy, &type, &destination->sin, &destination->sun,
               &destination->addrlen, &port, &host_str, &port_str,
               &sockfile_str, 0);
    free (arg_copy);
}

/*
 * accept socket, then create buffer events for self and remote
 */
void
accept_connect (int sock, short event, void *arg)
{
    struct listener *listener = (struct listener *) arg;
    socklen_t len;
    struct sockaddr_in sin;
    struct sockaddr_un sun;
    int csock = 0;
    struct bufferevent *bev_client, *bev_target;
    struct bev_arg *bev_arg_client, *bev_arg_target;

    struct destination *destination = first_destination;
    struct sockaddr *s = NULL;

    if (listener->s[0] == SOCKET_TCP) {
        len = sizeof (struct sockaddr_in);
        s = (struct sockaddr *) &sin;
    } else if (listener->s[0] == SOCKET_UNIX) {
        len = sizeof (struct sockaddr_un);
        s = (struct sockaddr *) &sun;
    }

    csock = accept4 (sock, s, &len, SOCK_NONBLOCK);

    if (csock == -1) {
        return;
    }

    listener->nr_allconn++;

    /* the first turn the switch on */
    /* the last turn the switch off */
    listener->nr_conn++;

    bev_client =
        bufferevent_socket_new (event_base, csock, BEV_OPT_CLOSE_ON_FREE);
    bev_target =
        bufferevent_socket_new (event_base, -1, BEV_OPT_CLOSE_ON_FREE);

    /* CLIENT bev_arg */
    /* parameter for callback functions */
    bev_arg_client = malloc (sizeof (struct bev_arg));
    bev_arg_client->type = BEV_CLIENT;
    bev_arg_client->listener = listener;
    bev_arg_client->bev = bev_client;
    bev_arg_client->connecting = 0;
    bev_arg_client->connect_timer = NULL;
    bev_arg_client->read_timeout = 0;
    bev_arg_client->destination = NULL;

    /* set callback functions and argument */
    if (listener->type == LISTENER_DEFAULT) {
        if (!mysql_cdb_file && !postgresql_cdb_file) {
            bufferevent_setcb (bev_client, read_callback, NULL, event_callback,
                               (void *) bev_arg_client);
        } else if (mysql_cdb_file) {
            /* if mysql_cdb is enabled, use different callback functions */
            bev_arg_client->ms = init_ms ();
            bufferevent_setcb (bev_client, mysql_read_callback, NULL,
                               mysql_event_callback, (void *) bev_arg_client);
        } else if (postgresql_cdb_file) {
            /* if postgresql_cdb is enabled, use different callback functions */
            bev_arg_client->ms = init_ms ();
            bufferevent_setcb (bev_client, postgresql_read_callback, NULL,
                               postgresql_event_callback, (void *) bev_arg_client);

        }
    } else if (listener->type == LISTENER_STATS) {
        bufferevent_setcb (bev_client, NULL, stats_write_callback,
                           stats_event_callback, (void *) bev_arg_client);
        send_stats_to_client (bev_client);

        return;
    }
    /* read buffer 64kb */
    bufferevent_setwatermark (bev_client, EV_READ, 0, INPUT_BUFFER_LIMIT);

    /* TARGET bev_arg */
    /* parameter for callback functions */
    bev_arg_target = malloc (sizeof (struct bev_arg));
    bev_arg_target->type = BEV_TARGET;
    bev_arg_target->connecting = 0;

    bev_arg_client->remote = bev_arg_target;

    /* set callback functions and argument */
    bev_arg_target->listener = listener;
    bev_arg_target->bev = bev_target;
    bev_arg_target->remote = bev_arg_client;

    bev_arg_target->connect_timer = NULL;
    bev_arg_target->read_timeout = 0;


    if (!mysql_cdb_file && !postgresql_cdb_file) {
        bufferevent_setcb (bev_target, read_callback, NULL, event_callback,
                           (void *) bev_arg_target);
    } else if (mysql_cdb_file) {
        /* mysql_stuff structure is same for client and target bufferevent */
        bev_arg_target->ms = bev_arg_client->ms;

        bufferevent_setcb (bev_target, mysql_read_callback, NULL,
                           mysql_event_callback, (void *) bev_arg_target);
    } else if (postgresql_cdb_file) {
        /* mysql_stuff structure is same for client and target bufferevent */
        bev_arg_target->ms = bev_arg_client->ms;

        bufferevent_setcb (bev_target, postgresql_read_callback, NULL,
                           postgresql_event_callback, (void *) bev_arg_target);

    }
    /* read buffer 64kb */
    bufferevent_setwatermark (bev_target, EV_READ, 0, INPUT_BUFFER_LIMIT);

    /* we can use cached init packet only if we can use MITM attack,
     * we can use MITM attack only if we use mysql_cdb_file where are hashed user passwords
     */
    if (!postgresql_cdb_file && (!mysql_cdb_file || (mysql_cdb_file && !cache_mysql_init_packet))) {
        if (destination->s[0] == SOCKET_TCP) {
            s = (struct sockaddr *) &destination->sin;
            len = destination->addrlen;
        } else {
            s = (struct sockaddr *) &destination->sun;
            len = destination->addrlen;
        }

        bev_arg_target->connecting = 1;
        bev_arg_target->destination = destination;

        if (bufferevent_socket_connect (bev_target, s, len) == -1) {
            logmsg ("bufferevent_socket_connect return -1 (full fd?)\n");
            listener->nr_conn--;
            bufferevent_free (bev_client);
            bufferevent_free (bev_target);
            free (bev_arg_client);
            free (bev_arg_target);

            return;
        }
        bev_arg_target->connecting = 0;
        struct linger l;
        int flag = 1;

        l.l_onoff = 1;
        l.l_linger = 0;

        setsockopt (bufferevent_getfd (bev_target), SOL_SOCKET, SO_LINGER,
                    (void *) &l, sizeof (l));
        setsockopt (bufferevent_getfd (bev_target), IPPROTO_TCP, TCP_NODELAY,
                    (char *) &flag, sizeof (int));

        /* connect timeout timer */
        struct timeval time;
        time.tv_sec = CONNECT_TIMEOUT;
        time.tv_usec = 0;

        bev_arg_target->connect_timer =
            event_new (event_base, -1, 0, connect_timeout_cb, bev_arg_target);
        if (bev_arg_target->connect_timer) {
            event_add (bev_arg_target->connect_timer, &time);
        }
    } else if (postgresql_cdb_file) {
        bev_arg_client->remote = NULL;
        bev_arg_client->ms->not_need_remote = 1;
        bev_arg_client->ms->handshake = 1;
        bufferevent_free (bev_target);
        free (bev_arg_target);
        bufferevent_enable (bev_client, EV_READ);
    } else {
        /* use cached init packet */
        bev_arg_client->remote = NULL;
        bev_arg_client->ms->not_need_remote = 1;
        bev_arg_client->ms->handshake = 1;
        /* we use bev_arg_client and ms pointers as random data for generating random string filled in init packet send to client */
        /* TODO: use better random input */
        bev_arg_client->ms->scramble1 =
            set_random_scramble_on_init_packet (cache_mysql_init_packet,
                                                bev_arg_target,
                                                bev_arg_client->ms);

        bufferevent_free (bev_target);
        free (bev_arg_target);

        if (bufferevent_write
            (bev_client, cache_mysql_init_packet,
             cache_mysql_init_packet_len) == -1) {
            listener->nr_conn--;
            bufferevent_free (bev_client);
            bufferevent_free (bev_target);
            free (bev_arg_client);
            free (bev_arg_target);
        }

        bufferevent_enable (bev_client, EV_READ);
    }
}

void
cache_init_packet_from_server ()
{
    socklen_t len;
    struct bufferevent *bev;
    struct bev_arg *bev_arg;

    struct destination *destination = first_destination;
    struct sockaddr *s;

    bev = bufferevent_socket_new (event_base, -1, BEV_OPT_CLOSE_ON_FREE);

    bev_arg = malloc (sizeof (struct bev_arg));

    bev_arg->type = BEV_CACHE;
    bev_arg->bev = bev;
    bev_arg->remote = NULL;
    bev_arg->connect_timer = NULL;
    bev_arg->read_timeout = 0;
    bev_arg->destination = NULL;

    /* set callback functions and argument */
    bufferevent_setcb (bev, cache_mysql_init_packet_read_callback, NULL,
                       cache_mysql_init_packet_event_callback,
                       (void *) bev_arg);

    /* read buffer 64kb */
    bufferevent_setwatermark (bev, EV_READ, 0, INPUT_BUFFER_LIMIT);

    if (destination->s[0] == SOCKET_TCP) {
        s = (struct sockaddr *) &destination->sin;
        len = destination->addrlen;
    } else {
        s = (struct sockaddr *) &destination->sun;
        len = destination->addrlen;
    }

    /* event_callback() will be called after nonblock connect() return 
     */
    if (bufferevent_socket_connect (bev, s, len) == -1) {
        logmsg ("bufferevent_socket_connect return -1 (full fd?)\n");
        bufferevent_free (bev);
        free (bev_arg);
        return;
    }

    struct linger l;

    l.l_onoff = 1;
    l.l_linger = 0;

    setsockopt (bufferevent_getfd (bev), SOL_SOCKET, SO_LINGER, (void *) &l,
                sizeof (l));

    /* connect timeout timer */
    struct timeval time;
    time.tv_sec = CONNECT_TIMEOUT;
    time.tv_usec = 0;

    bev_arg->connect_timer =
        event_new (event_base, -1, 0, connect_timeout_cb, bev_arg);
    if (bev_arg->connect_timer) {
        event_add (bev_arg->connect_timer, &time);
    }
}

void
log_cb ()
{
}
