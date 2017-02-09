#include "rum.h"

extern struct event_base *event_base;

extern struct destination *first_destination;

extern int connect_timeout;
extern int read_timeout;

int
pg_handle_init_packet_from_client (struct bev_arg *bev_arg,
                                const uv_buf_t *uv_buf, size_t nread)
{
    char user[64];
    char buf[512];
    char buf1[512];
    char buf2[512];
    char buf3[512];
    char buf4[512];
    int user_len, buflen, buflen_htonl, buf1len, buf2len, buf3len, buf4len;
    struct destination *destination = NULL, *dst;
    char *pg_server = NULL, *userptr;
    struct bev_arg *bev_arg_remote;

    if (nread < 2*sizeof(int) + sizeof("user")) {
        /* check if it is SSLRequest */
        if (nread == 8) {
            char bufx[8];
            char *ptr=bufx;
            int *a,*b;

            memcpy(bufx, uv_buf->base, nread);

            a=(int *)ptr;
            b=(int *)(ptr+sizeof(int));

            if (ntohl(*a) == 8 && ntohl(*b) == 80877103) {
                /* send client that we dont support SSL */
                uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
                uv_buf_t *newbuf = malloc(sizeof(uv_buf_t));
                newbuf->base = malloc (1);
                newbuf->base[0]='N';
                newbuf->len=1;
                req->data = newbuf;
                if (uv_write(req, bev_arg->stream, newbuf, 1, on_write)) {
                    uv_shutdown_t *shutdown = malloc(sizeof(uv_shutdown_t));
                    if (uv_shutdown(shutdown, bev_arg->stream, on_shutdown)) {
                        free(shutdown);
                        uv_close((uv_handle_t *)bev_arg->stream, on_close);
                    }
                }

                return 1;
            }
        }

        bev_arg->listener->nr_conn--;

        free_ms (bev_arg->ms);
        bev_arg->ms = NULL;

        if (bev_arg->remote) {
            bev_arg->remote->ms = NULL;
            free (bev_arg->remote);
        }

        free (bev_arg);

        return 1;
    }

    bev_arg->ms->client_auth_packet_len = nread;
    bev_arg->ms->client_auth_packet = malloc(nread);
    memcpy(bev_arg->ms->client_auth_packet, uv_buf->base, nread);

    userptr =
        bev_arg->ms->client_auth_packet + 2 * sizeof(int) +
        sizeof("user");
    user_len = strnlen (userptr, nread - 2*sizeof(int) - sizeof("user"));
    if (user_len > sizeof(user)-1) {
        uv_shutdown_t *shutdown = malloc(sizeof(uv_shutdown_t));
        if (uv_shutdown(shutdown, bev_arg->stream, on_shutdown)) {
            free(shutdown);
            uv_close((uv_handle_t *)bev_arg->stream, on_close);
        }
        return 1;
    }
    strncpy (user,
             bev_arg->ms->client_auth_packet + 2 * sizeof(int) +
             sizeof("user"), user_len);
    user[user_len] = '\0';

    get_data_from_cdb_postgresql (user, user_len, &pg_server);

    if (pg_server != NULL) {
        if (first_destination) {
            for (dst = first_destination; dst->next; dst = dst->next) {
                if (!strcmp (dst->s, pg_server)) {
                    destination = dst;
                    break;
                }
            }

            if (!destination) {
                dst->next = destination = malloc (sizeof (struct destination));
                prepareclient (pg_server, destination);
            }
        } else {
            first_destination = destination = malloc (sizeof (struct destination));
            prepareclient (pg_server, destination);
        }
    } else {
        /* if user is not found in cdb we use mysql server set with -d argument
         * but connection will not be successful, we need user encrypted password which should be in cdb file
         */
        destination = first_destination;

        logmsg("user %s not found in cdb", user);
        /* we reply access denied  */
        //memcpy (buf, ERR_LOGIN_PACKET_PREFIX, sizeof(ERR_LOGIN_PACKET_PREFIX));

        memset(buf, '\0', sizeof(buf));
        buf[0]='E';
        buf1len = snprintf (buf1, sizeof(buf1), "SFATAL");
        buf2len = snprintf (buf2, sizeof(buf2), "C28P01");
        buf3len = snprintf (buf3, sizeof(buf3), "MUser \"%s\" not found", user);
        buf4len = snprintf (buf4, sizeof(buf4), "Rauth_failed");
        buflen = 1 + 4 + buf1len + 1 + buf2len + 1 + buf3len + 1 + buf4len + 1 + 1;
        buflen_htonl = htonl(buflen - 1);
        memcpy (buf + 1, &buflen_htonl, sizeof(buflen_htonl));
        memcpy (buf + 1 + 4, buf1, buf1len);
        memcpy (buf + 1 + 4 + buf1len + 1, buf2, buf2len);
        memcpy (buf + 1 + 4 + buf1len + 1 + buf2len + 1, buf3, buf3len);
        memcpy (buf + 1 + 4 + buf1len + 1 + buf2len + 1 + buf3len + 1, buf4, buf4len);
        //bufferevent_write (bev, buf, buflen);
        // TODO

        /* enable write_callback so we close connection in case client doesn't */
/*
        bufferevent_setcb (bev, postgresql_read_callback, postgresql_write_callback,
                           postgresql_event_callback, (void *) bev_arg);
*/

        if (pg_server)
            free (pg_server);

        return 1;
    }

    /* if remote connection exists free it */
    if (bev_arg->remote) {
//        bufferevent_free (bev_arg->remote->bev);
        free (bev_arg->remote);
    }

    if (!destination) {
        if (pg_server)
            free (pg_server);

        uv_shutdown_t *shutdown = malloc(sizeof(uv_shutdown_t));
        if (uv_shutdown(shutdown, bev_arg->stream, on_shutdown)) {
            free(shutdown);
            uv_close((uv_handle_t *)bev_arg->stream, on_close);
        }

        return 1;
    }


    bev_arg_remote = create_server_connection(bev_arg, destination, bev_arg->listener);
    bev_arg->ms->not_need_remote = 0;
    bev_arg_remote->ms = bev_arg->ms;
    bev_arg_remote->listener = bev_arg->listener;
    bev_arg->ms->handshake = 2;

    if (pg_server)
        free (pg_server);

    return 1;
}
